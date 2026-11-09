#include "dafeiyu_display.h"

#include "dafeiyu_storage.h"
#include "dafeiyu_web.h"

#include "assets/lang_config.h"
#include "display/lvgl_display/emoji_collection.h"
#include "display/lvgl_display/lvgl_image.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_system.h>
#include "board.h"   // UpdateStatusBar 里用 Board::GetInstance().GetBatteryLevel() 拿电量
#include "board.h"   // Board::GetInstance().GetBatteryLevel()：状态栏取电量用
#include "audio/audio_codec.h"   // 菜单里的音量项要读写 output_volume()
#include "esp_attr.h"   // RTC_NOINIT_ATTR：崩溃标记要放 RTC 内存（掉电才丢）
#include "freertos/task.h"   // uxTaskGetStackHighWaterMark()：测 LVGL 任务栈水位用
#include "esp_heap_caps.h"   // heap_caps_malloc：呼吸球画布缓冲要放 PSRAM
#include <math.h>            // sinf/cosf/sqrtf：呼吸球逐像素渲染用
#include "esp_timer.h"       // esp_timer_get_time：呼吸球渲染耗时测量
#include "orb_lvgl.h"        // xiaozhi-orb：整数呼吸球 + LVGL canvas 后端
#include "orb_audio.h"       // orb_peek_audio_global：取音频任务算好的真实电平
#include "spec_fft.h"        // 2026-11-06 音乐频谱：输出 PCM 环形缓冲 + FFT 16 柱
#include "qrcodegen.h"       // 2026-11-05 控制页二维码：复用 esp_emote_gfx 的 qrcodegen
#include "esp_heap_caps.h"   // canvas 缓冲从 PSRAM 分配
#include "pet_themes.h"      // 大肥鱼待机主题引擎（_themes/，项目根目录下）
#include "pet_emotes.h"      // 角色表情动画（_emotes/）
#include "pet_idle_plan.h"   // 待机行为编排（定时随机挑动作）
#include "settings.h"        // 主题记忆：NVS 存上次切换的主题 id
#include <esp_wifi.h>   // esp_wifi_sta_get_ap_info()：状态栏取 WiFi 信号强度用
#include <esp_timer.h>
#include <lvgl.h>
#include <wifi_manager.h>

#include <algorithm>

#include <cmath>
#include <cstring>

/* 主题装饰容器显隐（实现在 _themes/pet_themes.c，纯 C）。
   ApplyThemeTo / SetWakeIndicator 在文件前部就用，声明必须放这里。 */
extern "C" void pet_theme_show(int on);
#include <ctime>
#include <strings.h>

// 时钟用的大字号（sdkconfig 里开了 CONFIG_LV_FONT_MONTSERRAT_48）
LV_FONT_DECLARE(lv_font_montserrat_48);
// 数值标签用中文字体：montserrat 只有拉丁字符，写汉字会渲染成空白。
// 这份 basic 是补过 UI 汉字的版本（见 _build_font.py），含「饱食心情亲密等级经验」。
LV_FONT_DECLARE(font_noto_sans_basic_14_1);
// 主题引擎的文本字体（日期/文案）：本板的 BUILTIN_TEXT_FONT，含汉字。
// 之前只在 zhengchen_cam_board.cc 里声明过，这里要用必须再声明一次。
LV_FONT_DECLARE(font_noto_sans_basic_20_4);

/* ── 角色位置修正 + 主题轮播（调试用） ─────────────────────────────── */
static constexpr lv_coord_t kPetLiftUp     = 48;   // 往上（越大越高）
static constexpr lv_coord_t kPetShiftRight = 24;   // 往右（越大越右）
/* ⚠️ 2026-11-01 暂时关掉轮播（改回 0）。
   开着轮播时机器会不断重启、且角色被背景盖住 —— 原因是我的轮播代码里
   每次切换都调了 pet_emotes_init()，而它是「初始化」函数、只该调一次，
   反复调用会累积动画/定时器；而且 s_pet 是复用的（pet_themes.c:875
   if (!s_pet) 才创建），切换时并不需要重新 init。
   要恢复轮播必须先修好那处，不能只是把这个数改回来。 */
static constexpr uint32_t   kThemeCycleMs  = 0;    // 主题轮播间隔；0 = 关闭轮播

static uint32_t s_theme_cycle_ms = 0;

/* 动作自动变换开关：默认 false = 固定在当前主题配套动作。
   用户要求「切到下雨/哭就一直是哭，过一会不要变成吃饭/默认」。 */
bool DafeiyuDisplay::s_auto_emote_ = false;

/* ── 崩溃现场标记（2026-11-01）─────────────────────────────────────────────
   背景：双击切主题会 PANIC 崩溃，但「一插上串口就不太崩」—— 串口日志的耗时
   恰好把竞态掩盖了，导致现场抓不到。而内核转存也用不了：工程是自定义分区表
   partitions/v2/16m.csv，里面没有 coredump 分区，加分区风险太大。
   办法：RTC 内存掉电才丢、panic 重启后仍然保留，所以把「执行到切主题的哪一步」
   写进去，重启后从网页 /api/state 的 "panic" 字段读出来，完全不依赖串口。
   步骤号含义见 ApplyThemeSwitch()。 */
#define PANIC_TRACE_MAGIC 0x50414E49u   /* "PANI"，用来判断 RTC 内容是否有效 */
struct PanicTrace {
    uint32_t magic;
    uint32_t step;    // 0 = 正常走完；非 0 = 崩在这一步
    uint32_t count;   // 累计切换次数
    uint32_t rsv;
};
static RTC_NOINIT_ATTR PanicTrace s_panic_trace;
static char s_panic_desc[64] = "无";

static void PanicMark(uint32_t step) {
    s_panic_trace.magic = PANIC_TRACE_MAGIC;
    s_panic_trace.step = step;
}

/* 给主题引擎（_themes/pet_themes.c，纯 C）用的分步标记入口。
   这样崩溃可以细分到 pet_theme_start() 内部的具体段落：
   21~25 = 函数内各阶段，见 pet_themes.c。 */
extern "C" void dafeiyu_panic_mark(uint32_t step) {
    PanicMark(step);
}

/* ⚠️ pet_theme_start() 每次调用都会重建角色对象（新的 lv_obj），
   所以下面这些修正必须在「初次创建」和「每次切主题后」都重做一遍，
   否则新对象会退回被裁脚 / 偏左 / 被遮挡的状态：
     · 上移：主题给的 y 让角色底边超出屏幕，脚被切
     · 右移：偏左
     · 搬到 top layer：screen 内任何对象都盖不到它（详见下方注释） */
static void ApplyPetFix(lv_obj_t *pet)
{
    if (pet == nullptr) return;
    lv_obj_set_y(pet, lv_obj_get_y(pet) - kPetLiftUp);
    lv_obj_set_x(pet, lv_obj_get_x(pet) + kPetShiftRight);
    /* ⚠️ 2026-11-01：不再搬到 lv_layer_top()，改为留在 screen 里提前景。
       原委：搬到 top layer 是为了躲遮挡，但 lv_obj_set_parent() 会在 LVGL 的
       事件链表里留下条目，导致 pet_theme_start() 里第 822 行的
       lv_obj_del(s_cont) 走到 lv_event_mark_deleted() 时解引用野指针 ——
       实测「双击切主题 PANIC 崩溃」，RTC 崩溃标记显示正是崩在
       pet_theme_start() 内部（第 2 步 → 第 3 步之间）。
       当初那些遮挡物现在已经都不挡了：壁纸 wallpaper_image_ 被
       ApplyConfigIfChanged() 拦住，旧立绘 pet_image_ 永久隐藏，
       主题自己的背景容器也会 move_background() 到底层。
       所以留在 screen、每帧 move_foreground 就够了。 */
    lv_obj_move_foreground(pet);
}

/* ── 主题记忆（2026-11-02 用户要求：切完主题断电，开机还是这套）──
   ⚠️ 单独开一个 NVS 命名空间，绝不动 UiConfig —— 那个结构体只要长度一变，
   整份界面配置（背景色/时钟/壁纸/唤醒动画…）就会被判为旧版本丢弃，
   之前已经因为加字段把它清空过一次。主题 id 是独立小数据，单独存最安全。 */
static const char* kThemeNs  = "pet_theme";
static const char* kThemeKey = "id";

/* ── 翻页时钟样式表（2026-11-06 用户要求多样式 + 双击音量键切换）──
   四套：0 极光（原版）/ 1 暖橙 / 2 青翠 / 3 极简（纯黑白、无卡框）。
   样式只换配色和日期显隐，翻页动画、几何全部共用 —— 改动面最小的做法。
   ⚠️ 必须放在 ApplyThemeSwitch 之前：那几个 Flip* 绘制函数和分流逻辑
   都要用它，放后面会报 not declared。 */
struct FcStyle {
    uint8_t bg[3];            /* 整屏底色 */
    uint8_t cardBg[3];        /* 卡底色 */
    uint8_t digit[4][3];      /* 四卡数字色 */
    uint8_t colon[3];         /* 冒号色 */
    uint32_t dateCol;         /* 日期 label 颜色 (0xRRGGBB) */
    bool showDate;
};
static const FcStyle kFcStyles[] = {
    /* 0 极光（原版） */
    {{5, 7, 12}, {10, 16, 30},
     {{85, 193, 255}, {104, 124, 255}, {154, 86, 246}, {222, 80, 169}},
     {170, 200, 255}, 0xC8D2E0, true},
    /* 1 暖橙（夜灯感：深棕底 + 炉火橙） */
    {{18, 10, 6}, {34, 20, 11},
     {{255, 168, 84}, {255, 122, 88}, {255, 96, 120}, {255, 156, 72}},
     {255, 196, 128}, 0xE8C8A8, true},
    /* 2 青翠（深绿底 + 荧光绿） */
    {{4, 14, 10}, {9, 28, 19},
     {{80, 255, 170}, {96, 232, 200}, {128, 255, 150}, {176, 255, 96}},
     {150, 255, 200}, 0xA8D8C0, true},
    /* 3 极简（纯黑白配色。2026-11-06 用户要求加回卡片：只换配色不动结构） */
    {{0, 0, 0}, {22, 24, 28},
     {{238, 240, 244}, {238, 240, 244}, {238, 240, 244}, {238, 240, 244}},
     {210, 214, 220}, 0xB8C0C8, true},
};
static constexpr int kFcStyleN = (int)(sizeof(kFcStyles) / sizeof(kFcStyles[0]));
static int s_fcStyleIdx = 0;   /* FlipPaintCard/Colon 是文件内 static 函数，直接用全局指针最省事 */

static pet_theme_id_t LoadSavedTheme()
{
    Settings s(kThemeNs, false);
    int id = s.GetInt(kThemeKey, (int)PET_THEME_IDLE);
    /* 2026-11-06 放行扩展主题 id（100=翻页时钟 101=自定义壁纸）：
       它们和大肥鱼系列共用同一个 NVS 键，断电记忆要一并生效。 */
    if (id < 0 || (id >= (int)PET_THEME_N && id != 100 && id != 101)) {
        id = (int)PET_THEME_IDLE;      /* 存档损坏/越界就回默认，别让开机失败 */
    }
    return (pet_theme_id_t)id;
}

static void SaveCurrentTheme(pet_theme_id_t id)
{
    Settings s(kThemeNs, true);
    s.SetInt(kThemeKey, (int)id);
    /* 这里用字面量而不是 TAG：本函数在 #define TAG 之前，编译器看不到那个宏。
       pet_theme_name() 内部按 0..8 查表，扩展 id 直接传会越界，要分开打。 */
    if ((int)id < (int)PET_THEME_N) {
        ESP_LOGI("DafeiyuDisplay", "主题已记忆 -> %d %s", (int)id, pet_theme_name(id));
    } else {
        ESP_LOGI("DafeiyuDisplay", "主题已记忆 -> %d (扩展主题)", (int)id);
    }
}
LV_FONT_DECLARE(lv_font_montserrat_14);

#define TAG "DafeiyuDisplay"

using namespace dafeiyu;

namespace {
// 屏幕背景色（角色 GIF 自带 1bit 透明，透出来就是这个色）
// 换色只改这里；后面接了网页配置后由壁纸/色值覆盖
constexpr uint32_t kScreenBgColor = 0x191F2E;   // 深夜蓝，衬蓝色角色
constexpr uint32_t kClockColor = 0xE8ECF5;
constexpr uint32_t kClockDateColor = 0x8A93A8;
constexpr uint32_t kTickIntervalMs = 120;
// 聆听时等这么久还没开始说话，就切到「思考」（问号）——避免一直举着沙漏
constexpr uint32_t kListenThinkDelayMs = 2600;
// LVGL 刷新周期（ms）。待机 30fps 够顺；说话时拉到 ~10fps。
// 为什么这么做能救音频：这台设备内部 SRAM 最小只剩几百字节，LVGL 的双缓冲根本
// 分配不出来（见 lcd_display.cc），而 LVGL 任务优先级已经是 1，也谈不上抢占。
// 唯一能腾出来的就是「每秒刷多少次屏」——每帧 153KB 的 SPI + PSRAM 流量，
// 说话时砍到三分之一，剩下的带宽留给解码和 I2S 的 DMA。
// 屏幕分区：左边一条竖留给信息层，右边整块给角色。
// 素材是事先在电脑上缩好的 150x171（见 _resize_assets.py），运行时按原始尺寸画 ——
// LVGL 的运行时缩放要软件重采样，这台设备撑不住。320 - 150 = 170，再扣掉边距。
constexpr lv_coord_t kInfoPanelW = 158;
constexpr lv_coord_t kPanelMargin = 6;

// 说话降到 10fps 试过：画面明显一格一格「细碎」，用户对这点很敏感。
// 20fps 是平衡值 —— 既省掉了三分之一带宽，又不至于看出跳帧。
constexpr uint32_t kRefrIdleMs = 33;   // ~30fps
constexpr uint32_t kRefrSpeakMs = 50;  // ~20fps（TTS 播放中）

// 待机随机小动作间隔。
// 两条设计约束（照抄 whale-girl 的「随机插曲」节奏并保证activity-independent）：
//   1. 间隔够短：25~45s 的版本实测「看几分钟才动一下」，这是待机呆板的根源
//   2. 间隔够长：不能是环境activity的指示灯——它应该是无理由的自主行为，
//      12~30s 这个量级既显得活泼又不会被读成「有事发生」
constexpr uint32_t kMicroMinMs = 12000;
constexpr uint32_t kMicroMaxMs = 30000;

// 数值阈值：低于多少算「有诉求」，高于多少算「状态好」
constexpr uint8_t kNeedHunger = 35;  // 饱食度低于此 → 想吃
constexpr uint8_t kNeedMood = 35;    // 心情低于此 → 困 / 无聊
constexpr uint8_t kCloseBond = 70;   // 亲密度高于此 → 撒娇求摸
constexpr uint8_t kGreatMood = 75;   // 心情高于此且吃得饱 → 得意

// 情绪反应冷却：服务端每段回复都会带 emotion，不冷却的话一句话插好几次
constexpr uint32_t kReactionCooldownMs = 15000;

const char* const kPositiveEmotions[] = {
    "happy", "laughing", "funny", "silly", "winking", "kissy",
    "loving", "delicious", "relaxed", "cool", "confident",
};
const char* const kNegativeEmotions[] = {
    "sad", "crying", "angry", "embarrassed",
};
}  // namespace

DafeiyuDisplay::DafeiyuDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {
    instance_ = this;
    // ⚠️ 2026-10-31：为追「语音唤醒就重启」加的诊断。
    // 我一度把它判成服务端远程调用了 esp_restart，还据此把那个 MCP 工具注销了
    // —— 但用户说照样重启，说明真凶另有其人，而且我那个处置属于误判。
    // 现在每次开机都把「上一次的复位原因」打出来，下次复现读这行就够了：
    // 正常上电是 POWERON；看到 PANIC / xxx_WDT / BROWNOUT 就对应崩溃、喂狗超时、供电不足。
    // 存下来而不是只打日志：串口那行一滚就过去了，等用户发现重启时根本来不及抓。
    // 放网页 /api/state 里，他遇到重启后打开网页就能看到。
    const esp_reset_reason_t rr = esp_reset_reason();
    switch (rr) {
        case ESP_RST_POWERON:   last_reset_name_ = "POWERON 正常上电"; break;
        case ESP_RST_EXT:       last_reset_name_ = "EXT 外部引脚"; break;
        case ESP_RST_SW:        last_reset_name_ = "SW 软件调用esp_restart"; break;
        case ESP_RST_PANIC:     last_reset_name_ = "PANIC 崩溃"; break;
        case ESP_RST_INT_WDT:   last_reset_name_ = "INT_WDT 中断看门狗"; break;
        case ESP_RST_TASK_WDT:  last_reset_name_ = "TASK_WDT 任务卡死"; break;
        case ESP_RST_WDT:       last_reset_name_ = "WDT 看门狗"; break;
        case ESP_RST_BROWNOUT:  last_reset_name_ = "BROWNOUT 供电不足"; break;
        case ESP_RST_DEEPSLEEP: last_reset_name_ = "DEEPSLEEP"; break;
        case ESP_RST_SDIO:      last_reset_name_ = "SDIO"; break;
        default:                last_reset_name_ = "UNKNOWN"; break;
    }
    ESP_LOGW(TAG, "上次复位: %s", last_reset_name_);

    /* 读上次崩溃时执行到切主题的哪一步（步骤号见 ApplyThemeSwitch）。
       RTC 内存掉电才丢，所以「上电」时这里通常是无效值，靠 magic 过滤。 */
    if (s_panic_trace.magic == PANIC_TRACE_MAGIC) {
        if (s_panic_trace.step != 0) {
            snprintf(s_panic_desc, sizeof(s_panic_desc), "崩在切主题第%u步(共切换%u次)",
                     (unsigned)s_panic_trace.step, (unsigned)s_panic_trace.count);
        } else {
            snprintf(s_panic_desc, sizeof(s_panic_desc), "切主题路径外(共切换%u次)",
                     (unsigned)s_panic_trace.count);
        }
        ESP_LOGW(TAG, "崩溃标记: %s", s_panic_desc);
        s_panic_trace.step = 0;   // 读完清零：下次若还是 0，说明崩在切主题之外
    } else {
        /* RTC 内容无效（上电会清掉）：初始化，避免读到上次遗留的垃圾值
           （之前 count 显示 333851905 就是这么来的）。
           rsv 复用为「LVGL 任务栈最低水位」，初值取最大。 */
        s_panic_trace.magic = PANIC_TRACE_MAGIC;
        s_panic_trace.step  = 0;
        s_panic_trace.count = 0;
        s_panic_trace.rsv   = 0xFFFFFFFFu;
    }
}

const char* DafeiyuDisplay::LastPanicStep() {
    return s_panic_desc;
}

DafeiyuDisplay* DafeiyuDisplay::instance_ = nullptr;
const char* DafeiyuDisplay::last_reset_name_ = "未初始化";

DafeiyuDisplay::~DafeiyuDisplay() {
    instance_ = nullptr;
    if (tick_timer_ != nullptr) {
        lv_timer_delete(tick_timer_);
        tick_timer_ = nullptr;
    }
    if (gif_) {
        gif_->Stop();
        gif_.reset();
    }
}

void DafeiyuDisplay::SetupUI() {
    LcdDisplay::SetupUI();  // 先建好原生 UI（内部已加锁）

    DisplayLockGuard lock(this);

    // 全屏立绘：不需要原生表情/字幕/侧栏
    // sys_bar_ 整个隐掉：它自带的那行「时间 + 电量 + 网络」跟我们自己的时钟
    // 卡片重复，而且是一串没标签的图标，用户反馈「看不懂是啥」。配网提示 /
    // 验证码 / 音量改由 sys_label_ 和 notification_label_ 承担，这两个有明确上下文。
    lv_obj_t* hidden[] = {container_,   top_bar_,     content_,
                          side_bar_,    bottom_bar_,  preview_image_, emoji_box_,
                          // ⚠️ 必须写 LcdDisplay::status_bar_ 这个限定名。
                          // 血泪教训：我自己也定义过一个叫 status_bar_ 的成员（现改名 sys_bar_），
                          // 同名会遮蔽基类的，导致这一句悄悄改成隐藏我自己的那个（当时还是 nullptr），
                          // 原生顶栏反而漏网 —— 于是屏幕顶部冒出「检查版本 / 待命」那行字。
                          emoji_image_, emoji_label_, chat_message_label_,
                          LcdDisplay::status_bar_};
    for (auto* obj : hidden) {
        if (obj != nullptr) {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (notification_label_ != nullptr) {
        // ⚠️ 2026-10-31 结案：「音量胶囊不出现」的真正根因（我前三次全猜错了）。
        //  1) 它不是被立绘盖住 —— 加提层没用
        //  2) 它也不在 hidden[] 列表里 —— 那一串是别的兄弟对象
        //  3) 真正的原因：它的父对象是 sys_bar_（见 lcd_display.cc:474），
        //     而 sys_bar_ 就在上面那个 hidden[] 里被整条隐藏了。
        //     LVGL 里父对象隐藏 → 子对象永不渲染；move_foreground 只在兄弟间排序，
        //     救不了「爹被藏了」这种情况。
        //     实测 ShowNotification 被正常调用了 29 次、文本也设上了，就是显示不出来。
        //  解法：改挂到屏幕上，脱离那个被隐藏的容器。
        if (lv_obj_t* scr = lv_screen_active(); scr != nullptr) {
            lv_obj_set_parent(notification_label_, scr);
        }

        // 音量、配网提示都走这里：给个胶囊底，比光秃秃一行字好看
        // ⚠️ 这里原来用的是 lv_font_montserrat_14 —— 那是纯拉丁字体，一个汉字都没有，
        // 而「配网模式」「进入配网模式...」是中文，走这条通道会渲染成一片空白。
        // 必须用补过汉字的那份 basic 字库（由 _build_font.py 生成）。
        lv_obj_set_style_text_font(notification_label_, &font_noto_sans_basic_14_1, 0);
        lv_obj_set_style_text_color(notification_label_, lv_color_hex(kClockColor), 0);
        lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(notification_label_, LV_HOR_RES - 24);
        lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(notification_label_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(notification_label_, LV_OPA_50, 0);
        lv_obj_set_style_radius(notification_label_, 10, 0);
        lv_obj_set_style_pad_all(notification_label_, 6, 0);
        lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    }

    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(kScreenBgColor), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // 壁纸在最底层：网页上传的 RGB565 原始图，没有上传时隐藏（露出底色）
    wallpaper_image_ = lv_image_create(screen);
    lv_obj_set_size(wallpaper_image_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(wallpaper_image_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(wallpaper_image_, LV_OBJ_FLAG_HIDDEN);

    pet_image_ = lv_image_create(screen);
    lv_obj_set_size(pet_image_, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(pet_image_);
    lv_obj_remove_flag(pet_image_, LV_OBJ_FLAG_SCROLLABLE);

    // ── 顶部胶囊条：时间 + 电量 + WiFi（2026-11-06 用户定稿）──
    // ⚠️ 必须建在 pet_image_ 之后：LVGL 后建的在上层，立绘是全屏的，
    // 早建的会被整块盖住看不见。全主题通用 —— 壁纸/时钟/肥鱼下都显示。
    // 样式：半透明黑胶囊（之前用户嫌一个孤立元素突兀，这是复用原有状态条改造）。
    sys_bar_ = lv_obj_create(screen);
    lv_obj_set_size(sys_bar_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(sys_bar_, LV_ALIGN_TOP_RIGHT, -kPanelMargin, kPanelMargin);
    lv_obj_set_style_bg_color(sys_bar_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(sys_bar_, LV_OPA_60, 0);
    lv_obj_set_style_border_width(sys_bar_, 0, 0);
    lv_obj_set_style_radius(sys_bar_, 14, 0);
    lv_obj_set_style_pad_hor(sys_bar_, 10, 0);
    lv_obj_set_style_pad_ver(sys_bar_, 5, 0);
    lv_obj_set_style_pad_gap(sys_bar_, 7, 0);
    lv_obj_set_flex_flow(sys_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sys_bar_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(sys_bar_, LV_SCROLLBAR_MODE_OFF);

    // 电池外框：空心矩形，内部一条填充，宽度按电量百分比走
    battery_outline_ = lv_obj_create(sys_bar_);
    lv_obj_set_size(battery_outline_, 22, 12);
    lv_obj_set_style_bg_opa(battery_outline_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(battery_outline_, 1, 0);
    lv_obj_set_style_border_color(battery_outline_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(battery_outline_, 2, 0);
    lv_obj_set_style_pad_all(battery_outline_, 1, 0);

    battery_fill_ = lv_obj_create(battery_outline_);
    lv_obj_set_height(battery_fill_, 6);
    lv_obj_align(battery_fill_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_width(battery_fill_, 2);
    lv_obj_set_style_bg_color(battery_fill_, lv_color_hex(0x6BE36B), 0);
    lv_obj_set_style_border_width(battery_fill_, 0, 0);
    lv_obj_set_style_radius(battery_fill_, 1, 0);

    /* 电量百分比数值：用户要求去掉，只留进度条（battery_outline_ + battery_fill_）。
       对象保留但整个隐藏，这样 UpdateStatusBars 里也不用再改它。 */
    battery_label_ = lv_label_create(sys_bar_);
    lv_obj_set_style_text_font(battery_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(battery_label_, "--");
    lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);

    // WiFi 信号：三根由低到高的竖条，像手机那个信号格。
    // 不依赖任何图标素材和特殊字形 —— 纯 LVGL 矩形，零资产、几乎不吃性能。
    wifi_signal_ = lv_obj_create(sys_bar_);
    lv_obj_set_size(wifi_signal_, 15, 11);
    lv_obj_set_flex_flow(wifi_signal_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_signal_, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_all(wifi_signal_, 0, 0);
    lv_obj_set_style_pad_gap(wifi_signal_, 1, 0);
    lv_obj_set_style_border_width(wifi_signal_, 0, 0);
    lv_obj_set_style_bg_opa(wifi_signal_, LV_OPA_TRANSP, 0);

    static const uint8_t kBarHeights[3] = {4, 7, 10};
    for (int i = 0; i < 3; ++i) {
        wifi_bars_[i] = lv_obj_create(wifi_signal_);
        lv_obj_set_size(wifi_bars_[i], 3, kBarHeights[i]);
        lv_obj_set_style_bg_color(wifi_bars_[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(wifi_bars_[i], LV_OPA_20, 0);   // 未点亮：暗
        lv_obj_set_style_border_width(wifi_bars_[i], 0, 0);
        lv_obj_set_style_radius(wifi_bars_[i], 1, 0);
    }

    // ── 壁纸主题的左上大时钟（2026-11-06 用户要求）──
    // 半透明胶囊底 + 48 号大字 HH:MM。壁纸主题下没有别的时钟
    // （clock_panel_ 已删），这是壁纸状态下唯一的时间显示。
    // 默认隐藏，ShowWallpaperTheme 时显示并提层。
    wp_clock_ = lv_obj_create(screen);
    lv_obj_set_size(wp_clock_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(wp_clock_, LV_ALIGN_TOP_LEFT, kPanelMargin, kPanelMargin);
    lv_obj_set_style_bg_color(wp_clock_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(wp_clock_, LV_OPA_60, 0);
    lv_obj_set_style_radius(wp_clock_, 14, 0);
    lv_obj_set_style_border_width(wp_clock_, 0, 0);
    lv_obj_set_style_pad_hor(wp_clock_, 14, 0);
    lv_obj_set_style_pad_ver(wp_clock_, 6, 0);
    lv_obj_set_scrollbar_mode(wp_clock_, LV_SCROLLBAR_MODE_OFF);
    wp_clock_label_ = lv_label_create(wp_clock_);
    lv_obj_set_style_text_font(wp_clock_label_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(wp_clock_label_, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(wp_clock_label_, "--:--");
    lv_obj_add_flag(wp_clock_, LV_OBJ_FLAG_HIDDEN);

    /* 旧时钟卡片 clock_panel_（时间 / 日期 / IP 三行）已于 2026-11-01 删除 ——
       时钟和日期现在由主题引擎画，IP 通过主题的文案行显示。
       UpdateClock() 和 SetClockVisible() 开头都有 `if (clock_label_/clock_panel_
       == nullptr) return;`，所以这里不建对象后它们会自动变成空操作。
       但 IP 文本还需要一个 label 存字符串，建在 screen 上并保持隐藏
       （真正显示走 pet_theme_set_quote()）。 */
    ip_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(ip_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ip_label_, lv_color_hex(kClockDateColor), 0);
    lv_label_set_text(ip_label_, "");
    lv_obj_add_flag(ip_label_, LV_OBJ_FLAG_HIDDEN);

    // 系统消息（配网 / 激活 / 升级进度）：状态栏上方，用主题字体所以中文能渲染
    sys_label_ = lv_label_create(screen);
    lv_obj_set_width(sys_label_, LV_HOR_RES - 24);
    lv_obj_set_style_text_align(sys_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(sys_label_, lv_color_hex(kClockColor), 0);
    lv_label_set_long_mode(sys_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(sys_label_, LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_label_set_text(sys_label_, "");
    lv_obj_add_flag(sys_label_, LV_OBJ_FLAG_HIDDEN);

    // 激活验证码：大号数字，屏幕中央
    sys_code_label_ = lv_label_create(screen);
    lv_obj_set_style_text_font(sys_code_label_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(sys_code_label_, lv_color_hex(0xFFD166), 0);
    lv_obj_align(sys_code_label_, LV_ALIGN_CENTER, 0, -30);
    lv_label_set_text(sys_code_label_, "");
    lv_obj_add_flag(sys_code_label_, LV_OBJ_FLAG_HIDDEN);

    /* ── 2026-11-05 音乐可视化三件套 ──
       频谱条：居中 16 根真 FFT 柱，只在放音乐时出现。
       歌词：只在放音乐时显示服务端唱词（2026-11-06 恢复，用户要歌词；
       说话时依旧无文字 —— 字幕功能本身已撤）。
       歌名：协议层 play_music 后第一条 sentence_start 自动抓取，屏顶显示。
       对象只创建一次、只隐藏不删除（同菜单/系统消息的约定）。 */
    BuildSpectrum();

    /* 歌词标签：字体在显示时动态换成 assets 全量字库（主题 text_font，
       6845 字），创建时先用 basic 兜底。 */
    subtitle_label_ = lv_label_create(screen);
    lv_obj_set_width(subtitle_label_, LV_HOR_RES - 32);
    lv_obj_set_style_text_font(subtitle_label_, &font_noto_sans_basic_20_4, 0);
    lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(subtitle_label_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(subtitle_label_, LV_OPA_60, 0);
    lv_obj_set_style_radius(subtitle_label_, 8, 0);
    lv_obj_set_style_pad_all(subtitle_label_, 5, 0);
    lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(subtitle_label_, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_label_set_text(subtitle_label_, "");
    lv_obj_add_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);

    song_label_ = lv_label_create(screen);
    lv_obj_set_width(song_label_, LV_HOR_RES - 40);
    lv_obj_set_style_text_font(song_label_, &font_noto_sans_basic_20_4, 0);
    lv_obj_set_style_text_color(song_label_, lv_color_hex(0xFFD166), 0);
    lv_obj_set_style_text_align(song_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(song_label_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(song_label_, LV_OPA_50, 0);
    lv_obj_set_style_radius(song_label_, 8, 0);
    lv_obj_set_style_pad_all(song_label_, 4, 0);
    lv_label_set_long_mode(song_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(song_label_, LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_text(song_label_, "");
    lv_obj_add_flag(song_label_, LV_OBJ_FLAG_HIDDEN);

    /* 养成面板 stat_panel_（饱食/心情/亲密 三条 bar + 数值 + 等级）已于
       2026-11-01 删除：养成玩法 PetGame 早先已删，这些条只显示固定满值，没有意义。
       UpdateStatusBars() 开头有 `if (hunger_bar_ == nullptr) return;`，
       所以这里不建对象后它会自动变成空操作，无需再改。 */

    // 音量胶囊的位置/样式：用户反馈原先居中那个黑条「很丑」而且挡在角色脸上。
    // 改成贴在养成面板正上方，宽度对齐面板，用同一套圆角卡片视觉 —— 这样它看起来
    // 是这张卡片的延伸，而不是悬浮在系统界面上的一层。
    // （前面那段把它 set_parent 到屏幕 + 这里的 align_to 会覆盖更早的居中设置）
    if (notification_label_ != nullptr) {
        // 样式在这里定，但「位置」不在这里定 —— 见下方说明。
        // 用户实测：在这里 align_to 会让胶囊跑到屏幕垂直中间、左端还伸出屏幕外，
        // 文字被截成「连接 QQ...」。原因是 stat_panel_ 高度为 LV_SIZE_CONTENT，
        // 此刻还没经过布局测量，拿到的是未定坐标（连 update_layout 也救不回来，
        // 整棵树要等首次刷新才稳定）。
        // 所以定位统一挪到第一个 tick 里做（那时布局必然已成），见 TickCallback。
        lv_obj_set_style_text_color(notification_label_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(notification_label_, LV_OPA_40, 0);
        lv_obj_set_style_radius(notification_label_, 8, 0);
        lv_obj_set_style_border_width(notification_label_, 0, 0);
        lv_obj_move_foreground(notification_label_);
    }

    // ── 大肥鱼待机主题接管屏幕 ─────────────────────────────────────────────
    // 2026-11-01 用户决定：养成面板没意义、现有宠物引擎不满意，整体换成
    // _themes/ 主题引擎 + _emotes/ 角色动画，时钟也改用主题自带的。
    //
    // ⚠️ 字体必须显式给：LV_FONT_DEFAULT 不含中文，不指定的话主题里的
    // 日期和文案会渲染成一片空白（这个坑在别处已经踩过一次了）。
    //   时钟 -> 48 号 montserrat（纯数字，够大）
    //   文本 -> 20 号 noto_sans_basic（含汉字，本板 BUILTIN_TEXT_FONT）
    pet_theme_set_fonts(&lv_font_montserrat_48, &font_noto_sans_basic_20_4);
    /* 开机默认主题 = 上次切换后记在 NVS 里的那套；没存过就用默认待机主题。
       这样双击音量键 / 菜单里切过的主题，断电重开还是同一套。 */
    const pet_theme_id_t boot_theme = LoadSavedTheme();
    /* 2026-11-06 存档是扩展主题（100 翻页时钟 / 101 自定义壁纸）时，
       pet_theme_start() 只认 0..8，直接传会越界 —— 先用待机主题把引擎立起来，
       theme_ready_ 置位后再写 theme_go_req_，由 tick 里的 ApplyThemeTo 正常分流。 */
    const bool boot_is_ext = ((int)boot_theme == kThemeClockExt ||
                              (int)boot_theme == kThemeWallpaperExt);
    const pet_theme_id_t boot_eff = boot_is_ext ? PET_THEME_IDLE : boot_theme;
    lv_obj_t* theme_pet = pet_theme_start(boot_eff, screen);
    if (theme_pet != nullptr) {
        pet_emotes_init(theme_pet);
        /* 配套动作也要对上：存档是「哭/生气」这类主题时，
           不能出现"雨天想哭"的背景却站着发呆的角色。 */
        pet_emotes_set((pet_emote_t)pet_theme_action(boot_eff));
        pet_idle_plan_init();
        /* ⚠️ 轮播模式下必须停掉待机行为编排。
           pet_idle_plan 每隔 5~12 秒会随机挑一个动作并 pet_emotes_set() 覆盖掉，
           于是「主题切成了 A、动作却被设成 B」—— 这正是用户反馈的
           「主题 动作 还是掺杂一些变动」。
           轮播时改由主题自己决定配套动作，所以关掉随机编排。
           想恢复自动行为：把 kThemeCycleMs 改成 0，这里就会保持开启。 */
        /* ⚠️ 主题接管后按开关决定随机编排：
           pet_idle_plan 每 5~12 秒会随机挑动作并 pet_emotes_set() 覆盖掉，
           于是「切到哭过一会变默认/吃饭」—— 用户明确不要这个自动变化，
           所以默认关掉（s_auto_emote_ = false）。
           网页「动作变换」开关打开时这里会变成 enable(true)。 */
        pet_idle_plan_enable(s_auto_emote_);

        // 用户反馈「人物有点偏下」：主题自带的 pet_bottom 让角色几乎贴着底边。
        // ⚠️ 改完 y 必须 rebase —— emotes 引擎是拿角色基准位置去算呼吸/跳跃偏移的，
        // 不重设基准的话下一帧就被拉回原处，看起来像「改了没生效」。
        // 两个方向都能调：kPetLiftUp 往上（越大越高），kPetShiftRight 往右（越大越右）。
        // ⚠️ 改完必须 pet_emotes_rebase()，否则下一帧就被动画拉回原处。
        // 框现在是源图尺寸 220 高，而绘制区域底部贴框底，
        // 所以「角色底边」= y + 220。让它落在屏幕底部（240）就需要 y≈20。
        // 而主题给的 y = 240 - 172 - 0 = 68，故需上移 48。
        ApplyPetFix(theme_pet);
        /* ⚠️ 开机动画期间必须把角色藏起来。
           ApplyPetFix() 把角色搬到了 lv_layer_top()，而开机面板 boot_panel_
           在 screen 里 —— top layer 永远盖在 screen 之上，于是动画还没演完
           角色就先出现在屏幕上了（用户反馈的现象）。
           开机结束后由 FinishBootScreen() 把它放出来。 */
        lv_obj_add_flag(theme_pet, LV_OBJ_FLAG_HIDDEN);

        /* ⚠️ 2026-11-01：把角色搬到 LVGL 的 top layer。
           背景里已经做了三件事都没解决遮挡：
             1) pet_theme_start 内部的 move_foreground(s_pet)
             2) 我每帧额外调 move_foreground(theme_pet_)
             3) 让旧的全屏立绘 pet_image_ 永久隐藏
           每帧提最前都无效 —— 说明遮挡物不在「screen 的子对象兄弟顺序」这个维度上。
           top layer 是独立于 screen 的最高层，screen 内任何对象都盖不到它上面去，
           于是不用再管兄弟顺序。坐标是相对各层自己的，top layer 同样是 320x240，
           所以上面设的 x/y 不用改。 */
        /* 见 ApplyPetFix()：角色留在 screen，不再搬 top layer（会引发切主题崩溃） */
        lv_obj_move_foreground(theme_pet);

        theme_pet_ = theme_pet;
        pet_emotes_rebase();

        theme_ready_ = true;
        if (boot_is_ext) {
            /* 存档是扩展主题：请求 tick 分流（ApplyThemeTo 认 100/101）。
               ⚠️ 2026-11-06 用户报「开机过程会闪大肥鱼」：肥鱼引擎先立起来
               才能等 tick 分流，中间那几帧肥鱼画面就露出来了 —— 存档是
               扩展主题时把肥鱼角色先藏住，回肥鱼时 ApplyThemeTo 会恢复。 */
            lv_obj_add_flag(theme_pet_, LV_OBJ_FLAG_HIDDEN);
            theme_go_req_ = (int)boot_theme;
            ESP_LOGI(TAG, "待机主题已启用: %d (扩展主题，肥鱼已隐藏等 tick 分流)", (int)boot_theme);
        } else {
            ESP_LOGI(TAG, "待机主题已启用: %d %s (来自存档)", (int)boot_theme,
                     pet_theme_name(boot_theme));
        }
    } else {
        ESP_LOGW(TAG, "待机主题创建失败，回退到旧界面");
    }

    /* 2026-11-06 修复「重启后壁纸要唤醒再退回才显示」：开机时 storage 还没
       挂载（挂在 StartWebServer），壁纸主题开机直达时 HasWallpaper=false，
       壁纸加载被跳过 —— 这里提前挂载并读一次（都幂等，重复调无害）。 */
    if (MountStorage()) {
        RefreshWallpaper();
    }

    // 旧界面元素让位：先隐藏而不是删代码 —— 主题是全新接入的，万一有问题
    // 还能靠去掉这几行快速回退。等实测通过再彻底删掉它们。
    if (stat_panel_ != nullptr) {
        lv_obj_add_flag(stat_panel_, LV_OBJ_FLAG_HIDDEN);
    }
    if (clock_panel_ != nullptr) {
        lv_obj_add_flag(clock_panel_, LV_OBJ_FLAG_HIDDEN);
    }
    if (pet_image_ != nullptr) {
        lv_obj_add_flag(pet_image_, LV_OBJ_FLAG_HIDDEN);
    }

    // ⚠️ 音量胶囊/提示条看不见的根因：notification_label_ 是基类 SetupUI() 建的，
    // 比这里全屏的 pet_image_ 早，LVGL 又是「后建的在上层」——于是它被整块立绘盖住。
    // 必须在所有pet-related 对象都建好之后补一次提层，它才会重新露出来。
    // 放在 CreateBootScreen() 之前，是为了别把开机动画也压到它下面去。
    if (notification_label_ != nullptr) {
        lv_obj_move_foreground(notification_label_);
    }

    // assets 分区挂载比 UI 晚（约 9 秒），这段时间放自绘的开机动画，别黑屏
    CreateBootScreen();

    /* 唤醒界面：全黑屏 + 呼吸光球（2026-11-02 用户要求，取代之前的边框光环）。
       待命时隐藏；聆听/说话时整屏压黑，只留中心光球呼吸。 */
    wake_overlay_ = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(wake_overlay_);
    lv_obj_set_size(wake_overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(wake_overlay_, 0, 0);
    lv_obj_set_style_bg_color(wake_overlay_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(wake_overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wake_overlay_, 0, 0);
    lv_obj_set_style_radius(wake_overlay_, 0, 0);
    lv_obj_remove_flag(wake_overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(wake_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wake_overlay_, LV_OBJ_FLAG_HIDDEN);

    /* 外层光晕：大而淡，反向脉动，像能量往外扩散 */
    wake_halo_ = lv_obj_create(wake_overlay_);
    lv_obj_remove_style_all(wake_halo_);
    lv_obj_set_size(wake_halo_, 110, 110);
    lv_obj_center(wake_halo_);
    lv_obj_set_style_radius(wake_halo_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(wake_halo_, lv_color_hex(0x2FE0FF), 0);
    lv_obj_set_style_bg_opa(wake_halo_, LV_OPA_20, 0);
    lv_obj_remove_flag(wake_halo_, LV_OBJ_FLAG_CLICKABLE);

    /* 核心光球：亮、带阴影发光，直径和光晕一起呼吸 */
    wake_orb_ = lv_obj_create(wake_overlay_);
    lv_obj_remove_style_all(wake_orb_);
    lv_obj_set_size(wake_orb_, 56, 56);
    lv_obj_center(wake_orb_);
    lv_obj_set_style_radius(wake_orb_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(wake_orb_, lv_color_hex(0x2FE0FF), 0);
    lv_obj_set_style_bg_opa(wake_orb_, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(wake_orb_, lv_color_hex(0x2FE0FF), 0);
    lv_obj_set_style_shadow_width(wake_orb_, 30, 0);
    lv_obj_set_style_shadow_opa(wake_orb_, LV_OPA_70, 0);
    lv_obj_remove_flag(wake_orb_, LV_OBJ_FLAG_CLICKABLE);

    /* 聆听用：三圈向外扩散的涟漪（只有边框、背景透明，依次放大并淡出） */
    for (int i = 0; i < 3; i++) {
        wake_ripple_[i] = lv_obj_create(wake_overlay_);
        lv_obj_remove_style_all(wake_ripple_[i]);
        lv_obj_set_size(wake_ripple_[i], 40, 40);
        lv_obj_center(wake_ripple_[i]);
        lv_obj_set_style_radius(wake_ripple_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(wake_ripple_[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wake_ripple_[i], 3, 0);
        lv_obj_set_style_border_color(wake_ripple_[i], lv_color_hex(0x2FE0FF), 0);
        lv_obj_remove_flag(wake_ripple_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(wake_ripple_[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 说话用：五条跳动的声波条（圆角竖条，高度随时间变化，像均衡器） */
    {
        const int bw = 8, gap = 9, n = 5;
        const int total = n * bw + (n - 1) * gap;          // 76
        const int x0 = (LV_HOR_RES - total) / 2;
        for (int i = 0; i < n; i++) {
            wake_bar_[i] = lv_obj_create(wake_overlay_);
            lv_obj_remove_style_all(wake_bar_[i]);
            lv_obj_set_size(wake_bar_[i], bw, 20);
            lv_obj_set_pos(wake_bar_[i], x0 + i * (bw + gap), LV_VER_RES / 2);
            lv_obj_set_style_radius(wake_bar_[i], bw / 2, 0);   // 胶囊头
            lv_obj_set_style_bg_color(wake_bar_[i], lv_color_hex(0xFF7A29), 0);
            lv_obj_set_style_bg_opa(wake_bar_[i], LV_OPA_COVER, 0);
            lv_obj_remove_flag(wake_bar_[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(wake_bar_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 画布版呼吸球：180x180 RGB565，缓冲放 PSRAM（约 65KB）。
       申请失败就保持 nullptr，上面那些图元对象会作为备用方案继续用。 */
    /* 2026-11-02 重开画布，改用 I8 索引色（256 色调色板）：
       每像素只存 1 字节索引，同样 180x180 只要 32.4KB，
       比 RGB565 的 65KB 省一半，而渐变/发光/高光都能用调色板还原。
       ⚠️ 必须用 I8 —— L8 是灰度格式，用错会变黑白。 */
    /* 2026-11-02 改用 xiaozhi-orb 组件（main/orb/）：纯整数扫描线渲染，
       径向渐变降级为垂直渐变查表、sin 走 256 项查表、只刷球体脏矩形。
       canvas 半径 = 40*2.65 ≈ 106 → 约 214x214 缓冲放 PSRAM（90KB），
       内部 RAM 零负担。fps 传 0 由我们在 OnTick 里手动驱动。 */
    wake_orb_lv_ = orb_lv_create(wake_overlay_, LV_HOR_RES / 2, LV_VER_RES / 2, 40);
    if (wake_orb_lv_ != nullptr) {
        orb_lv_set_bg(wake_orb_lv_, 0x0000);
        orb_lv_set_fps(wake_orb_lv_, 0);
        ESP_LOGI(TAG, "唤醒球已建立(xiaozhi-orb canvas %dx%d)",
                 (int)wake_orb_lv_->w, (int)wake_orb_lv_->h);
    } else {
        ESP_LOGW(TAG, "唤醒球创建失败(PSRAM 不足)，退回图元方案");
    }

    /* 2026-11-02 大眼睛方案已停用（用户反馈不好看），不再创建对象。
       相关函数保留在文件里，将来想再启用只需放开下面这一行。 */
    // BuildEyesFace();

    lv_obj_move_foreground(wake_overlay_);

    tick_timer_ = lv_timer_create(&DafeiyuDisplay::TickCallback, kTickIntervalMs, this);
    ESP_LOGI(TAG, "full screen pet display ready (%dx%d)", LV_HOR_RES, LV_VER_RES);
}

void DafeiyuDisplay::TickCallback(lv_timer_t* timer) {
    auto* self = static_cast<DafeiyuDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->OnTick();
    }
}

/* ====================== 按键菜单（长按 BOOT 进入） ======================
   交互：长按 BOOT 打开 → 音量+/- 上下移动 → BOOT 确认 → 长按 BOOT 退出。
   实现要点：
     1. 菜单对象挂在 lv_layer_top()，角色被 ApplyPetFix 搬到了同一层，
        所以每次打开都 move_foreground，保证盖在角色上面；
     2. 对象只创建一次、只隐藏不删除 —— 这个工程里删 LVGL 对象踩过
        全局事件链表的坑（见 UpdateBootScreen 的注释）；
     3. 按键回调只写 menu_req_，所有 LVGL 操作都在 tick 里做。 */
/* 2026-11-05 顺序跟 MenuItem 枚举对齐：控制页插在主题后面。
   2026-11-06 改名「WEB」：basic 子集字库没有 控/制/页 三个字（渲染成方块），
   查过 charsets/basic.json，中文名里只有 浏览/手机/开关 全覆盖，都不达意。 */
/* 2026-11-06 用户定稿：主题/时钟/壁纸 三个直达选项，各走各的，不再混在一个循环里。
   「退出」删了 —— 长按 BOOT 本来就能关菜单。总数还是 8 项，行距不用动。 */
static const char* kMenuNames[DafeiyuDisplay::kMenuCount] = {
    "音量", "亮度", "主题", "时钟", "壁纸", "WEB", "配网", "重启"};

void DafeiyuDisplay::BuildMenu() {
    if (menu_root_ != nullptr) {
        return;
    }
    menu_root_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(menu_root_);
    lv_obj_set_size(menu_root_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(menu_root_, 0, 0);
    lv_obj_set_style_bg_color(menu_root_, lv_color_hex(0x05070C), 0);
    lv_obj_set_style_bg_opa(menu_root_, LV_OPA_90, 0);
    lv_obj_remove_flag(menu_root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(menu_root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(menu_root_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* title = lv_label_create(menu_root_);
    lv_obj_set_style_text_font(title, &font_noto_sans_basic_14_1, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x2FE0FF), 0);
    lv_label_set_text(title, "菜单");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    for (int i = 0; i < kMenuCount; i++) {
        menu_row_[i] = lv_label_create(menu_root_);
        lv_obj_set_style_text_font(menu_row_[i], &font_noto_sans_basic_14_1, 0);
        /* 2026-11-06 菜单加到 8 项（多「时钟」），行距 24→21
           免得最后一项和底部操作提示叠在一起 */
        lv_obj_align(menu_row_[i], LV_ALIGN_TOP_LEFT, 30, 32 + i * 21);
    }

    menu_tip_ = lv_label_create(menu_root_);
    lv_obj_set_style_text_font(menu_tip_, &font_noto_sans_basic_14_1, 0);
    lv_obj_set_style_text_color(menu_tip_, lv_color_hex(0x66748C), 0);
    lv_label_set_text(menu_tip_, "音量+/-选择  BOOT确认");
    lv_obj_align(menu_tip_, LV_ALIGN_BOTTOM_MID, 0, -8);

    ESP_LOGI(TAG, "按键菜单已建立");
    UpdateMenuUI();
}

void DafeiyuDisplay::UpdateMenuUI() {
    if (menu_root_ == nullptr) {
        return;
    }
    auto& board = Board::GetInstance();
    auto* codec = board.GetAudioCodec();
    auto* bl    = board.GetBacklight();
    char buf[48];

    for (int i = 0; i < kMenuCount; i++) {
        if (menu_row_[i] == nullptr) {
            continue;
        }
        const bool sel = (i == menu_sel_);
        const char* mark = sel ? "> " : "  ";
        if (i == kMenuVolume) {
            const int vol = codec ? codec->output_volume() / 10 : 0;
            snprintf(buf, sizeof(buf), "%s音量 %d", mark, vol);
        } else if (i == kMenuBright) {
            const int pct = bl ? (int)bl->brightness() * 100 / 255 : 0;
            snprintf(buf, sizeof(buf), "%s亮度 %d%%", mark, pct);
        } else {
            snprintf(buf, sizeof(buf), "%s%s", mark, kMenuNames[i]);
        }
        lv_label_set_text(menu_row_[i], buf);
        lv_obj_set_style_text_color(menu_row_[i],
                                    lv_color_hex(sel ? 0x2FE0FF : 0x8E9AB0), 0);
    }
}

void DafeiyuDisplay::MenuClose() {
    menu_open_ = false;
    menu_edit_ = false;
    if (menu_root_ != nullptr) {
        lv_obj_add_flag(menu_root_, LV_OBJ_FLAG_HIDDEN);
    }
}

void DafeiyuDisplay::MenuMove(int dir) {
    if (!menu_open_) {
        return;
    }
    /* 编辑态：音量/亮度是加减，不是移动光标 */
    if (menu_edit_) {
        if (menu_sel_ == kMenuVolume) {
            auto* codec = Board::GetInstance().GetAudioCodec();
            if (codec != nullptr) {
                int v = codec->output_volume() + dir * 5;
                if (v < 0) v = 0;
                if (v > 100) v = 100;
                codec->SetOutputVolume(v);
            }
        } else if (menu_sel_ == kMenuBright) {
            auto* bl = Board::GetInstance().GetBacklight();
            if (bl != nullptr) {
                int b = (int)bl->brightness() + dir * 26;
                if (b < 13) b = 13;
                if (b > 255) b = 255;
                /* permanent=true：写进 NVS，重启后亮度不会回弹 */
                bl->SetBrightness((uint8_t)b, true);
            }
        }
        UpdateMenuUI();
        return;
    }
    /* 光标移动，首尾循环 */
    menu_sel_ = (menu_sel_ + dir + kMenuCount) % kMenuCount;
    UpdateMenuUI();
}

void DafeiyuDisplay::MenuConfirm() {
    if (!menu_open_) {
        return;
    }
    /* 编辑态下再按 BOOT = 结束调节，回到列表 */
    if (menu_edit_) {
        menu_edit_ = false;
        UpdateMenuUI();
        return;
    }
    switch (menu_sel_) {
        case kMenuVolume:
        case kMenuBright:
            menu_edit_ = true;
            UpdateMenuUI();
            break;
        case kMenuTheme:
            /* 2026-11-06 用户定稿：菜单「主题」永远指大肥鱼系列 ——
               在肥鱼下 = 循环下一套；在时钟/壁纸下 = 直接回肥鱼待机。
               （双击音量键才是时钟切样式的入口，两者不共用逻辑。） */
            if (InExtTheme()) {
                theme_go_req_ = 0;
                MenuClose();
            } else {
                SwitchTheme(+1);
            }
            break;
        case kMenuQr:
            /* 2026-11-05 控制页二维码：收起菜单、弹出全屏浮层。
               浮层显示期间 BOOT 单击只负责关浮层（板侧按键分发里处理）。 */
            MenuClose();
            ShowControlPageQr();
            break;
        case kMenuClock:
            /* 2026-11-06 用户定稿：已经在时钟主题下时，再选一次「时钟」
               = 切换到下一个时钟样式（本主题内循环，不用先绕回肥鱼）。 */
            if (ext_theme_ == kThemeClockExt) {
                MenuClose();
                CycleClockStyle(+1);
                break;
            }
            MenuClose();
            theme_go_req_ = kThemeClockExt;
            break;
        case kMenuWallpaper:
            MenuClose();
            if (ext_theme_ != kThemeWallpaperExt) {
                theme_go_req_ = kThemeWallpaperExt;
            }
            break;
        case kMenuWifi:
        case kMenuReboot:
            if (menu_action_ && menu_action_(menu_sel_)) {
                MenuClose();
            }
            break;
        default:
            MenuClose();
            break;
    }
}

void DafeiyuDisplay::ApplyMenuRequest() {
    const int req = menu_req_;
    if (req == kMenuNone) {
        return;
    }
    menu_req_ = kMenuNone;

    if (req == kMenuOpen) {
        /* 开机动画期间、唤醒对话中、拍照预览时不弹菜单 ——
           这几个界面各有自己的时序，插进来会互相打架。 */
        if (!boot_finished_ || wake_state_ != 0 || previewing_) {
            return;
        }
        if (menu_root_ == nullptr) {
            BuildMenu();
        }
        if (menu_root_ == nullptr) {
            return;
        }
        menu_open_ = true;
        menu_edit_ = false;
        menu_sel_ = 0;
        lv_obj_remove_flag(menu_root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(menu_root_);   // 角色也在 top layer，必须提到最前
        UpdateMenuUI();
        return;
    }

    if (!menu_open_) {
        return;
    }
    switch (req) {
        case kMenuUp:      MenuMove(-1); break;
        case kMenuDown:    MenuMove(+1); break;
        case kMenuConfirm: MenuConfirm(); break;
        case kMenuClose:   MenuClose(); break;
        default: break;
    }
}

void DafeiyuDisplay::OnTick() {
    /* 菜单请求最先处理：它不依赖素材，哪怕 pet_image_ 还没就绪也要能用 */
    ApplyMenuRequest();

    if (pet_image_ == nullptr) {
        return;
    }
    // 网页验收接口请求的片段：HTTP 回调跑在别的线程，当时只置了 debug_clip_ 标记，
    // 真正的装载必须挪到这个 LVGL 线程里做，否则两边会同时改同一批 lv_obj 状态
    if (debug_clip_ != Clip::kNone) {
        Clip clip = debug_clip_;
        debug_clip_ = Clip::kNone;
        PlayOnce(clip);
    }
    // 首次 tick：此时整棵 UI 树已经过一轮布局，坐标是最终值 —— 只有现在定位才准。
    // （在 SetupUI 里 align_to 会拿到未定坐标，实测胶囊跑到屏幕垂直中间、左端出屏）
    if (!ui_settled_) {
        if (notification_label_ != nullptr) {
            ui_settled_ = true;
            /* 旧养成面板 stat_panel_ 已删除，音量胶囊改为直接对齐屏幕顶部居中。
               宽度写死 200（原来取的是面板宽度）。 */
            lv_obj_set_width(notification_label_, 200);
            lv_obj_align(notification_label_, LV_ALIGN_TOP_MID, 0, 8);
            lv_obj_move_foreground(notification_label_);
            ESP_LOGI(TAG, "音量胶囊已定位到养成面板上方 (w=%d)",
                     (int)lv_obj_get_width(notification_label_));
        }
    }

    // 主题引擎 tick。开机动画期间不跑 —— 那段时间画面由 boot_panel_ 主导，
    // 跑了也看不见，白白占 CPU。
    if (boot_finished_ && theme_ready_) {
        const uint32_t now = lv_tick_get();
        // TODO: 小时目前写死 12。pet_idle_plan 用它判断「深夜该打哈欠」之类，
        // 等时钟接入后换成真实小时。
        /* 双击音量键的切主题请求在这里真正执行 —— 本处跑在 LVGL 线程，
           而按键回调不是，所以那边只能置标志、这边才能动 LVGL。 */
        if (theme_switch_req_ != 0) {
            const int dir = theme_switch_req_;
            theme_switch_req_ = 0;
            ApplyThemeSwitch(dir);
        }
        /* 绝对切主题（MCP 工具 self.pet.set_theme）：同样只能在 tick 里做 */
        if (theme_go_req_ >= 0) {
            const int id = theme_go_req_;
            theme_go_req_ = -1;
            ApplyThemeTo(id);
        }

        /* ── 2026-11-06 性能定稿：肥鱼引擎 tick 只在「大肥鱼待机 + 没在对话」时跑 ──
           · 扩展主题（时钟/壁纸）：角色和装饰全隐藏，这三个 tick 纯属空转
             （idle_plan 还会随机挑动作、emotes/theme 逐帧算动画坐标）—— 跳过；
           · 对话中（唤醒层压黑）：对象全藏了，动画照算也是白算 —— 省下来的
             CPU 直接留给音频解码，这是「对话卡顿」的显示侧减负。
           恢复时机：回肥鱼主题 / 唤醒结束，条件重新满足时 tick 自然续上
           （引擎内部状态都在，无需重置）。 */
        if (ext_theme_ < 0 && wake_state_ == 0) {
        pet_idle_plan_tick(now, 12);
        pet_theme_tick(now);
        pet_emotes_tick(now);
        // 角色和装饰不在同一个父容器里（装饰在主题的 s_cont 下，角色在屏幕上），
        // pet_theme_start 里那次 move_foreground 只在当时有效，装饰后续重建/重排
        // 会把角色压到下面 —— 实测角色脚被从左往右飘的蓝色线条（云朵/波纹）盖住。
        // 所以每帧把角色拉回最前。
        // 主题轮播（调试用）：每 kThemeCycleMs 切到下一套，并在串口打印名字。
        // 想固定只看某一套：把 kThemeCycleMs 改成 0，再把 SetupUI 里
        // pet_theme_start(PET_THEME_IDLE, ...) 换成想要的 id。
        if (kThemeCycleMs > 0 && lv_tick_elaps(s_theme_cycle_ms) > kThemeCycleMs) {
            s_theme_cycle_ms = now;
            pet_theme_id_t id = (pet_theme_id_t)((pet_theme_current() + 1) % PET_THEME_N);
            lv_obj_t *np = pet_theme_start(id, lv_scr_act());
            if (np != nullptr) {
                /* 注意：这里不能调 pet_emotes_init()！
                   s_pet 是复用的（pet_themes.c 里 if (!s_pet) 才创建），
                   而且 init 是「初始化」语义，反复调用会累积动画/定时器 ——
                   开着轮播时机器不断重启就是这么来的。切换只需 set 动作。 */
                // 主题配套动作：pet_theme_action() 返回该主题对应的 pet_emote_t。
                // 之前只切背景不切动作，所以每套主题看起来角色都在发呆。
                pet_emotes_set((pet_emote_t)pet_theme_action(id));
                ApplyPetFix(np);
                theme_pet_ = np;
                pet_emotes_rebase();
                ESP_LOGI(TAG, "主题轮播 -> %d %s", (int)id, pet_theme_name(id));
            }
        }
        if (theme_pet_ != nullptr) {
            lv_obj_move_foreground(theme_pet_);
        }
        }   /* end: ext_theme_ < 0 && wake_state_ == 0 */
    }

    UpdateStatusBar();   // 内部有 3 秒节流

    /* 记录 LVGL 任务栈的最低水位（剩余字节数），写进 RTC 内存 —— panic 重启后
       仍然保留，所以崩一次就能在 /api/state 看到当时栈还剩多少。
       OnTick 是 lv_timer 回调，本身就跑在 LVGL 任务里，故用 NULL 取当前任务。 */
    {
        const uint32_t hw = uxTaskGetStackHighWaterMark(NULL);
        if (hw < s_panic_trace.rsv) {
            s_panic_trace.rsv = hw;
        }
    }

    /* ── 2026-11-05 音乐可视化 + 浮层收尾 ── */
    UpdateSpectrum();
    /* 歌名条：只在音乐没在放时才按 20 秒倒计时收 —— 放歌中不倒计时
       （用户反馈「名字显示一会就没了」）。音乐停止由 SetMusicPlaying(false)
       立刻收，倒计时只是 MCP 兜底流程的保险。 */
    if (song_label_ != nullptr && song_until_ms_ != 0 &&
        !music_playing_ && lv_tick_get() >= song_until_ms_) {
        song_until_ms_ = 0;
        lv_obj_add_flag(song_label_, LV_OBJ_FLAG_HIDDEN);
    }
    /* 控制页浮层关闭请求：按键任务置标志，这里（LVGL 线程）执行。
       ⚠️ 2026-11-07 用户要求：不打开二维码时默认关闭，别一直开着 ——
       旧逻辑关闭只藏不删，45KB canvas 缓冲 + 浮层对象常驻内存。
       现在关闭即整体销毁，下次打开时 ShowControlPageQr 会重建。 */
    if (qr_close_req_) {
        qr_close_req_ = false;
        if (qr_root_ != nullptr) {
            lv_obj_delete(qr_root_);
            qr_root_ = nullptr;
            qr_url_label_ = nullptr;
            qr_canvas_ = nullptr;
        }
        if (qr_canvas_buf_ != nullptr) {
            heap_caps_free(qr_canvas_buf_);
            qr_canvas_buf_ = nullptr;
        }
        qr_visible_ = false;
    }
    /* 2026-11-06 翻页时钟模式：关闭请求 + 每秒驱动 */
    if (clock_close_req_) {
        clock_close_req_ = false;
        CloseClockMode();
    }
    if (clock_mode_visible_) {
        ClockTick();
    }

    /* ── 大眼睛待机形象 ──
       ApplyFaceMode 是幂等的（状态没变就只做两次标志位比较），每帧调无妨；
       这样网页开关一改、或开机/唤醒状态一变，下一帧就能自动纠正。 */
    ApplyFaceMode();
    if (face_root_ != nullptr && !lv_obj_has_flag(face_root_, LV_OBJ_FLAG_HIDDEN)) {
        AnimateEyes(lv_tick_get());
    }

    /* 唤醒呼吸球：xiaozhi-orb 每帧约 2.5ms（整数扫描线 + 只刷脏矩形）。
       只在界面可见时渲染 —— 待命时一次都不算，零开销。 */
    if (wake_orb_lv_ != nullptr && wake_state_ != 0 && GetConfig().wake_style == 0 &&
        !lv_obj_has_flag(wake_overlay_, LV_OBJ_FLAG_HIDDEN) && !spec_visible_) {
        static uint32_t last_ms = 0;
        static uint32_t last_pcm_ms = 0;
        static float    lvl = 0.0f;
        static float    band[4] = {0, 0, 0, 0};
        const uint32_t now_ms = lv_tick_get();
        const uint32_t dt = (last_ms == 0) ? 50 : (now_ms - last_ms);
        last_ms = now_ms;

        /* 真实音频驱动（2026-11-02）：音频任务通过 orb_feed_pcm_global 把
           播放/录音 PCM 换成电平，这里每帧取走 —— 声波环和亮度就跟着真人声音走。
           超过 200ms 没有新数据就让它自然回落，避免"说完了球还亮着"。 */
        if (orb_take_pcm_global(&lvl, band)) {
            last_pcm_ms = now_ms;
        } else if (now_ms - last_pcm_ms > 200) {
            lvl *= 0.85f;
            if (lvl < 0.01f) lvl = 0.0f;
            for (int i = 0; i < 4; i++) {
                band[i] *= 0.85f;
                if (band[i] < 0.01f) band[i] = 0.0f;
            }
        }
        orb_lv_feed_audio(wake_orb_lv_, lvl, band);

        orb_lv_render(wake_orb_lv_, dt);
    }

    /* 唤醒动画：聆听 = 中心球呼吸 + 三圈涟漪向外扩散（收声）；
       说话 = 五条声波条跳动（放声）。两种形状完全不同。
       全用整数三角波，不用浮点，开销很小。 */
    if (wake_overlay_ != nullptr && wake_state_ != 0 && GetConfig().wake_style == 2 &&
        !lv_obj_has_flag(wake_overlay_, LV_OBJ_FLAG_HIDDEN)) {
        const uint32_t ms = lv_tick_get();

        /* 背景光晕：两种状态都留一层柔和呼吸 */
        if (wake_halo_ != nullptr) {
            int p = (int)((ms / 9) % 100);
            int tri = (p < 50) ? (p * 2) : ((99 - p) * 2);
            int hd = 104 + tri * 26 / 100;
            lv_obj_set_size(wake_halo_, hd, hd);
            lv_obj_center(wake_halo_);
            lv_obj_set_style_bg_opa(wake_halo_, (lv_opa_t)(14 + (99 - tri) * 26 / 100), 0);
        }

        if (wake_state_ == 1) {
            /* 聆听：中心球呼吸 */
            if (wake_orb_ != nullptr) {
                int p = (int)((ms / 9) % 100);
                int tri = (p < 50) ? (p * 2) : ((99 - p) * 2);
                int d = 42 + tri * 26 / 100;
                lv_obj_set_size(wake_orb_, d, d);
                lv_obj_center(wake_orb_);
                lv_obj_set_style_shadow_width(wake_orb_, 20 + tri * 26 / 100, 0);
                lv_obj_set_style_bg_opa(wake_orb_, (lv_opa_t)(170 + tri * 85 / 100), 0);
            }
            /* 涟漪：三圈错开相位，由小变大并淡出 */
            for (int i = 0; i < 3; i++) {
                if (wake_ripple_[i] == nullptr) continue;
                int p = (int)((ms / 4 + (uint32_t)i * 60) % 160);
                int ph = p * 100 / 160;                       // 0..99
                int d = 34 + ph * 100 / 100;
                lv_obj_set_size(wake_ripple_[i], d, d);
                lv_obj_center(wake_ripple_[i]);
                lv_obj_set_style_border_opa(wake_ripple_[i],
                                            (lv_opa_t)(230 * (99 - ph) / 99), 0);
            }
        } else {
            /* 说话：五条声波条各自错开相位上下跳动，底边对齐 */
            const int bw = 8, gap = 9, n = 5;
            const int total = n * bw + (n - 1) * gap;
            const int x0 = (LV_HOR_RES - total) / 2;
            const int baseY = LV_VER_RES / 2 + 34;
            for (int i = 0; i < n; i++) {
                if (wake_bar_[i] == nullptr) continue;
                int p = (int)((ms / 6 + (uint32_t)i * 31) % 100);
                int tri = (p < 50) ? (p * 2) : ((99 - p) * 2);
                int h = 14 + tri * 58 / 100;                  // 14..72
                lv_obj_set_size(wake_bar_[i], bw, h);
                lv_obj_set_pos(wake_bar_[i], x0 + i * (bw + gap), baseY - h);
            }
        }
    }
    // 真正的装载只在这里做：SetStatus/SetEmotion 来自协议任务，不能在回调里
    // 分配 384KB canvas + 解首帧（会卡住 TTS 播放和 AFE 唤醒），只记 requested_
    if (requested_ != Clip::kNone) {
        Clip clip = requested_;
        requested_ = Clip::kNone;
        if (PlayLocked(clip)) {
            pending_ = Clip::kNone;
        } else {
            pending_ = clip;  // 资产没就绪，下个 tick 继续重试
        }
        return;
    }
    if (pending_ != Clip::kNone) {
        Play(pending_);
    }

    const uint32_t now = lv_tick_get();
    const bool playing = (gif_ && gif_->IsPlaying());

    // 一次性片段播完 -> 回状态层
    if (current_ != Clip::kNone && !IsLoopClip(current_) && !playing) {
        Clip next = (return_clip_ != Clip::kNone) ? return_clip_ : base_;
        return_clip_ = Clip::kNone;
        if (next != Clip::kNone) {
            Play(next);
        }
        return;
    }

    // 聆听：一直没等到回复就切「思考」，别一直举沙漏
    if (base_ == Clip::kWait && current_ == Clip::kWait && !think_switched_ &&
        lv_tick_elaps(clip_start_ms_) > kListenThinkDelayMs) {
        think_switched_ = true;
        ESP_LOGD(TAG, "listening idle > %ums, switch to thinking", kListenThinkDelayMs);
        Play(Clip::kThink);
    }

    // 待机小动作：由养成数值决定播哪一段 —— 饿去翻吃的、困打哈欠、跟你熟了撒娇求摸、
    // 状态好就得意跳一段。数值一眼看得出，但走「可爱的需求表达」而不是「惨状」：
    // 饿是这个世界上最可爱的事，不是惩罚。
    if (base_ == Clip::kIdle && current_ == Clip::kIdle && next_micro_ms_ != 0 &&
        (int32_t)(now - next_micro_ms_) >= 0) {
        const Clip pick = PickIdleActionByStats();
        if (pick == last_idle_clip_) {
            // 同一个诉求已经演过一遍了：别没事就重复，把下次推迟到你可能真喂过它
            // 的时间之后，否则看着像卡碟
            next_micro_ms_ += kMicroMinMs;
        } else {
            ScheduleNextMicro();
        }
        last_idle_clip_ = pick;
        if (pick != Clip::kNone) {
            PlayOnce(pick);
        }
    }

    ApplyMotionTransform();

    // 待机时钟：只在待机（且没在拍照预览）时露出来
    const bool want_clock =
        assets_ready_ && !previewing_ && base_ == Clip::kIdle && GetConfig().clock_enabled;
    SetClockVisible(want_clock);
    if (want_clock) {
        UpdateClock();
    }

    ApplyRefrPeriod();
    ApplyConfigIfChanged();
    HandlePetAction();
    UpdateStatusBars(false);
    UpdateBootScreen();
    UpdateIpLabel();

    // 系统消息别一直挂着（下载进度这类会不断刷，靠这个超时收摊）
    if (sys_msg_ms_ != 0 && lv_tick_elaps(sys_msg_ms_) > 20000) {
        ShowSystemMessage("");
        sys_msg_ms_ = 0;
    }
}

void DafeiyuDisplay::ApplyConfigIfChanged() {
    /* ⚠️ 2026-11-01 关键修复：主题接管后，外观一律由主题决定，不再套用网页配置。
       这段原本会按网页配置设置背景色并显示壁纸，而壁纸是 320x240 的全屏对象，
       一显示就盖在主题背景之上。因为配置是异步就绪的（版本号要过一会儿才变），
       现象就是用户实测的「开机正常、过一会儿渐变背景又出来把主题盖住」。 */
    if (theme_ready_) {
        return;
    }
    const uint32_t version = ConfigVersion();
    if (version == config_version_) {
        return;
    }
    config_version_ = version;
    const UiConfig cfg = GetConfig();

    lv_obj_t* screen = lv_screen_active();
    if (screen != nullptr) {
        lv_obj_set_style_bg_color(screen, lv_color_hex(cfg.bg_color), 0);
    }

    ApplyLayout(cfg.layout == Layout::kInfo);

    // 壁纸：网页上传后版本号会变，这里重读数据（2026-11-06 起
    // 自定义壁纸只在壁纸主题显示，这里只更新数据不管显隐）
    if (HasWallpaper()) {
        if (wallpaper_buf_ == nullptr) {
            wallpaper_buf_ = static_cast<uint8_t*>(
                heap_caps_malloc(kWallpaperBytes, MALLOC_CAP_SPIRAM));
        }
        if (wallpaper_buf_ != nullptr &&
            LoadWallpaper(wallpaper_buf_, kWallpaperBytes)) {
            wallpaper_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
            wallpaper_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
            wallpaper_dsc_.header.flags = 0;
            wallpaper_dsc_.header.w = kWallpaperW;
            wallpaper_dsc_.header.h = kWallpaperH;
            wallpaper_dsc_.header.stride = kWallpaperW * 2;
            wallpaper_dsc_.data = wallpaper_buf_;
            wallpaper_dsc_.data_size = kWallpaperBytes;
            lv_image_set_src(wallpaper_image_, &wallpaper_dsc_);
            if (ext_theme_ == kThemeWallpaperExt) {
                lv_obj_remove_flag(wallpaper_image_, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(wallpaper_image_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    ESP_LOGI(TAG, "ui config v%u: bg=#%06X clock=%d wallpaper=%d", (unsigned)version,
             (unsigned)(cfg.bg_color & 0xFFFFFF), cfg.clock_enabled ? 1 : 0,
             HasWallpaper() ? 1 : 0);
}

void DafeiyuDisplay::ApplyRefrPeriod() {
    lv_display_t* disp = lv_display_get_default();
    if (disp == nullptr) {
        return;
    }
    lv_timer_t* refr = lv_display_get_refr_timer(disp);
    if (refr == nullptr) {
        return;
    }
    // 只在 TTS 播放（含它前后那一小段）降帧，其余时刻保持顺滑
    // 2026-11-06：放歌期间也降帧 —— 音乐解码 + 频谱 + 角色动画三份 PSRAM/SPI
    // 带宽同时吃紧，20fps 对观感几乎无影响，但能给解码让出明显裕量。
    const bool talking = (base_ == Clip::kSpeak) || (current_ == Clip::kSpeak) ||
                         music_playing_;
    const uint32_t want = talking ? kRefrSpeakMs : kRefrIdleMs;
    if (want == refr_period_ms_) {
        return;
    }
    refr_period_ms_ = want;
    lv_timer_set_period(refr, want);
    ESP_LOGD(TAG, "refresh period -> %ums", static_cast<unsigned>(want));
}

void DafeiyuDisplay::SwitchTheme(int dir) {
    /* ⚠️ 这里只置请求标志，绝不能直接操作 LVGL。
       本函数由音量键的双击回调调用，那个回调不在 LVGL 线程；
       而 pet_theme_start() / lv_obj_* 全都要在 LVGL 线程里跑，
       跨线程调用会直接崩 —— 实测「双击音量键立刻重启」就是这个原因。
       真正的切换放到 tick 里做（见 ApplyThemeSwitch）。 */
    theme_switch_req_ = dir;
}

void DafeiyuDisplay::SetThemeById(int id) {
    /* 越界直接忽略：调用方是 MCP 工具（服务端给的字符串）或表情识别结果，
       不能因为一个坏 id 就把角色切没了。 */
    if (id < 0 || id >= (int)PET_THEME_N) {
        return;
    }
    theme_go_req_ = id;      // 只置请求，tick 里执行
}

/* 双击音量键在时钟主题下的分流目标：4 套样式循环 + NVS 记忆。
   调用方（板侧按键任务）不持 LVGL 锁 —— 但本函数只写共享状态，
   真正重绘在 ClockTick 里做，和 theme_go_req_ 的并发模式一致。 */
void DafeiyuDisplay::CycleClockStyle(int dir) {
    if (fclock_style_ < 0) {
        return;   /* 还没进过时钟主题，样式未载入，忽略 */
    }
    fclock_style_ = (int8_t)((fclock_style_ + dir + kFcStyleN) % kFcStyleN);
    s_fcStyleIdx = fclock_style_;
    Settings s(kThemeNs, true);
    s.SetInt("clk_style", fclock_style_);
    fclock_last_min_ = 0xFFFF;   /* 强制 ClockTick 下一帧整屏重绘出新配色 */
    ESP_LOGI(TAG, "时钟样式 -> %d/%d", fclock_style_, kFcStyleN);
}

/* ── 网页控制页视图接口（2026-11-06 用户要求）──
   HTTP 线程调用。扩展主题/跨大区切换写 theme_go_req_（tick 消费，与按键同通道）；
   肥鱼内部直切直接调 ApplyThemeTo（自带 LVGL 递归锁）。 */
bool DafeiyuDisplay::WebSetView(int id) {
    if (id == kThemeWallpaperExt && !HasWallpaper()) {
        return false;   /* 还没上传过壁纸，别让它切过去看黑屏 */
    }
    if (id == kThemeClockExt || id == kThemeWallpaperExt) {
        theme_go_req_ = id;
        return true;
    }
    if (id < 0 || id >= (int)PET_THEME_N) {
        return false;
    }
    if (ext_theme_ >= 0) {
        theme_go_req_ = id;   /* 从时钟/壁纸回肥鱼指定套 */
        return true;
    }
    if ((int)pet_theme_current() == id) {
        return true;          /* 已经是这套，幂等 */
    }
    ApplyThemeTo(id);
    return true;
}

int DafeiyuDisplay::WebCurrentView() const {
    return ext_theme_ >= 0 ? ext_theme_ : (int)pet_theme_current();
}

int DafeiyuDisplay::WebClockStyle(int n) {
    if (fclock_style_ < 0) {   /* 还没进过时钟：先按 NVS 载入一次 */
        Settings s(kThemeNs, false);
        int st = s.GetInt("clk_style", 0);
        fclock_style_ = (int8_t)(st < 0 || st >= kFcStyleN ? 0 : st);
        s_fcStyleIdx = fclock_style_;
    }
    if (n >= 0 && n < kFcStyleN && n != fclock_style_) {
        fclock_style_ = (int8_t)n;
        s_fcStyleIdx = n;
        Settings s(kThemeNs, true);
        s.SetInt("clk_style", n);
        fclock_last_min_ = 0xFFFF;   /* 强制 ClockTick 整屏重绘新配色 */
    }
    return fclock_style_;
}

const char* DafeiyuDisplay::WebThemeName(int id) const {
    return (id >= 0 && id < (int)PET_THEME_N) ? pet_theme_name((pet_theme_id_t)id) : "";
}

void DafeiyuDisplay::ApplyThemeSwitch(int dir) {
    if (!theme_ready_) return;
    /* 2026-11-06 用户定稿：双击音量键按当前视图分流 ——
       时钟主题下切「时钟样式」；壁纸没有样式，直接忽略；
       大肥鱼系列照旧切主题（9 套循环，绝不误入时钟/壁纸）。 */
    if (ext_theme_ == kThemeClockExt) {
        CycleClockStyle(dir);
        return;
    }
    if (ext_theme_ == kThemeWallpaperExt) {
        return;   /* 壁纸双击无效（用户要求） */
    }
    const int n = (int)PET_THEME_N;
    const int cur = (int)pet_theme_current();
    ApplyThemeTo(((cur + dir) % n + n) % n);
}

void DafeiyuDisplay::ApplyThemeTo(int id) {
    if (!theme_ready_) return;
    if (id < 0 || id >= (int)PET_THEME_N) {
        /* ── 扩展主题（100=翻页时钟 / 101=自定义壁纸）──
           不走 pet_theme_start：引擎那 9 套都是「角色 + 装饰」，而这两套
           没有角色。复用现成的 fclock 全屏浮层和壁纸图层，对外仍是同一
           主题 id 空间，菜单循环 / 双击音量键 / NVS 记忆全部通用。 */
        if (id == kThemeClockExt || id == kThemeWallpaperExt) {
            /* 先收掉当前扩展层（互斥），再铺目标层。
               fclock 浮层在 lv_layer_top 且全遮蔽，能直接盖住壁纸层；
               但壁纸层的显隐状态要收干净，否则切回普通主题时会残留。 */
            if (ext_theme_ == kThemeClockExt) {
                CloseClockMode();
            } else if (ext_theme_ == kThemeWallpaperExt) {
                CloseWallpaperTheme();
            }
            ext_theme_ = id;
            pet_theme_pause(true);   // 引擎装饰停掉，省 CPU/内存
            pet_theme_show(0);       /* ⚠️ 装饰容器(s_cont)也藏掉 —— 2026-11-06
                                        用户报「壁纸下左下角冒出肥鱼几秒」：角色
                                        (theme_pet_)和装饰容器是两个对象，之前只
                                        藏了角色，装饰层压在壁纸上露了出来。 */
            if (theme_pet_ != nullptr) {
                lv_obj_add_flag(theme_pet_, LV_OBJ_FLAG_HIDDEN);   // 角色让位
            }
            if (id == kThemeClockExt) {
                ShowClockMode();
            } else {
                ShowWallpaperTheme();
            }
            SaveCurrentTheme((pet_theme_id_t)id);   // 断电记忆同普通主题
            ESP_LOGI(TAG, "切扩展主题 -> %d", id);
        }
        return;
    }

    /* 从扩展主题切回大肥鱼系列：先收掉扩展层、恢复角色和引擎动画 */
    if (InExtTheme()) {
        if (ext_theme_ == kThemeClockExt) {
            CloseClockMode();
        }
        CloseWallpaperTheme();
        ext_theme_ = -1;
        if (theme_pet_ != nullptr) {
            lv_obj_remove_flag(theme_pet_, LV_OBJ_FLAG_HIDDEN);
        }
        pet_theme_pause(false);
        pet_theme_show(1);   /* 装饰容器跟角色一起回来 */
    }

    /* ⚠️ 2026-11-01 崩溃修复（addr2line 定位，勿删）：
       切主题前必须先把角色从 top layer 移回 screen。
       pet_theme_start() 内部第 822 行会 lv_obj_del(s_cont) 重建背景容器；
       而角色此前被 ApplyPetFix() 搬到了 lv_layer_top()，set_parent 会在
       LVGL 的事件链表里留下失效条目，于是这次删除走到
       lv_event_mark_deleted() 时遍历到野指针，直接 LoadProhibited 重启
       （实测表现为「双击切主题必重启」「开轮播时不断重启」）。
       顺序必须是：移回 screen → 切主题(删容器) → 再搬到 top layer。 */
    s_panic_trace.count++;
    PanicMark(1);   // 1 = 已算出目标主题，准备调 pet_theme_start

    /* 角色现在常驻 screen（见 ApplyPetFix），切主题前不再需要 set_parent
       移来移去 —— 而 set_parent 正是 lv_event_mark_deleted() 崩野指针的诱因。 */

    PanicMark(2);   // 2 = 准备调 pet_theme_start（内部会删旧容器）
    lv_obj_t *np = pet_theme_start((pet_theme_id_t)id, lv_scr_act());
    PanicMark(3);   // 3 = pet_theme_start 已返回（旧容器已删、新容器已建）
    if (np == nullptr) return;
    /* ⚠️ 不要调 pet_emotes_init()：s_pet 是复用的，而且重复 init 会累积
       动画/定时器 —— 之前开着轮播时机器不断重启就是这么来的。 */
    pet_emotes_set((pet_emote_t)pet_theme_action((pet_theme_id_t)id));
    PanicMark(4);   // 4 = 动作已设（会切换角色贴图）
    ApplyPetFix(np);
    PanicMark(5);   // 5 = 角色已搬回 top layer
    theme_pet_ = np;
    pet_emotes_rebase();
    /* 切完主题重新按开关设一次：关着就别让随机编排把配套动作覆盖掉。 */
    pet_idle_plan_enable(s_auto_emote_);
    PanicMark(0);   // 0 = 整个切换正常走完

    /* 切换成功后立刻写 NVS：断电/重启后开机默认就是这一套。
       放在所有步骤都成功之后 —— 崩在半路时不写，存档不会指向一个没建起来的主题。 */
    SaveCurrentTheme((pet_theme_id_t)id);
    ESP_LOGI(TAG, "切主题 -> %d %s", id, pet_theme_name((pet_theme_id_t)id));
}

void DafeiyuDisplay::HandlePetAction() {
    /* 养成玩法（PetGame / PetAction）已于 2026-11-01 删除，此函数不再做任何事。
       保留空壳是因为 tick 里仍在调用它，等旧界面清完再连调用点一起删。 */
}

void DafeiyuDisplay::UpdateStatusBars(bool force) {
    DisplayLockGuard lock(this);   /* 应用任务调用；理由同 SetEmojiCollection */
    if (hunger_bar_ == nullptr) {
        return;
    }
    if (!force && lv_tick_elaps(last_bar_ms_) < 5000) {
        return;
    }
    last_bar_ms_ = lv_tick_get();
    /* 养成面板已随 PetGame 删除，这几个养成条不再有真实数据，固定给满值。 */
    lv_bar_set_value(hunger_bar_, 100, LV_ANIM_OFF);
    lv_bar_set_value(mood_bar_, 100, LV_ANIM_OFF);
    if (bond_bar_ != nullptr) {
        lv_bar_set_value(bond_bar_, 100, LV_ANIM_OFF);
    }
    // 32 字节而不是 20：汉字在 UTF-8 里占 3 字节，「等级」俩字就 6 字节了，
    // 编译器按字节算截断，buf 小了会直接 -Werror
    char buf[32];
    snprintf(buf, sizeof(buf), "%u", 100u);
    lv_label_set_text(hunger_val_, buf);
    snprintf(buf, sizeof(buf), "%u", 100u);
    lv_label_set_text(mood_val_, buf);
    if (bond_val_ != nullptr) {
        snprintf(buf, sizeof(buf), "%u", 100u);
        lv_label_set_text(bond_val_, buf);
    }
    snprintf(buf, sizeof(buf), "等级%u 经验%u", 1u, 0u);
    lv_label_set_text(level_label_, buf);
}

void DafeiyuDisplay::UpdateClock() {
    if (clock_label_ == nullptr) {
        return;
    }
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    if (tm_info == nullptr || tm_info->tm_year < 2025 - 1900) {
        return;  // 还没对上时间（服务端没给 / SNTP 没同步），别显示假时间
    }
    if (!clock_logged_) {
        clock_logged_ = true;
        ESP_LOGI(TAG, "clock synced: %04d-%02d-%02d %02d:%02d", tm_info->tm_year + 1900,
                 tm_info->tm_mon + 1, tm_info->tm_mday, tm_info->tm_hour, tm_info->tm_min);
    }
    if (tm_info->tm_min == last_clock_min_) {
        return;  // 分钟没变就不刷文本
    }
    last_clock_min_ = tm_info->tm_min;

    char time_str[16];
    char date_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M", tm_info);
    strftime(date_str, sizeof(date_str), "%m/%d %a", tm_info);
    lv_label_set_text(clock_label_, time_str);
    lv_label_set_text(clock_date_label_, date_str);
}

void DafeiyuDisplay::SetClockVisible(bool visible) {
    if (clock_panel_ == nullptr) {
        return;
    }
    /* 主题自带时钟，接管后旧时钟卡片必须保持隐藏 ——
       否则 idle 时它会被显示出来盖在主题上。 */
    if (visible && !theme_ready_) {
        lv_obj_remove_flag(clock_panel_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(clock_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}

void DafeiyuDisplay::CreateBootScreen() {
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        return;
    }
    boot_panel_ = lv_obj_create(screen);
    lv_obj_set_size(boot_panel_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(boot_panel_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(boot_panel_, lv_color_hex(kScreenBgColor), 0);
    lv_obj_set_style_bg_opa(boot_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(boot_panel_, 0, 0);
    lv_obj_set_style_radius(boot_panel_, 0, 0);
    lv_obj_set_style_pad_all(boot_panel_, 0, 0);
    lv_obj_set_scrollbar_mode(boot_panel_, LV_SCROLLBAR_MODE_OFF);

    // 转圈：一段 270° 的弧，每 tick 推进 30°，1.5 秒转一圈
    boot_arc_ = lv_arc_create(boot_panel_);
    lv_obj_set_size(boot_arc_, 76, 76);
    lv_obj_align(boot_arc_, LV_ALIGN_CENTER, 0, -26);
    lv_arc_set_bg_angles(boot_arc_, 0, 360);
    lv_arc_set_value(boot_arc_, 75);
    lv_obj_remove_style(boot_arc_, nullptr, LV_PART_KNOB);
    lv_obj_set_style_arc_width(boot_arc_, 6, 0);
    lv_obj_set_style_arc_width(boot_arc_, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(boot_arc_, lv_color_hex(0x2A3348), 0);
    lv_obj_set_style_arc_color(boot_arc_, lv_color_hex(0x7C5CFF), LV_PART_INDICATOR);

    // 第二层环：更小、更暗、反向转。两层反向能让画面立刻不像「一个光秃秃的圈」，
    // 而且成本极低 —— 就是再来一个 arc，不动解帧、不占 PSRAM。
    boot_arc2_ = lv_arc_create(boot_panel_);
    lv_obj_set_size(boot_arc2_, 52, 52);
    lv_obj_align(boot_arc2_, LV_ALIGN_CENTER, 0, -26);
    lv_arc_set_bg_angles(boot_arc2_, 0, 360);
    lv_arc_set_value(boot_arc2_, 50);
    lv_obj_remove_style(boot_arc2_, nullptr, LV_PART_KNOB);
    lv_obj_set_style_arc_width(boot_arc2_, 4, 0);
    lv_obj_set_style_arc_width(boot_arc2_, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(boot_arc2_, lv_color_hex(0x1E2740), 0);
    lv_obj_set_style_arc_color(boot_arc2_, lv_color_hex(0x26D0CE), LV_PART_INDICATOR);

    // 中心呼吸光点：单纯的圆，靠透明度起伏，避免改尺寸触发重排。
    boot_core_ = lv_obj_create(boot_panel_);
    lv_obj_set_size(boot_core_, 18, 18);
    lv_obj_align(boot_core_, LV_ALIGN_CENTER, 0, -26);
    lv_obj_set_style_bg_color(boot_core_, lv_color_hex(0x7C5CFF), 0);
    lv_obj_set_style_bg_opa(boot_core_, LV_OPA_30, 0);
    lv_obj_set_style_border_width(boot_core_, 0, 0);
    lv_obj_set_style_radius(boot_core_, LV_RADIUS_CIRCLE, 0);

    lv_obj_t* title = lv_label_create(boot_panel_);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(kClockColor), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 28);
    lv_label_set_text(title, "DAFEIYU");

    boot_bar_ = lv_bar_create(boot_panel_);
    lv_obj_set_size(boot_bar_, 176, 6);
    lv_obj_align(boot_bar_, LV_ALIGN_CENTER, 0, 50);
    lv_bar_set_range(boot_bar_, 0, 100);
    lv_bar_set_value(boot_bar_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(boot_bar_, lv_color_hex(0x2A3348), 0);
    lv_obj_set_style_bg_opa(boot_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(boot_bar_, lv_color_hex(0x7C5CFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(boot_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(boot_bar_, 3, 0);

    boot_text_ = lv_label_create(boot_panel_);
    // ⚠️ 必须是含汉字那份字库！阶段文案是中文（扫描WiFi / 连接网络 / 待命），
    // 原来用的 lv_font_montserrat_14 是纯拉丁字体，汉字会整行渲染成空白。
    lv_obj_set_style_text_font(boot_text_, &font_noto_sans_basic_14_1, 0);
    lv_obj_set_style_text_color(boot_text_, lv_color_hex(kClockDateColor), 0);
    lv_obj_align(boot_text_, LV_ALIGN_CENTER, 0, 64);
    lv_label_set_text(boot_text_, "扫描WiFi");

    // 配网信息卡：默认藏起来，进配网模式时由 UpdateBootScreen 填内容并显示。
    // 存在的意义是替掉系统原生那条提示 —— 那条跟我们的动画叠在一起，信息重复且不美观。
    boot_cfg_label_ = lv_label_create(boot_panel_);
    lv_obj_set_style_text_font(boot_cfg_label_, &font_noto_sans_basic_14_1, 0);
    lv_obj_set_style_text_color(boot_cfg_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(boot_cfg_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(boot_cfg_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(boot_cfg_label_, LV_HOR_RES - 40);
    lv_obj_align(boot_cfg_label_, LV_ALIGN_CENTER, 0, -84);
    lv_label_set_text(boot_cfg_label_, "");
    lv_obj_add_flag(boot_cfg_label_, LV_OBJ_FLAG_HIDDEN);

    // 三个呼吸圆点，让纯色开场不那么死板
    static const lv_coord_t kDotsX[3] = {-24, 0, 24};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* dot = lv_obj_create(boot_panel_);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_align(dot, LV_ALIGN_CENTER, kDotsX[i], 84);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0x7C5CFF), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_40 + i * 25, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_radius(dot, 4, 0);
    }

    /* 过渡白幕：整屏白色，平时隐藏。开机动画收起时闪现一次再淡出，
       把「人物先出现、时钟后出现」这段突兀的换场盖掉。
       建在最后 → 天然位于最上层（LVGL 同层按创建顺序堆叠）。 */
    boot_flash_ = lv_obj_create(screen);
    lv_obj_remove_style_all(boot_flash_);
    lv_obj_set_size(boot_flash_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(boot_flash_, 0, 0);
    lv_obj_set_style_bg_color(boot_flash_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(boot_flash_, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(boot_flash_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(boot_flash_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(boot_flash_, LV_OBJ_FLAG_HIDDEN);

    boot_start_ms_ = lv_tick_get();
    ESP_LOGI(TAG, "boot screen created");
}

/* ── 开机换场过渡 ──
   背景：开机动画收起后，角色主题先出现、时钟/文案要晚一点才补齐，
   肉眼能看出「先出人物、后出时钟」两段。这里用一层整屏白幕把换场盖住，
   同时让角色从不透明 0 淡入 —— 视觉上就会是"闪一下 → 整体浮现"。 */
static lv_obj_t* s_boot_flash_obj = nullptr;

static void BootFlashOpaCb(void* obj, int32_t v) {
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
}

static void BootFlashDoneCb(lv_anim_t* a) {
    (void)a;
    if (s_boot_flash_obj != nullptr) {
        lv_obj_add_flag(s_boot_flash_obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 注意：不要给 theme_pet_ 挂动画。
   主题切换会把 pet 对象整个换掉（ApplyPetFix/切主题都会重建），
   动画变量一旦指向已删除的对象，就会在 lv_obj_delete 时踩坏事件链表 ——
   实测偶发 LoadProhibited。白幕全白时角色就在下层放出来，
   白幕淡出时它自然显现，视觉效果一样，但完全不用碰角色对象。 */

void DafeiyuDisplay::PlayBootTransition() {
    if (boot_flash_ == nullptr) {
        return;
    }
    /* 白幕：立刻全白并提到最上层，然后 620ms 淡出 ——
       淡出这段时间正好覆盖时钟/文案后到的过程。 */
    s_boot_flash_obj = boot_flash_;
    lv_obj_remove_flag(boot_flash_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(boot_flash_, LV_OPA_COVER, 0);
    lv_obj_move_foreground(boot_flash_);
    lv_anim_delete(boot_flash_, BootFlashOpaCb);   // 防止重复触发时叠加两条动画

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, boot_flash_);
    lv_anim_set_values(&a, static_cast<int32_t>(LV_OPA_COVER), 0);
    lv_anim_set_delay(&a, 50);
    lv_anim_set_duration(&a, 620);
    lv_anim_set_exec_cb(&a, BootFlashOpaCb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, BootFlashDoneCb);
    lv_anim_start(&a);

    /* 角色此时才放出来 —— 白幕正是不透明状态，观众看到的仍是白屏，
       白幕 620ms 淡出时角色自然浮现（不用给角色挂动画，理由见上方注释）。 */
    /* ⚠️ 2026-11-07 用户报「壁纸下左下角肥鱼还是出现」：白幕淡出时无条件
       放角色的这条路径就是元凶 —— tick 早把角色藏好切进壁纸主题了，
       700ms 后白幕收起这段又把它放出来（top layer，左下角站位）。
       扩展主题（时钟/壁纸）下绝不放，回肥鱼由 ApplyThemeTo 统一恢复。 */
    if (ext_theme_ < 0 && theme_pet_ != nullptr) {
        lv_obj_remove_flag(theme_pet_, LV_OBJ_FLAG_HIDDEN);
    }
}

void DafeiyuDisplay::UpdateBootScreen() {
    if (boot_panel_ == nullptr) {
        return;
    }
    if (boot_finished_) {
        // 素材就绪：进度拉满，稍等一下再收起来，让"完成"看得见。
        // 注意只隐藏不删除：这里删对象会踩到 LVGL 的全局事件链表
        // （lv_event_mark_deleted 遍历时野指针），实测必崩
        if (boot_finish_ms_ != 0 && lv_tick_elaps(boot_finish_ms_) > 700) {
            lv_obj_add_flag(boot_panel_, LV_OBJ_FLAG_HIDDEN);
            boot_finish_ms_ = 0;
            PlayBootTransition();     // 白幕闪一下 + 角色淡入，抹平换场
        }
        return;
    }
    boot_spin_ = (boot_spin_ + 30) % 360;
    lv_arc_set_rotation(boot_arc_, boot_spin_);

    // 内层反向转
    if (boot_arc2_ != nullptr) {
        lv_arc_set_rotation(boot_arc2_, (360 - boot_spin_) % 360);
    }
    // 中心呼吸：来回数相位再映射到不透明度。
    // 不用 sin —— 一来省 <cmath>，二来查整型比浮点便宜，这个每 tick 都要跑。
    boot_breath_ = (boot_breath_ + 1) % 40;
    if (boot_core_ != nullptr) {
        const int d = (boot_breath_ < 20) ? boot_breath_ : 40 - boot_breath_;  // 0..20
        lv_obj_set_style_bg_opa(boot_core_,
                                static_cast<lv_opa_t>(LV_OPA_30 + d * 6), 0);  // 30%..~90%
    }

    const uint32_t elapsed = lv_tick_elaps(boot_start_ms_);
    int pct;
    // ⚠️ 这些文案必须是中文时用得起的字体 —— CreateBootScreen 里给 boot_text_ 配的是
    // font_noto_sans_basic_14_1（含汉字）。千万别换成 montserrat：那是纯拉丁字体，
    // 汉字会整行渲染成空白，跟之前 notification_label_ 踩的那个坑一模一样。
    const char* phase = "扫描WiFi";
    if (elapsed < 3000) {
        pct = static_cast<int>(elapsed * 20 / 3000);              // 0~20  等 Wi-Fi
    } else if (elapsed < 7000) {
        // 连上了就把 AP 名字显示出来 —— 用户明确要求，
        // 光写「连接网络」看不出连的是哪个。取不到时才退回通用文案。
        // 用 static：phase 是个指针，函数返回后还要被 lv_label_set_text 用到。
        static char ap_text[48] = "连接网络";
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            char ssid[33] = {0};
            memcpy(ssid, ap.ssid, sizeof(ap.ssid));   // ssid 不保证 '\0' 结尾
            if (ssid[0] != '\0') {
                snprintf(ap_text, sizeof(ap_text), "连接 %s", ssid);
            }
        }
        phase = ap_text;
        pct = 20 + static_cast<int>((elapsed - 3000) * 30 / 4000);  // 20~50 连服务端
    } else {
        // 2026-11-06 用户要求屏幕全局不显示 IP：开机进度条也不显示，只报状态
        phase = "载入素材";
        pct = 50 + static_cast<int>(std::min<uint32_t>(45, (elapsed - 7000) * 45 / 9000));
    }
    lv_bar_set_value(boot_bar_, pct, LV_ANIM_OFF);
    lv_label_set_text(boot_text_, phase);

    // ── 配网模式：把配网信息直接融进这个动画，替掉系统原生那条提示 ──────────
    // 为什么会重叠：配网发生在 assets 分区挂载之前，boot_finished_ 一直是 false，
    // 于是开机动画赖着不走；而系统同时又自己在别处显示配网提示，两层信息叠在一起。
    // 这里不去动系统那条（隐藏/删除它容易踩 LVGL 的坑），而是让动画自己把热点名和
    // 访问地址讲清楚，视觉上由我们这一块主导。
    {
        wifi_mode_t mode = WIFI_MODE_NULL;
        const bool ap_now = (esp_wifi_get_mode(&mode) == ESP_OK) &&
                            (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
        // ⚠️ 必须判「持续时长」，不能只看当前这一刻：
        // WiFi 初始化过程中会短暂处于 AP/APSTA 模式，直接按瞬时状态判断的话，
        // 配网文字会在每次开机时闪一下（用户实测到的就是这个）。
        // 连续 2 秒都还在 AP 模式，才算真的进了配网。
        static uint32_t ap_since_ms = 0;
        if (ap_now) {
            if (ap_since_ms == 0) {
                ap_since_ms = lv_tick_get();
            }
        } else {
            ap_since_ms = 0;
        }
        const bool in_ap = ap_now && (ap_since_ms != 0) &&
                           (lv_tick_elaps(ap_since_ms) > 2000);
        if (in_ap) {
            wifi_config_t cfg = {};
            char ssid[33] = {0};
            if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
                // ap.ssid 是 32 字节且不保证以 '\0' 结尾，必须拷进自己的缓冲区
                memcpy(ssid, cfg.ap.ssid, sizeof(cfg.ap.ssid));
            }
            static char info[96];
            if (ssid[0] != '\0') {
                snprintf(info, sizeof(info), "手机连接热点 %s\n浏览器访问 192.168.4.1", ssid);
            } else {
                snprintf(info, sizeof(info), "浏览器访问 192.168.4.1");
            }
            if (boot_cfg_label_ != nullptr) {
                lv_label_set_text(boot_cfg_label_, info);
                lv_obj_remove_flag(boot_cfg_label_, LV_OBJ_FLAG_HIDDEN);
            }
            // 覆盖掉上面那句阶段文案 —— 配网时它还在显示「载入素材」，会误导
            lv_label_set_text(boot_text_, "配网模式");
        } else if (boot_cfg_label_ != nullptr) {
            lv_obj_add_flag(boot_cfg_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void DafeiyuDisplay::FinishBootScreen() {
    if (boot_panel_ == nullptr || boot_finished_) {
        return;
    }
    boot_finished_ = true;
    boot_finish_ms_ = lv_tick_get();
    lv_bar_set_value(boot_bar_, 100, LV_ANIM_OFF);
    lv_label_set_text(boot_text_, "待命");   // 中文：字体必须是 basic 那份，见 UpdateBootScreen

    /* ⚠️ 2026-11-02：这里绝对不能立刻放出角色。
       原来在此 remove_flag(HIDDEN)，而白幕要等 700ms 后机动画收面板时才触发，
       于是角色比白幕先露脸 —— 用户看到的就是「角色在白幕之前出现了」。
       现在保持隐藏，等 PlayBootTransition() 里连白幕一起淡入。
       兜底：万一白幕对象没建起来，还是按老逻辑立刻显示，别让角色一直藏着。 */
    if (boot_flash_ == nullptr && ext_theme_ < 0 && theme_pet_ != nullptr) {
        lv_obj_remove_flag(theme_pet_, LV_OBJ_FLAG_HIDDEN);
    }
}

void DafeiyuDisplay::ShowSystemMessage(const char* text) {
    if (sys_label_ == nullptr) {
        return;
    }
    if (text == nullptr || text[0] == '\0') {
        lv_obj_add_flag(sys_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sys_code_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(sys_label_, text);
    lv_obj_remove_flag(sys_label_, LV_OBJ_FLAG_HIDDEN);
    sys_msg_ms_ = lv_tick_get();  // 20 秒后自动收掉，别永久挂在屏幕上

    // 消息里那串连续数字可能是激活验证码，单独放大显示。
    // 但这条通道也过素材下载进度（"42% 200KB/s"），所以两边都不能是
    // 字母 / % / / / . / 空格紧邻，否则会把 “1234KB/s” 认成验证码
    char code[16] = {0};
    size_t best = 0;
    const char* p = text;
    while (*p != '\0') {
        size_t n = 0;
        while (p[n] >= '0' && p[n] <= '9') {
            n++;
        }
        if (n >= 4 && n > best) {  // 至少 4 位才候选
            const char prev = (p == text) ? '\0' : p[-1];
            const char next = p[n];
            auto noisy = [](char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '%' ||
                       c == '/' || c == '.' || c == ':' || c == 'x' || c == '*';
            };
            if (!noisy(prev) && !noisy(next)) {
                best = n;
                const size_t len = n < sizeof(code) - 1 ? n : sizeof(code) - 1;
                memcpy(code, p, len);
                code[len] = '\0';
            }
        }
        p += (n > 0) ? n : 1;
    }
    if (best > 0) {
        lv_label_set_text(sys_code_label_, code);
        lv_obj_remove_flag(sys_code_label_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "system message with code: %s", code);
    } else {
        lv_obj_add_flag(sys_code_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

uint32_t DafeiyuDisplay::notify_count_ = 0;
char DafeiyuDisplay::last_notify_buf_[64] = "";

// ── 追查「音量胶囊不出现」────────────────────────────────────────────────
// 接管而非改基类：既能记诊断数据，又不影响别的界面的行为。
void DafeiyuDisplay::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);   /* 应用任务调用，会新建 LVGL 对象 */
    notify_count_++;
    snprintf(last_notify_buf_, sizeof(last_notify_buf_), "%s",
             notification != nullptr ? notification : "(null)");
    ESP_LOGW(TAG, "[追胶囊] 调用 #%u dur=%dms text=\"%s\"",
             (unsigned)notify_count_, duration_ms, last_notify_buf_);

    // ⚠️ 2026-10-31 用户实测确认：开机阶段那条横条就是 notification_label_，
    // 而且它显示的内容（连接 WiFi、连上 AP 之类）跟开机动画里的阶段文案完全重复
    // —— 于是屏幕上同时有两条一模一样的信息，一条在动画里，一条是这条横条。
    // 用户明确说：动画下方已经显示信息了，就不需要这条横条再来一遍。
    // 所以开机未完成时直接拦掉，不交给基类显示；开机结束后才恢复，
    // 那时它承载的是音量调节这类真正需要弹的提示。
    if (!boot_finished_) {
        return;
    }

    SpiLcdDisplay::ShowNotification(notification, duration_ms);

    // 每次显示前把样式钉一遍：用户反馈「文字还是黑色」，
    // 说明别处有代码在改它，只在 SetupUI 里设一次不够保险。
    if (notification_label_ != nullptr) {
        lv_obj_set_style_text_color(notification_label_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(notification_label_, LV_OPA_40, 0);
        lv_obj_move_foreground(notification_label_);
    }

    // 基类设完文本会 remove HIDDEN，但它建得比全屏立绘早；这里补一次提层，
    // 免得只是被 pet_image_ 压住（之前单独在 SetupUI 末尾提层验证过无效，
    // 留着这一句是防御，真正的结论看 notify 计数是否被调起来）。
    if (notification_label_ != nullptr) {
        /* ⚠️ 2026-11-06 用户实测「时钟下双击 AEC 看不到反馈」：通知建在
           screen 层，而时钟/壁纸扩展主题的浮层在 lv_layer_top 且全遮蔽 ——
           通知弹了也被盖死。挪到 top layer 再提最前，永远压在一切之上。 */
        if (lv_obj_get_parent(notification_label_) != lv_layer_top()) {
            lv_obj_set_parent(notification_label_, lv_layer_top());
        }
        lv_obj_move_foreground(notification_label_);
    }
}

void DafeiyuDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void DafeiyuDisplay::UpdateStatusBar() {
    DisplayLockGuard lock(this);   /* 应用任务调用；理由同 SetEmojiCollection */
    if (sys_bar_ == nullptr) {
        return;
    }
    if (lv_tick_elaps(last_status_ms_) < 3000) {
        return;
    }
    last_status_ms_ = lv_tick_get();

    // 电量走板子的标准接口（cam 板已经实现了 GetBatteryLevel），
    // 别在这里自己读 ADC —— 那边有均值滤波和分档表，重复造轮子只会得到不准的数。
    int level = 0;
    bool charging = false, discharging = false;
    if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        /* 电量百分比数值已按用户要求去掉（label 在 SetupUI 里就已隐藏），
           这里只更新进度条宽度和颜色。 */

        // 外框 22px、边框 1px、pad 1px → 内部可用宽度 18px 表示 100%
        int w = 18 * level / 100;
        if (w < 2) {
            w = 2;
        }
        lv_obj_set_width(battery_fill_, w);

        uint32_t col = 0x6BE36B;                    // 正常：绿
        if (charging) {
            col = 0x5AA9FF;                         // 充电：蓝
        } else if (level <= 20) {
            col = 0xFF6B6B;                         // 低电量：红
        }
        lv_obj_set_style_bg_color(battery_fill_, lv_color_hex(col), 0);
    }

    // WiFi 信号强度：直接问驱动要 RSSI，比「有没有 IP」靠谱得多 ——
    // 「连上了但信号很弱」是很常见的状态，只看 IP 完全看不出来。
    int lit = 0;
    bool connected = false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        connected = true;
        if (ap.rssi >= -55) {
            lit = 3;      // 强
        } else if (ap.rssi >= -70) {
            lit = 2;      // 中
        } else if (ap.rssi >= -85) {
            lit = 1;      // 弱
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (wifi_bars_[i] == nullptr) {
            continue;
        }
        lv_obj_set_style_bg_opa(wifi_bars_[i], i < lit ? LV_OPA_COVER : LV_OPA_20, 0);
        // 整组变红 = 根本没连上；白色点亮格数 = 连着但强弱不同。一眼能区分。
        lv_obj_set_style_bg_color(wifi_bars_[i],
                                  lv_color_hex(connected ? 0xFFFFFF : 0xFF6B6B), 0);
    }

    /* 壁纸主题的左上大时钟（2026-11-06 用户要求：时间放左边）。
       只在壁纸主题可见；分钟没变不重写。 */
    if (wp_clock_label_ != nullptr &&
        !lv_obj_has_flag(wp_clock_label_, LV_OBJ_FLAG_HIDDEN)) {
        time_t nowt = time(NULL);
        struct tm* ti = localtime(&nowt);
        if (ti != nullptr && ti->tm_year >= 2025 - 1900) {
            if (last_wp_min_ != (uint16_t)ti->tm_min) {
                last_wp_min_ = (uint16_t)ti->tm_min;
                char t[8];
                strftime(t, sizeof(t), "%H:%M", ti);
                lv_label_set_text(wp_clock_label_, t);
            }
        }
    }
}

/* ====================== 音乐可视化（2026-11-05） ====================== */

void DafeiyuDisplay::BuildSpectrum() {
    if (spec_root_ != nullptr) {
        return;
    }
    lv_obj_t* screen = lv_screen_active();
    spec_root_ = lv_obj_create(screen);
    lv_obj_remove_style_all(spec_root_);
    /* 2026-11-06 改版：对话时顶替唤醒球，居中显示，
       待机时整个容器隐藏 —— 屏底不再常驻。 */
    lv_obj_set_size(spec_root_, 240, 72);
    lv_obj_align(spec_root_, LV_ALIGN_CENTER, 0, -12);
    lv_obj_remove_flag(spec_root_, LV_OBJ_FLAG_CLICKABLE);

    /* 2026-11-06 性能重做（用户反馈放音乐声音卡）：弃用 8 个 lv_obj 圆角柱
       （每帧半透明圆角混合重绘 + 大面积刷屏，挤占音频解码的内存带宽），
       改成参考 esp32_-fft_-tft 的做法 —— 单 canvas 像素直绘，
       一次 invalidate、零混合零圆角，渲染成本降一个量级。 */
    spec_buf_ = (uint16_t*)heap_caps_calloc(
        240 * 72, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (spec_buf_ != nullptr) {
        spec_canvas_ = lv_canvas_create(spec_root_);
        lv_canvas_set_buffer(spec_canvas_, spec_buf_, 240, 72,
                             LV_COLOR_FORMAT_RGB565);

        /* ── 2026-11-06 起播卡顿优化（用户反馈「刚刚播放时候还是卡顿」）──
           把两笔「第一次做会卡一下」的活全部挪到开机这里：
           ① 画布铺底（35KB 像素写入，原来在频谱首次显示时做）；
           ② FFT 工作区懒分配（4KB PSRAM + 1KB 内部 SRAM）与 256 次 cosf
              建表（原来在音乐起播的第一个 FFT 帧做）。
           这样点歌瞬间只剩「解码启动 + 网络突发 + 画柱」，不再三件事叠加。 */
        for (int i = 0; i < 240 * 72; i++) {
            spec_buf_[i] = 0x0000;      /* 铺底（SpecFill 定义在后面，这里直接填） */
        }
        static int16_t warm_pcm[SPEC_FFT_N] = {0};
        static float warm_bars[SPEC_BARS];
        spec_fft_compute_bars(warm_pcm, warm_bars);
    }
    lv_obj_add_flag(spec_root_, LV_OBJ_FLAG_HIDDEN);
}

/* 电平数据说明：orb_peek 是非消耗读。注意唤醒球用的是 take（消耗 seq），
   两者互不干扰 —— peek 永远能拿到最新值。
   2026-11-06 改版行为（用户定案）：
   - 只在**放音乐**时（MCP self.music.now / self.music.playing 上报）出现，
     居中顶替唤醒球；对话说话（TTS）永远用圆圈；
   - 待机时完全隐藏；
   - 音乐间隙（静音 900ms）圆圈回来，出声再切频谱。 */
/* ── 频谱绘制小工具（只在 spec_buf_ 上写像素）──
   颜色 0xRRGGBB → RGB565 */
static inline uint16_t SpecRgb(uint32_t c) {
    return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x1F));
}
static inline void SpecFill(uint16_t* buf, int x0, int x1, int y0, int y1,
                            uint16_t c) {
    for (int y = y0; y <= y1; y++) {
        uint16_t* row = buf + y * 240;
        for (int x = x0; x <= x1; x++) {
            row[x] = c;
        }
    }
}

void DafeiyuDisplay::UpdateSpectrum() {
    if (spec_root_ == nullptr) {
        return;
    }
    const uint32_t now = lv_tick_get();

    /* 音乐没放/开关关着：完全不读音频、不重绘 —— 零开销（顺便修音乐卡顿） */
    if (!GetConfig().spectrum_enabled || !music_playing_) {
        if (spec_visible_) {
            spec_visible_ = false;
            if (wake_orb_lv_ != nullptr && wake_orb_lv_->canvas != nullptr) {
                lv_obj_remove_flag(wake_orb_lv_->canvas, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_add_flag(spec_root_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    /* ── 2026-11-06 起播错峰（用户反馈「刚刚播放时候还是卡顿」）──
       点歌后前 600ms 先不出频谱：这段时间要同时做解码启动 + 网络突发 +
       歌名字体渲染，首帧画布再挤进去就容易把音频解码的带宽挤没。
       600ms 后再出现，用户基本察觉不到。 */
    static bool last_music = false;
    static uint32_t music_start_ms = 0;
    if (music_playing_ && !last_music) {
        music_start_ms = now;
    }
    last_music = music_playing_;
    if (now - music_start_ms < 600) {
        return;
    }

    /* 50ms 节流（20fps）：FFT 开销减半，肉眼无差别 —— 音频略卡的减负项 */
    static uint32_t last_upd_ms = 0;
    if (spec_visible_ && now - last_upd_ms < 50) {
        return;
    }
    last_upd_ms = now;

    /* 真 FFT 数据源（2026-11-06，按 WorkBuddy 交接文档）：从输出环形缓冲
       取 512 点 → 16 个对数频段柱值。FFT 在显示任务算（音频任务只 memcpy）。 */
    static int16_t frame[SPEC_FFT_N];
    static float bars[SPEC_BARS];
    const bool have = spec_fft_read_latest(frame, SPEC_FFT_N) != 0;
    if (have) {
        spec_fft_compute_bars(frame, bars);
    }
    float mx = 0.0f;                       /* 只给自动增益用 */
    for (int b = 0; b < SPEC_BARS; b++) if (bars[b] > mx) mx = bars[b];
    /* 2026-11-06（用户反馈「偶尔圆圈会出来」）：放歌全程频谱常驻 ——
       音乐有轻的段落，按电平回落圆圈会来回闪。柱子走平滑自然落底。
       能走到这里说明 spectrum_enabled && music_playing_ 都成立。 */
    if (!spec_visible_) {
        spec_visible_ = true;
        if (wake_orb_lv_ != nullptr && wake_orb_lv_->canvas != nullptr) {
            lv_obj_add_flag(wake_orb_lv_->canvas, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(spec_root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(spec_root_);
        /* 铺底已在 BuildSpectrum（开机）做过，这里只重画 */
        lv_obj_invalidate(spec_canvas_);
    }
    if (spec_buf_ == nullptr) {
        return;
    }

    /* ── 自动增益（包络峰值阻尼归一化），防止「全顶格」或「全趴底」── */
    static float g_peak = 0.12f;
    g_peak *= 0.90f;
    if (mx > g_peak) g_peak = mx;
    if (g_peak < 0.12f) g_peak = 0.12f;

    /* 极光渐变 16 色：青 → 紫 → 粉（与大肥鱼角色主色一套），静态初始化一次 */
    static uint16_t col_r[16], col_g[16], col_b[16];
    static bool col_ready = false;
    if (!col_ready) {
        for (int i = 0; i < 16; i++) {
            const float f = (float)i / 15.0f;
            float r, g, b;
            if (f < 0.5f) { const float k = f * 2.0f;
                r = 0x2F + (0x7B - 0x2F) * k; g = 0xE0 + (0x5A - 0xE0) * k; b = 0xFF; }
            else          { const float k = (f - 0.5f) * 2.0f;
                r = 0x7B + (0xFF - 0x7B) * k; g = 0x5A + (0x4A - 0x5A) * k; b = 0xFF + (0x8C - 0xFF) * k; }
            col_r[i] = (uint16_t)r; col_g[i] = (uint16_t)g; col_b[i] = (uint16_t)b;
        }
        col_ready = true;
    }

    /* 柱几何：16 根 × 12px 宽 + 15 缝 × 3px = 237，kX0=1；
       基线 y=68，最大柱高 62（峰帽画在 y≥4）。
       纵向渐变：柱子从底 35% 亮度线性升到顶 100%，逐行整数乘法算 RGB565。 */
    constexpr int kW = 240, kH = 72;
    constexpr int kBarW = 12, kGap = 3, kX0 = 1;
    constexpr int kBaseY = 68, kMaxH = 62;
    bool changed = false;
    for (int i = 0; i < SPEC_BARS; i++) {
        float v = bars[i] / g_peak;
        if (v > 1.0f) v = 1.0f;
        int h = (int)(v * (float)kMaxH);
        if (h < 2) h = 2;
        /* 快起慢落：上升直达，下降每次 -3px */
        if (h > spec_h_[i]) {
            spec_h_[i] = (uint8_t)h;
        } else if (spec_h_[i] > 3) {
            spec_h_[i] -= 3;
        }
        /* 峰值保持：柱顶上方留个白帽，比柱子慢 1px/帧 滑落 */
        if (spec_h_[i] > spec_peak_[i]) {
            spec_peak_[i] = spec_h_[i];
        } else if (spec_peak_[i] > spec_h_[i] + 1) {
            spec_peak_[i] -= 1;
        }
        if (spec_peak_[i] < spec_h_[i]) {
            spec_peak_[i] = spec_h_[i];
        }

        const int x0 = kX0 + i * (kBarW + kGap);
        const int x1 = x0 + kBarW - 1;
        SpecFill(spec_buf_, x0, x1, 0, kH - 1, 0x0000);   // 清列
        const int bh = spec_h_[i];
        if (bh > 0) {
            const int top = kBaseY - bh;
            for (int y = top; y < kBaseY; y++) {
                /* 底 35% → 顶 100% 亮度（sc 为 ×256 定点） */
                const int t = bh - 1 - (y - top);          // 0=底
                const uint32_t sc = 89 + (166 * t) / (bh > 1 ? bh - 1 : 1);
                const uint32_t r8 = (col_r[i] * sc) >> 8;  // 0..255
                const uint32_t g8 = (col_g[i] * sc) >> 8;
                const uint32_t b8 = (col_b[i] * sc) >> 8;
                const uint16_t c = (uint16_t)(((r8 & 0xF8) << 8) |
                                              ((g8 & 0xFC) << 3) |
                                              (b8 >> 3));
                uint16_t* row = spec_buf_ + y * kW;
                for (int x = x0; x <= x1; x++) row[x] = c;
            }
        }
        if (spec_peak_[i] > 2) {
            const int py = kBaseY - spec_peak_[i] - 1;
            if (py >= 0) SpecFill(spec_buf_, x0, x1, py, py, 0xFFFF);  // 白峰帽
        }
        changed = true;
    }
    if (changed) {
        /* 只 invalidate canvas，不整屏 —— 这是省带宽的关键 */
        lv_obj_invalidate(spec_canvas_);
    }
}

/* 控制页二维码浮层：菜单「控制页」项打开，BOOT 单击关闭。
   实现说明：不用 LVGL 自带 lv_qrcode（CONFIG_LV_USE_QRCODE 与 esp_emote_gfx
   里的 qrcodegen.c 符号冲突），用后者公开的 qrcodegen API 生成点阵，
   手动画进 LVGL canvas。文案全部用 ASCII（montserrat），避免中文子集缺字。 */
void DafeiyuDisplay::ShowControlPageQr() {
    lv_obj_t* parent = lv_layer_top();
    if (qr_root_ == nullptr) {
        qr_root_ = lv_obj_create(parent);
        lv_obj_remove_style_all(qr_root_);
        lv_obj_set_size(qr_root_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(qr_root_, 0, 0);
        lv_obj_set_style_bg_color(qr_root_, lv_color_hex(0x05070C), 0);
        lv_obj_set_style_bg_opa(qr_root_, LV_OPA_90, 0);
        lv_obj_remove_flag(qr_root_, LV_OBJ_FLAG_SCROLLABLE);

        qr_url_label_ = lv_label_create(qr_root_);
        lv_obj_set_style_text_font(qr_url_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(qr_url_label_, lv_color_hex(0x2FE0FF), 0);
        lv_obj_set_style_text_align(qr_url_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(qr_url_label_, LV_HOR_RES - 30);
    }
    constexpr int kQrW = 150;
    constexpr int kQrH = 150;
    if (qr_canvas_ == nullptr) {
        qr_canvas_buf_ = (uint16_t*)heap_caps_calloc(
            kQrW * kQrH, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (qr_canvas_buf_ != nullptr) {
            qr_canvas_ = lv_canvas_create(qr_root_);
            lv_canvas_set_buffer(qr_canvas_, qr_canvas_buf_, kQrH, kQrH,
                                 LV_COLOR_FORMAT_RGB565);
        }
    }
    char url[48];
    if (ip_text_[0] != '\0') {
        snprintf(url, sizeof(url), "http://%s/", ip_text_);
    } else {
        snprintf(url, sizeof(url), "NO-IP");   // 还没拿到 IP：二维码先空着
    }

    bool ok = false;
    if (qr_canvas_buf_ != nullptr) {
        uint8_t qrcode[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
        uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
        ok = qrcodegen_encodeText(url, tmp, qrcode, qrcodegen_Ecc_LOW,
                                  1, 6, qrcodegen_Mask_AUTO, true);
        if (ok) {
            const int size = qrcodegen_getSize(qrcode);
            /* 两侧各留 2 个模块静区，模块放大到铺满画布 */
            const int scale = kQrW / (size + 4);
            const int off = (kQrW - size * scale) / 2;
            for (int y = 0; y < kQrH; y++) {
                uint16_t* row = qr_canvas_buf_ + y * kQrW;
                for (int x = 0; x < kQrW; x++) {
                    row[x] = 0xFFFF;   // 白底
                }
            }
            for (int my = 0; my < size; my++) {
                for (int mx = 0; mx < size; mx++) {
                    if (!qrcodegen_getModule(qrcode, mx, my)) {
                        continue;
                    }
                    for (int dy = 0; dy < scale; dy++) {
                        uint16_t* row = qr_canvas_buf_ +
                                        (off + my * scale + dy) * kQrW;
                        for (int dx = 0; dx < scale; dx++) {
                            row[off + mx * scale + dx] = 0x0000;   // 黑模块
                        }
                    }
                }
            }
            lv_obj_invalidate(qr_canvas_);
        }
    }
    lv_label_set_text(qr_url_label_, ok ? url : "QR-FAIL");
    if (qr_canvas_ != nullptr) {
        lv_obj_align(qr_canvas_, LV_ALIGN_CENTER, 0, -14);
    }
    lv_obj_align(qr_url_label_, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_remove_flag(qr_root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(qr_root_);
    qr_visible_ = true;
    ESP_LOGI(TAG, "控制页二维码: %s (ok=%d)", url, ok ? 1 : 0);
}

/* ═══════════════════ 翻页时钟模式（2026-11-06）═══════════════════
   按 WorkBuddy 交接文档《翻页时钟固件实现方案》落地：
   全屏 320x240 canvas 像素直绘（与频谱同一套路子，零 LVGL 混合开销），
   aurora 皮肤：4 张数字卡（54x72，缝 8，冒号区 16，X0=28，Y0=52）+ 日期行。
   分钟变化时只有变化的卡翻页：两段式 320ms（上半旧字保持→换新字；下半
   旧字向铰链收合→新字展开），级联 110ms/卡（个位先动）。
   冒号 1Hz 闪烁。稳态每秒只重绘冒号小区域，翻页每秒最多 10 帧卡区域。 */

static const uint8_t kFlipDigitFont[10][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  /* 0 */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  /* 1 */
    {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F},  /* 2 */
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  /* 3 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  /* 4 */
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  /* 5 */
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  /* 6 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  /* 7 */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  /* 8 */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  /* 9 */
};

/* 卡片几何（320x240 横屏） */
static constexpr int kFcW = 320, kFcH = 240;
static constexpr int kFcCardW = 54, kFcCardH = 72, kFcCardY = 52;
static constexpr int kFcCardX[4] = {28, 90, 175, 237};
static constexpr int kFcHinge = kFcCardY + kFcCardH / 2;   /* 88：数字上下半分界 */
static constexpr int kFcDigitX = 7, kFcDigitY = 8;         /* 卡内内边距 */

/* 极光四卡数字色：跨卡 青 → 紫 → 粉 插值（取各卡中心）。
   2026-11-06 起配色改由 kFcStyles 样式表管理，这表只留作颜色速查。 */
static const uint8_t kFcColor[4][3] = {
    {85, 193, 255}, {104, 124, 255}, {154, 86, 246}, {222, 80, 169},
};

static inline uint16_t FlipRgb565(uint32_t r, uint32_t g, uint32_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* 位图数字按行区间画（scale256 = 纵向压缩，256 为 1:1；sc = 亮度 ×256） */
static void FlipDrawDigitRows(uint16_t* buf, int x, int yTop, int d, int rowFrom,
                              int rowTo, uint32_t scale256, uint32_t sc,
                              uint32_t r, uint32_t g, uint32_t b) {
    if (d < 0 || d > 9 || scale256 == 0) return;
    const int rows = rowTo - rowFrom + 1;
    const int outH = (int)(((uint32_t)rows * scale256) >> 8);
    const uint16_t c = FlipRgb565((r * sc) >> 8, (g * sc) >> 8, (b * sc) >> 8);
    for (int dy = 0; dy < outH; dy++) {
        const int srcRow = rowFrom + (int)(((uint32_t)dy * 256) / scale256);
        if (srcRow > rowTo) break;
        const uint8_t row = kFlipDigitFont[d][srcRow / 8];
        const int y = yTop + dy;
        if (y < 0 || y >= kFcH) continue;
        uint16_t* line = buf + y * kFcW + x;
        for (int gx = 0; gx < 5; gx++) {
            if (!(row & (0x10 >> gx))) continue;
            uint16_t* p = line + gx * 8;
            for (int px = 0; px < 8; px++) p[px] = c;
        }
    }
}

/* 画一整张卡（pMs < 0 = 静态；否则为翻页进度 0~320ms）。颜色从样式表取 */
static void FlipPaintCard(uint16_t* buf, int idx, int dOld, int dNew, int pMs) {
    const FcStyle& st = kFcStyles[s_fcStyleIdx];
    const uint32_t r = st.digit[idx][0], g = st.digit[idx][1], b = st.digit[idx][2];
    const int cx = kFcCardX[idx];
    /* 卡底 + 1px 描边（卡片色 40%） */
    const uint16_t bg = FlipRgb565(st.cardBg[0], st.cardBg[1], st.cardBg[2]);
    const uint16_t bd = FlipRgb565((r * 102) >> 8, (g * 102) >> 8, (b * 102) >> 8);
    for (int y = kFcCardY; y < kFcCardY + kFcCardH; y++) {
        uint16_t* line = buf + y * kFcW + cx;
        const bool edgeRow = (y == kFcCardY || y == kFcCardY + kFcCardH - 1);
        for (int x = 0; x < kFcCardW; x++) {
            line[x] = (edgeRow || x == 0 || x == kFcCardW - 1) ? bd : bg;
        }
    }
    /* 铰链缝 1px：平时卡片色 45%，翻页中亮白 */
    {
        const uint16_t hinge = (pMs >= 0)
            ? FlipRgb565(230, 232, 240)
            : FlipRgb565((r * 115) >> 8, (g * 115) >> 8, (b * 115) >> 8);
        uint16_t* line = buf + kFcHinge * kFcW + cx + 1;
        for (int x = 0; x < kFcCardW - 2; x++) line[x] = hinge;
    }
    const int dx = cx + kFcDigitX;
    const int dy = kFcCardY + kFcDigitY;
    if (pMs < 0) {
        /* 静态：整字（亮度略降让卡片色更耐看） */
        FlipDrawDigitRows(buf, dx, dy, dNew, 0, 55, 256, 235, r, g, b);
        return;
    }
    if (pMs < 160) {
        /* 阶段一：上半旧字保持，下半旧字向铰链收合 + 变暗 */
        FlipDrawDigitRows(buf, dx, dy, dOld, 0, 27, 256, 235, r, g, b);
        const uint32_t s = (uint32_t)((160 - pMs) * 256) / 160;
        const uint32_t sc = (uint32_t)(256 - (uint32_t)pMs * 141 / 160);
        FlipDrawDigitRows(buf, dx, kFcHinge, dOld, 28, 55, s, sc, r, g, b);
    } else {
        /* 阶段二：上半换新字，下半新字从铰链展开 + 变亮 */
        FlipDrawDigitRows(buf, dx, dy, dNew, 0, 27, 256, 235, r, g, b);
        const uint32_t s = (uint32_t)((pMs - 160) * 256) / 160;
        const uint32_t sc = (uint32_t)(115 + (uint32_t)(pMs - 160) * 141 / 160);
        FlipDrawDigitRows(buf, dx, kFcHinge, dNew, 28, 55, s, sc, r, g, b);
    }
}

/* 冒号区（x 144~174）重绘：亮=两个方块，灭=只有背景（颜色随样式） */
static void FlipPaintColon(uint16_t* buf, bool on) {
    const FcStyle& st = kFcStyles[s_fcStyleIdx];
    const uint16_t bg = FlipRgb565(st.bg[0], st.bg[1], st.bg[2]);
    for (int y = kFcCardY; y < kFcCardY + kFcCardH; y++) {
        uint16_t* line = buf + y * kFcW + 144;
        for (int x = 0; x < 31; x++) line[x] = bg;
    }
    if (!on) return;
    const uint16_t c = FlipRgb565(st.colon[0], st.colon[1], st.colon[2]);
    for (int y = 72; y < 80; y++) {
        uint16_t* line = buf + y * kFcW + 155;
        for (int x = 0; x < 9; x++) line[x] = c;
    }
    for (int y = 92; y < 100; y++) {
        uint16_t* line = buf + y * kFcW + 155;
        for (int x = 0; x < 9; x++) line[x] = c;
    }
}

void DafeiyuDisplay::ShowClockMode() {
    DisplayLockGuard lock(this);
    /* 样式只在首次进时钟时从 NVS 载入一次；之后由 CycleClockStyle 管理 */
    if (fclock_style_ < 0) {
        Settings s(kThemeNs, false);
        int st = s.GetInt("clk_style", 0);
        if (st < 0 || st >= kFcStyleN) {
            st = 0;
        }
        fclock_style_ = (int8_t)st;
        s_fcStyleIdx = st;
    }
    const FcStyle& st = kFcStyles[s_fcStyleIdx];
    if (fclock_root_ == nullptr) {
        fclock_root_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(fclock_root_);
        lv_obj_set_size(fclock_root_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(fclock_root_, 0, 0);
        lv_obj_set_style_bg_color(fclock_root_,
            lv_color_hex((st.bg[0] << 16) | (st.bg[1] << 8) | st.bg[2]), 0);
        lv_obj_set_style_bg_opa(fclock_root_, LV_OPA_100, 0);   /* 全遮蔽主题 */
        lv_obj_remove_flag(fclock_root_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(fclock_root_, LV_OBJ_FLAG_SCROLLABLE);

        fclock_buf_ = (uint16_t*)heap_caps_calloc(
            kFcW * kFcH, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (fclock_buf_ != nullptr) {
            fclock_canvas_ = lv_canvas_create(fclock_root_);
            lv_canvas_set_buffer(fclock_canvas_, fclock_buf_, kFcW, kFcH,
                                 LV_COLOR_FORMAT_RGB565);
        }
        fclock_date_ = lv_label_create(fclock_root_);
        lv_obj_set_style_text_color(fclock_date_, lv_color_hex(0xC8D2E0), 0);
        lv_obj_set_style_text_align(fclock_date_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(fclock_date_, LV_ALIGN_TOP_MID, 0, 142);
        /* 日期用 assets 全量字库（6845 字），中文星期不再方块 */
        auto theme = static_cast<LvglTheme*>(GetTheme());
        if (theme != nullptr) {
            auto tf = theme->text_font();
            if (tf != nullptr && tf->font() != nullptr) {
                lv_obj_set_style_text_font(fclock_date_, tf->font(), 0);
            }
        }
    }
    /* 日期颜色/显隐随样式（极简样式整个藏掉日期行）。
       放在创建判断之外：CycleClockStyle 切换后走不到创建分支，也要生效。 */
    if (fclock_date_ != nullptr) {
        lv_obj_set_style_text_color(fclock_date_, lv_color_hex(st.dateCol), 0);
        if (st.showDate) {
            lv_obj_remove_flag(fclock_date_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(fclock_date_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    fclock_last_min_ = 0xFFFF;     /* 强制下次 tick 全量重绘 */
    fclock_colon_ = true;
    for (int i = 0; i < 4; i++) {
        fclock_anim_start_[i] = 0;
    }
    lv_obj_remove_flag(fclock_root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(fclock_root_);
    clock_mode_visible_ = true;
    ClockTick();                   /* 立即出第一帧 */
    ESP_LOGI(TAG, "时钟模式: 打开");
}

void DafeiyuDisplay::CloseClockMode() {
    DisplayLockGuard lock(this);
    clock_mode_visible_ = false;
    for (int i = 0; i < 4; i++) {
        fclock_anim_start_[i] = 0;
    }
    if (fclock_root_ != nullptr) {
        lv_obj_add_flag(fclock_root_, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "时钟模式: 关闭");
}

/* ── 扩展主题「自定义壁纸」（2026-11-06）──
   壁纸对象 wallpaper_image_ 在 screen 层，而角色被 ApplyPetFix 搬到了
   lv_layer_top() —— 角色会盖住壁纸，所以进壁纸主题必须把角色藏起来。
   装饰动画用 pet_theme_pause(true) 停掉（进扩展主题时统一做）。 */
/* 网页上传壁纸后的热刷新（2026-11-06 用户报「上传了没显示」）：
   旧链路只在开机 / config 版本变化时加载，且要求 wallpaper_enabled 开着 ——
   开关一关 wallpaper_buf_ 永远是空的，壁纸主题下放出来的就是个空 image。
   这里无论开关状态，只要文件在就读进 LVGL。 */
void DafeiyuDisplay::RefreshWallpaper() {
    DisplayLockGuard lock(this);
    if (wallpaper_image_ == nullptr || !HasWallpaper()) {
        return;
    }
    if (wallpaper_buf_ == nullptr) {
        wallpaper_buf_ = static_cast<uint8_t*>(
            heap_caps_malloc(kWallpaperBytes, MALLOC_CAP_SPIRAM));
    }
    if (wallpaper_buf_ == nullptr || !LoadWallpaper(wallpaper_buf_, kWallpaperBytes)) {
        return;
    }
    wallpaper_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    wallpaper_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    wallpaper_dsc_.header.flags = 0;
    wallpaper_dsc_.header.w = kWallpaperW;
    wallpaper_dsc_.header.h = kWallpaperH;
    wallpaper_dsc_.header.stride = kWallpaperW * 2;
    wallpaper_dsc_.data = wallpaper_buf_;
    wallpaper_dsc_.data_size = kWallpaperBytes;
    lv_image_set_src(wallpaper_image_, &wallpaper_dsc_);
    /* ⚠️ 2026-11-06 用户定稿：自定义壁纸只在「壁纸主题」(101) 下显示 ——
       大肥鱼主题用自己的背景/壁纸，角色绝不能叠在用户的壁纸上面。
       肥鱼下的显隐由 ApplyThemeTo / CloseWallpaperTheme 收拾。 */
    if (ext_theme_ == kThemeWallpaperExt) {
        lv_obj_remove_flag(wallpaper_image_, LV_OBJ_FLAG_HIDDEN);
        if (theme_pet_ != nullptr) {
            lv_obj_add_flag(theme_pet_, LV_OBJ_FLAG_HIDDEN);
        }
        if (clock_panel_ != nullptr) {
            lv_obj_move_foreground(clock_panel_);
        }
        if (status_bar_ != nullptr) {
            lv_obj_move_foreground(status_bar_);
        }
    } else {
        lv_obj_add_flag(wallpaper_image_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_invalidate(wallpaper_image_);
    ESP_LOGI(TAG, "壁纸热刷新完成");
}

void DafeiyuDisplay::ShowWallpaperTheme() {
    DisplayLockGuard lock(this);
    if (theme_pet_ != nullptr) {
        lv_obj_add_flag(theme_pet_, LV_OBJ_FLAG_HIDDEN);
    }
    if (wallpaper_image_ != nullptr && HasWallpaper()) {
        RefreshWallpaper();   /* 进主题前确保拿到的是最新的图（递归锁，安全） */
        lv_obj_remove_flag(wallpaper_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_bar_ != nullptr) {
        lv_obj_move_foreground(status_bar_);
    }
    /* 左上大时钟：显示 + 提层（首次显示强制刷一次分钟） */
    if (wp_clock_ != nullptr) {
        lv_obj_move_foreground(wp_clock_);
        lv_obj_remove_flag(wp_clock_, LV_OBJ_FLAG_HIDDEN);
        last_wp_min_ = 0xFFFF;   /* 强制 UpdateStatusBar 立刻重写时间 */
    }
    ESP_LOGI(TAG, "壁纸主题: 打开");
}

void DafeiyuDisplay::CloseWallpaperTheme() {
    DisplayLockGuard lock(this);
    /* ⚠️ 2026-11-06 用户定稿：退出壁纸主题一律把自定义壁纸藏掉 ——
       肥鱼主题用自己的背景，用户的壁纸只属于壁纸主题（101）。
       之前的「wallpaper_enabled 开着就在肥鱼下也显示」口径作废。 */
    if (wallpaper_image_ != nullptr) {
        lv_obj_add_flag(wallpaper_image_, LV_OBJ_FLAG_HIDDEN);
    }
    /* 角色显隐交给引擎/tick 的既有逻辑恢复；时钟卡片由 want_clock 那段接管 */
    if (wp_clock_ != nullptr) {
        lv_obj_add_flag(wp_clock_, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "壁纸主题: 关闭");
}

void DafeiyuDisplay::ClockTick() {
    if (!clock_mode_visible_ || fclock_root_ == nullptr) {
        return;
    }
    /* 唤醒对话中时钟视图被临时藏掉（SetWakeIndicator），跳过重绘省 CPU */
    if (wake_state_ != 0) {
        return;
    }
    const uint32_t now = lv_tick_get();
    bool need_inv = false;

    if (fclock_buf_ != nullptr) {
        time_t nowt = time(NULL);
        struct tm* ti = localtime(&nowt);
        const bool synced = (ti != nullptr && ti->tm_year >= 2025 - 1900);

        if (synced && fclock_last_min_ != (uint16_t)ti->tm_min) {
            const bool first = (fclock_last_min_ == 0xFFFF);
            const int nd[4] = {ti->tm_hour / 10, ti->tm_hour % 10,
                               ti->tm_min / 10, ti->tm_min % 10};
            for (int i = 0; i < 4; i++) {
                if (first) {
                    fclock_digit_[i] = (int8_t)nd[i];
                    fclock_prev_[i] = (int8_t)nd[i];
                    fclock_anim_start_[i] = 0;
                    FlipPaintCard(fclock_buf_, i, nd[i], nd[i], -1);
                } else if (nd[i] != fclock_digit_[i]) {
                    /* 个位（第 4 张）先动，向左级联 110ms/卡 */
                    fclock_prev_[i] = fclock_digit_[i];
                    fclock_digit_[i] = (int8_t)nd[i];
                    fclock_anim_start_[i] = now + (uint32_t)(3 - i) * 110;
                } else {
                    fclock_anim_start_[i] = 0;
                }
            }
            if (first) {
                /* 首帧：整屏铺底（样式底色）+ 冒号 */
                const FcStyle& st = kFcStyles[s_fcStyleIdx];
                const uint16_t bg = FlipRgb565(st.bg[0], st.bg[1], st.bg[2]);
                for (int y = 0; y < kFcH; y++) {
                    uint16_t* line = fclock_buf_ + y * kFcW;
                    for (int x = 0; x < kFcW; x++) line[x] = bg;
                }
                for (int i = 0; i < 4; i++) {
                    FlipPaintCard(fclock_buf_, i, fclock_digit_[i],
                                  fclock_digit_[i], -1);
                }
                FlipPaintColon(fclock_buf_, true);
            }
            fclock_last_min_ = (uint16_t)ti->tm_min;

            /* 日期行 */
            static const char* kWk[7] = {"周日", "周一", "周二", "周三",
                                         "周四", "周五", "周六"};
            char dbuf[48];
            snprintf(dbuf, sizeof(dbuf), "%d月%d日 %s", ti->tm_mon + 1,
                     ti->tm_mday, kWk[ti->tm_wday]);
            if (fclock_date_ != nullptr) {
                lv_label_set_text(fclock_date_, dbuf);
            }
            need_inv = true;
        }

        if (synced && !(fclock_last_min_ == 0xFFFF)) {
            /* 冒号 1Hz 闪：只重绘冒号小区域 */
            if (now - fclock_last_blink_ms_ >= 500) {
                fclock_last_blink_ms_ = now;
                fclock_colon_ = !fclock_colon_;
                FlipPaintColon(fclock_buf_, fclock_colon_);
                need_inv = true;
            }
            /* 翻页帧（320ms 内每 tick 重绘对应卡） */
            for (int i = 0; i < 4; i++) {
                if (fclock_anim_start_[i] == 0) {
                    continue;
                }
                const int32_t p = (int32_t)(now - fclock_anim_start_[i]);
                if (p < 0) {
                    /* 还没轮到这张卡：保持旧字 */
                    FlipPaintCard(fclock_buf_, i, fclock_prev_[i],
                                  fclock_prev_[i], -1);
                    need_inv = true;
                } else if (p >= 320) {
                    FlipPaintCard(fclock_buf_, i, fclock_prev_[i],
                                  fclock_digit_[i], -1);
                    fclock_anim_start_[i] = 0;
                    need_inv = true;
                } else {
                    FlipPaintCard(fclock_buf_, i, fclock_prev_[i],
                                  fclock_digit_[i], (int)p);
                    need_inv = true;
                }
            }
        }
    }
    if (need_inv && fclock_canvas_ != nullptr) {
        lv_obj_invalidate(fclock_canvas_);
    }
}

void DafeiyuDisplay::SetSongTitle(const char* title) {
    if (title == nullptr || title[0] == '\0') {
        return;
    }
    DisplayLockGuard lock(this);   /* MCP 工具跑在主循环任务，理由同 SetStatus */
    if (song_label_ == nullptr) {
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", title);
    buf[sizeof(buf) - 1] = '\0';
    /* 2026-11-06 歌名换用 assets 分区的全量字库（font_noto_sans_common_20_4，
       6845 常用字，cbin 格式，Assets::Apply 开机加载进主题）——
       原来的 basic 静态字体只有 890 字，歌名里生僻字全渲染成方块。 */
    auto theme = static_cast<LvglTheme*>(GetTheme());
    if (theme != nullptr) {
        auto tf = theme->text_font();
        if (tf != nullptr && tf->font() != nullptr) {
            lv_obj_set_style_text_font(song_label_, tf->font(), 0);
        }
    }
    lv_label_set_text(song_label_, buf);
    lv_obj_remove_flag(song_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(song_label_);
    song_until_ms_ = lv_tick_get() + 20000;   // 20 秒后由 tick 收走
    music_playing_ = true;   /* 2026-11-06 报歌名即视为开始放音乐（频谱依据） */
    ApplyRefrPeriod();       /* 放歌降帧生效（音乐解码优先） */
    ESP_LOGI(TAG, "歌名上报: %s", buf);
}

/* 2026-11-06 音乐起/停（MCP self.music.playing）。
   频谱只认这个标志：对话 TTS（说话态）永远走唤醒球，不再出频谱。 */
void DafeiyuDisplay::SetMusicPlaying(bool on) {
    DisplayLockGuard lock(this);
    music_playing_ = on;
    if (!on) {
        if (song_label_ != nullptr) {
            song_until_ms_ = 0;
            lv_obj_add_flag(song_label_, LV_OBJ_FLAG_HIDDEN);
        }
        /* 歌词跟着音乐一起收（用户要求：歌词只在放音乐时显示） */
        if (subtitle_label_ != nullptr) {
            subtitle_until_ms_ = 0;
            lv_obj_add_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* 2026-11-06：放歌期间把 LVGL 刷新降到 20fps（见 ApplyRefrPeriod） */
    ApplyRefrPeriod();
    ESP_LOGI(TAG, "音乐播放状态: %s", on ? "开始" : "停止");
}

void DafeiyuDisplay::UpdateIpLabel() {
    if (ip_label_ == nullptr) {
        return;
    }
    if (lv_tick_elaps(last_ip_ms_) < 5000) {
        return;
    }
    last_ip_ms_ = lv_tick_get();
    if (ip_text_[0] != '\0') {
        return;  // 拿到了就不用再查
    }
    std::string ip = WifiManager::GetInstance().GetIpAddress();
    if (ip.empty()) {
        return;
    }
    snprintf(ip_text_, sizeof(ip_text_), "%s", ip.c_str());
    lv_label_set_text(ip_label_, ip_text_);
    /* 2026-11-06 用户要求：屏幕上全局不显示 IP（之前塞在主题文案行）。
       ip_text_ 保留在内存里 —— 二维码、MCP 报网址还靠它。 */
    if (theme_ready_) {
        pet_theme_set_quote("");
    }
    ESP_LOGI(TAG, "ip ready (不再上屏): %s", ip_text_);
}



void DafeiyuDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);   /* 应用任务调用；理由同 SetEmojiCollection */
    const bool show = (image != nullptr);
    // 基类负责缓存 LvglImage 和 5 秒后自动清掉的定时器
    SpiLcdDisplay::SetPreviewImage(std::move(image));

    previewing_ = show;
    if (show) {
        // 拍了张照：接下来服务端的描述里如果有食物，就拿去喂她
        // （养成玩法 PetGame 已删除，拍照喂食逻辑一并去掉）
    }
    if (preview_image_ != nullptr) {
        if (show) {
            lv_obj_set_size(preview_image_, LV_HOR_RES, LV_VER_RES);
            lv_obj_center(preview_image_);
            if (preview_image_cached_ != nullptr) {
                auto* dsc = preview_image_cached_->image_dsc();
                if (dsc != nullptr && dsc->header.w > 0 && dsc->header.h > 0) {
                    /* ⚠️ 2026-11-01 摄像头方向修正：
                       本板用 Esp32Camera（esp32_camera.cc），只支持 hmirror/vflip；
                       Kconfig 的 XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE 只有
                       esp_video.cc（ESP32-P4）实现，开了对本板无效 —— 实测画面仍左转 90°。
                       所以改在显示层用 LVGL 旋转：900 = 顺时针 90°（向右）。
                       旋转后视觉宽高互换，缩放必须按互换后的尺寸算，否则铺不满屏幕。 */
                    lv_image_set_rotation(preview_image_, 900);
                    const int sx = LV_HOR_RES * 256 / (int)dsc->header.h;
                    const int sy = LV_VER_RES * 256 / (int)dsc->header.w;
                    lv_image_set_scale(preview_image_, (sx > sy) ? sx : sy);
                }
            }
            lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(preview_image_);
            /* ⚠️ 2026-11-01：拍照预览时把挡画面的东西藏起来。
               角色 theme_pet_ 被 ApplyPetFix() 放到了 lv_layer_top()，
               而预览图在 screen 里 —— top layer 永远盖在 screen 之上，
               光靠 move_foreground 救不回来，这就是「有东西挡住画面」。
               顶部状态栏（电量/WiFi）同理，一并隐藏。 */
            if (theme_pet_ != nullptr) {
                lv_obj_add_flag(theme_pet_, LV_OBJ_FLAG_HIDDEN);
            }
            if (sys_bar_ != nullptr) {
                lv_obj_add_flag(sys_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
            /* 恢复：角色只在开机动画结束后才该露面（见 FinishBootScreen）。
               ⚠️ 2026-11-02 修复「调用摄像头后人物在唤醒界面还能看到」：
               预览结束的恢复是无条件的，而角色在 top layer、唤醒黑屏在
               screen 里 —— top layer 永远盖在上面，于是人物漏了出来。
               正处于唤醒界面（wake_state_ != 0）时必须继续隐藏，
               等待命时 SetWakeIndicator(0) 统一恢复。 */
            if (theme_pet_ != nullptr && boot_finished_ && wake_state_ == 0 &&
                ext_theme_ < 0) {   /* 壁纸/时钟主题下恢复预览也不放角色 */
                lv_obj_remove_flag(theme_pet_, LV_OBJ_FLAG_HIDDEN);
            }
            if (sys_bar_ != nullptr && wake_state_ == 0) {
                lv_obj_remove_flag(sys_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    // 拍照时角色和时钟让位，拍完恢复
    if (pet_image_ != nullptr) {
        /* ⚠️ 2026-11-01 关键修复：这里原本在 show=false 时会把 pet_image_ 重新显示出来，
           把 SetupUI 里那次隐藏悄悄推翻。
           pet_image_ 是「全屏 320x240 的旧立绘」，而主题引擎会把自己背景容器
           lv_obj_move_background() 推到最底层 —— 于是旧立绘一旦显示就直接盖在
           主题角色之上，表现为用户实测的「只到裙子没漏出脚」（怎么调上下位置都没用，
           因为根本不是位置问题，是被整块盖住了）。
           主题接管后必须让它永久隐藏。 */
        if (show || theme_ready_) {
            lv_obj_add_flag(pet_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(pet_image_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    SetClockVisible(!show && base_ == Clip::kIdle);
}

void DafeiyuDisplay::ScheduleNextMicro() {
    uint32_t span = kMicroMaxMs - kMicroMinMs;
    next_micro_ms_ = lv_tick_get() + kMicroMinMs + (span == 0 ? 0 : esp_random() % span);
}

DafeiyuDisplay::Clip DafeiyuDisplay::PickIdleActionByStats() {
    /* 养成玩法（PetGame / PetState）已删除，这里已没有真实数据可依据。
       旧引擎的待机小片段也不再使用，直接返回 kNone，不再播旧立绘待机动作。 */
    return Clip::kNone;
}

// 实时动作参数（一套 intestinal parametric animation）。
//
// 设计来自 `_anim_transitions.html`（另一位同学做的大肥鱼待机界面动画），这里把它
// 移植到 LVGL 上。思路跟「播 GIF」完全不同：素材是单张静态图，动起来靠**实时算**
// 一组正弦变换叠加，而不是一帧帧图片。
//
//   zoom/bob/sway/tilt/jump = [振幅, 周期秒]
//   squash                  = 落地瞬间的挤压量
//   ant                     = 前摇 [时长ms, zoom偏移, y偏移px, tilt偏移deg]，
//                             从切换那一刻起按 easeOutCubic 衰减到 0
//                             （生气前先吸气膨胀、高兴前先下蹲蓄力、哭之前先低头）
//
// 【为什么这样就不会「看起来在循环」】各参数周期取的是互质比例，几个正弦的合成
// 周期长达数十秒，肉眼根本找不到重复点。之前用「整段 GIF 循环播」是做不到这点的。
struct MotionParams {
    float zoom_amp, zoom_period;
    float bob_amp, bob_period;
    float sway_amp, sway_period;
    float tilt_amp, tilt_period;
    float jump_amp, jump_period;
    float squash;
    int ant_ms;
    float ant_zoom, ant_y, ant_tilt;
};

//                        zoom           bob           sway           tilt           jump           squash  ── ant ──────────────────
constexpr MotionParams kMotionIdle  = {0.015f, 3.6f, 3.0f, 2.4f,  1.0f, 5.7f,  0.0f, 1.00f, 0.0f, 1.00f, 0.00f, 260, -0.05f, 0.0f,  0.0f};
constexpr MotionParams kMotionWalk  = {0.000f, 1.0f, 2.0f, 0.76f, 6.0f, 5.9f,  2.0f, 1.52f, 2.0f, 0.76f, 0.03f, 200, -0.04f, 3.0f,  0.0f};
constexpr MotionParams kMotionCry   = {0.000f, 1.0f, 2.0f, 1.5f,  5.0f, 0.9f,  0.0f, 1.00f, 0.0f, 1.00f, 0.00f, 350, -0.03f, 5.0f, -2.0f};
constexpr MotionParams kMotionFear  = {0.000f, 1.0f, 0.0f, 1.0f,  2.0f, 0.14f, 0.0f, 1.00f, 0.0f, 1.00f, 0.00f, 150,  0.06f, 2.0f,  0.0f};
constexpr MotionParams kMotionHappy = {0.000f, 1.0f, 0.0f, 1.0f,  0.0f, 1.0f,  3.0f, 1.24f, 7.0f, 0.62f, 0.05f, 200, -0.05f, 6.0f,  0.0f};
constexpr MotionParams kMotionThink = {0.000f, 1.0f, 1.5f, 3.2f,  0.0f, 1.0f,  3.5f, 4.40f, 0.0f, 1.00f, 0.00f, 400,  0.00f, 0.0f, -3.0f};
constexpr MotionParams kMotionAngry = {0.000f, 1.0f, 2.0f, 0.8f,  3.0f, 0.16f, 0.0f, 1.00f, 0.0f, 1.00f, 0.00f, 180,  0.07f, 0.0f,  0.0f};
constexpr MotionParams kMotionEat   = {0.010f, 2.8f, 2.5f, 0.7f,  0.0f, 1.0f,  0.0f, 1.00f, 0.0f, 1.00f, 0.00f, 250, -0.03f, 3.0f,  0.0f};
constexpr MotionParams kMotionYawn  = {0.025f, 3.4f, 2.0f, 2.7f,  0.0f, 1.0f,  0.0f, 1.00f, 0.0f, 1.00f, 0.00f, 400, -0.04f, 0.0f,  0.0f};
constexpr MotionParams kMotionDance = {0.000f, 1.0f, 2.0f, 1.7f,  0.0f, 1.0f,  9.0f, 0.86f, 5.0f, 0.86f, 0.04f, 220,  0.00f, 0.0f, -8.0f};

// 统一的入场弹性：从 0.90 弹到 1.0，220ms
constexpr uint32_t kPopMs = 220;
constexpr float kPopFrom = 0.90f;
constexpr float kTau = 6.28318530718f;

float EaseOutBack(float k) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    const float p = k - 1.0f;
    return 1.0f + c3 * p * p * p + c1 * p * p;
}

const char* DafeiyuDisplay::ClipListForDebug() {
    return "idle,wait,speak,think,wake,boot,hum,eat,yawn,dance,walk,"
           "error,fear,sad,cry,angry,happy,success,pet";
}

bool DafeiyuDisplay::PlayClipByName(const char* name) {
    DisplayLockGuard lock(this);   /* 网页 /api/act 在 HTTP 任务里调，必须加锁 */
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    // Clip 枚举不在 public 里，这张表只能写在成员函数内部
    static const struct { const char* name; Clip clip; } kMap[] = {
        {"idle", Clip::kIdle},       {"wait", Clip::kWait},   {"speak", Clip::kSpeak},
        {"think", Clip::kThink},     {"wake", Clip::kWake},   {"boot", Clip::kBoot},
        {"hum", Clip::kHum},         {"eat", Clip::kEat},     {"yawn", Clip::kYawn},
        {"dance", Clip::kDance},     {"walk", Clip::kWalk},   {"error", Clip::kError},
        {"fear", Clip::kError},      {"sad", Clip::kSad},     {"cry", Clip::kSad},
        {"angry", Clip::kAngry},     {"happy", Clip::kHappy}, {"success", Clip::kSuccess},
        {"pet", Clip::kPet},
    };
    for (const auto& kv : kMap) {
        if (strcasecmp(name, kv.name) == 0) {
            debug_clip_ = kv.clip;   // 只置标记，真正的播放留给 LVGL tick
            return true;
        }
    }
    return false;
}

void DafeiyuDisplay::ApplyMotionTransform() {
    if (pet_image_ == nullptr || current_ == Clip::kNone) {
        return;
    }
    // Clip 枚举在类里不可外部访问，所以这组映射只能写在成员函数内
    MotionParams m;
    switch (current_) {
        case Clip::kWalk:   m = kMotionWalk;  break;
        case Clip::kThink:  m = kMotionThink; break;
        case Clip::kHappy:
        case Clip::kSuccess:
        case Clip::kPet:
        case Clip::kWake:   m = kMotionHappy; break;
        case Clip::kError:  m = kMotionFear;  break;
        case Clip::kSad:    m = kMotionCry;   break;
        case Clip::kAngry:  m = kMotionAngry; break;
        case Clip::kEat:    m = kMotionEat;   break;
        case Clip::kYawn:   m = kMotionYawn;  break;
        case Clip::kDance:  m = kMotionDance; break;
        default:            m = kMotionIdle;  break;   // idle/wait/speak/boot/hum
    }

    // 时间轴从「片段起播」算起，换片段时相位归零 —— 前摇和入场弹性才有敕地方着手
    const uint32_t el = lv_tick_elaps(clip_start_ms_);
    const float t = el / 1000.0f;

    float zoom = 1.0f, dx = 0.0f, dy = 0.0f, rot = 0.0f;
    if (m.zoom_amp != 0) zoom += m.zoom_amp * sinf(kTau * t / m.zoom_period);
    if (m.sway_amp != 0) dx   += m.sway_amp * sinf(kTau * t / m.sway_period);
    if (m.bob_amp != 0)  dy   += m.bob_amp  * sinf(kTau * t / m.bob_period);
    if (m.tilt_amp != 0) rot  += m.tilt_amp * sinf(kTau * t / m.tilt_period);

    if (m.jump_amp != 0) {
        const float ph = kTau * t / m.jump_period;
        dy -= m.jump_amp * fabsf(sinf(ph));
        if (m.squash > 0) {
            // 落地那一下压扁一点，弹起来才有「重量」
            float c = fabsf(cosf(ph));
            c *= c; c *= c; c *= c;           // 8 次方，只在触地瞬间生效
            zoom *= 1.0f - m.squash * c;
        }
    }

    // 入场弹性
    if (el < kPopMs) {
        zoom *= kPopFrom + (1.0f - kPopFrom) * EaseOutBack(el / static_cast<float>(kPopMs));
    }
    // 前摇（ anticipate ），随 easeOutCubic 衰减到 0
    if (el < static_cast<uint32_t>(m.ant_ms) && m.ant_ms > 0) {
        const float k = el / static_cast<float>(m.ant_ms);
        const float e = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
        zoom *= 1.0f + m.ant_zoom * (1.0f - e);
        dy   += m.ant_y * (1.0f - e);
        rot  += m.ant_tilt * (1.0f - e);
    }

    // 缩放中心放在「脚底」：这样 jump / squash 是脚不离地的弹跳，
    // 用默认的画布中心的话角色会穿地（负责RequestParam 的感觉）
    const lv_coord_t w = lv_obj_get_width(pet_image_);
    const lv_coord_t h = lv_obj_get_height(pet_image_);
    lv_obj_set_style_transform_pivot_x(pet_image_, w / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(pet_image_, h, LV_PART_MAIN);

    lv_obj_set_style_transform_scale(pet_image_, static_cast<int32_t>(zoom * 256.0f), LV_PART_MAIN);
    lv_obj_set_style_translate_x(pet_image_, static_cast<lv_coord_t>(dx), LV_PART_MAIN);
    lv_obj_set_style_translate_y(pet_image_, static_cast<lv_coord_t>(dy), LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(pet_image_, static_cast<int32_t>(rot * 10.0f), LV_PART_MAIN);
}

const char* DafeiyuDisplay::AssetName(Clip clip) {
    switch (clip) {
        case Clip::kIdle:
            return "dafeiyu_idle";
        case Clip::kWait:
            return "dafeiyu_wait";
        case Clip::kThink:
            return "dafeiyu_think";
        case Clip::kSpeak:
            return "dafeiyu_speak";
        case Clip::kWake:
            return "dafeiyu_wake";
        case Clip::kSuccess:
            return "dafeiyu_success";
        case Clip::kError:
            return "dafeiyu_error";
        case Clip::kHappy:
            return "dafeiyu_happy";
        case Clip::kYawn:
            return "dafeiyu_yawn";
        case Clip::kEat:
            return "dafeiyu_eat";
        case Clip::kDance:
            return "dafeiyu_dance";
        case Clip::kHum:
            return "dafeiyu_hum";
        case Clip::kPet:
            return "dafeiyu_pet";
        case Clip::kBoot:
            return "dafeiyu_boot";
        case Clip::kSad:
            return "dafeiyu_sad";
        case Clip::kAngry:
            return "dafeiyu_angry";
        case Clip::kWalk:
            return "dafeiyu_walk";
        default:
            return nullptr;
    }
}

bool DafeiyuDisplay::IsLoopClip(Clip clip) {
    // kWalk 故意不在里面：它是一次性的，演完这一小段就回正面站姿。
    // 让侧身图一直循环的话，角色会一直侧着，跟「偶尔溜达两步」不是一回事。
    return clip == Clip::kIdle || clip == Clip::kWait || clip == Clip::kThink ||
           clip == Clip::kSpeak;
}

bool DafeiyuDisplay::StatusIs(const char* status, const char* ref) {
    return status != nullptr && ref != nullptr && std::strcmp(status, ref) == 0;
}

DafeiyuDisplay::Clip DafeiyuDisplay::EmotionToClip(const char* emotion) {
    if (emotion == nullptr) {
        return Clip::kNone;
    }
    for (const char* name : kPositiveEmotions) {
        if (strcasecmp(emotion, name) == 0) {
            return Clip::kHappy;
        }
    }
    // 素材里有独立的「哭」和「生气」两张脸，先按精确名字接走，
    // 不然它们会跟着 embarrassed 一起落到「惊恐」那张，表情区分度就没了
    if (strcasecmp(emotion, "sad") == 0 || strcasecmp(emotion, "crying") == 0) {
        return Clip::kSad;
    }
    if (strcasecmp(emotion, "angry") == 0) {
        return Clip::kAngry;
    }
    for (const char* name : kNegativeEmotions) {
        if (strcasecmp(emotion, name) == 0) {
            return Clip::kError;
        }
    }
    if (strcasecmp(emotion, "sleepy") == 0) {
        return Clip::kYawn;
    }
    return Clip::kNone;
}

void DafeiyuDisplay::FitImageObject(lv_coord_t w, lv_coord_t h) {
    if (pet_image_ == nullptr || w <= 0 || h <= 0) {
        return;
    }
    // ⚠️ 永远别用 set_size 把对象改小指望它「缩略」：LVGL 对 lv_image 是裁剪
    // 不是缩放 —— 画布仍是素材原始尺寸，改小对象只会显示左上角一小块（用户原话
    // 「小窗口里看局部」）。要变小就在电脑上先把 GIF 缩好（见 _resize_assets.py），
    // 运行时永远按素材原始尺寸画。
    if (lv_obj_get_width(pet_image_) != w || lv_obj_get_height(pet_image_) != h) {
        lv_obj_set_size(pet_image_, w, h);
    }
    // 贴右边站：左边那一条是信息层的地盘（见 kInfoPanelW）
    lv_obj_align(pet_image_, LV_ALIGN_RIGHT_MID, -4, 0);
}

void DafeiyuDisplay::ApplyLayout(bool /*info_mode*/) {
    // 之前有「桌面模式 / 信息模式」两套摆位，实测那是在跟屏幕不够宽较劲：
    // 素材 210x240 占满屏，两套模式都挤。现在统一成一刀切的分区布局
    //（左 158px 信息层 + 右 150px 角色），所以这里不再按模式改 Klein Arrange。
    // 参数留着是为了不动调用方和存档结构。
}

void DafeiyuDisplay::StopGifLocked() {
    if (gif_) {
        gif_->Stop();
        gif_.reset();
    }
}

bool DafeiyuDisplay::PlayLocked(Clip clip) {
    const char* name = AssetName(clip);
    if (name == nullptr) {
        return true;
    }
    // 同一个循环片段正在播就不重开，避免状态刷新把动画打断
    if (clip == current_ && IsLoopClip(clip) && gif_ && gif_->IsPlaying()) {
        return true;
    }

    auto* theme = static_cast<LvglTheme*>(current_theme_);
    if (theme == nullptr) {
        return false;
    }
    auto collection = theme->emoji_collection();
    if (collection == nullptr) {
        return false;
    }
    const LvglImage* image = collection->GetEmojiImage(name);
    if (image == nullptr) {
        return false;  // assets 尚未就绪，等 tick 重试
    }

    StopGifLocked();

    if (!image->IsGif()) {
        lv_image_set_src(pet_image_, image->image_dsc());
        FitImageObject(image->image_dsc()->header.w, image->image_dsc()->header.h);
        current_ = clip;
        clip_start_ms_ = lv_tick_get();
        ESP_LOGI(TAG, "clip -> %s (static)", name);
        return true;
    }

    const int64_t t0 = esp_timer_get_time();
    auto controller = std::make_unique<LvglGif>(image->image_dsc());
    const uint32_t load_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (!controller->IsLoaded()) {
        ESP_LOGE(TAG, "failed to load gif: %s", name);
        return false;
    }
    const bool loop = IsLoopClip(clip);
    controller->SetLoopCount(loop ? 0 : 1);
    controller->SetFrameCallback([this]() {
        if (pet_image_ != nullptr && gif_) {
            lv_image_set_src(pet_image_, gif_->image_dsc());
        }
    });

    gif_ = std::move(controller);
    FitImageObject(gif_->width(), gif_->height());
    lv_image_set_src(pet_image_, gif_->image_dsc());
    // 说话（TTS 播放）时把动画降到一半帧率：解码 + 渲染的 CPU 让给音频，
    // 播放卡顿主要就是这段抢 CPU 抢出来的
    gif_->SetFrameSkip(clip == Clip::kSpeak ? 1 : 0);
    gif_->Start();
    current_ = clip;
    clip_start_ms_ = lv_tick_get();
    ESP_LOGI(TAG, "clip -> %s (%s) load=%ums", name, loop ? "loop" : "once", load_ms);
    return true;
}

void DafeiyuDisplay::Play(Clip clip) {
    if (clip == Clip::kNone) {
        return;
    }
    if (pet_image_ == nullptr) {
        pending_ = clip;
        return;
    }
    // 只登记请求，等 tick 里真正装载；这样调用方（协议任务）立刻返回
    requested_ = clip;
}

void DafeiyuDisplay::PlayOnce(Clip clip) {
    return_clip_ = (base_ != Clip::kNone) ? base_ : Clip::kIdle;
    Play(clip);
}

/* 唤醒状态光环：0=隐藏 1=聆听(青色慢呼吸) 2=说话(橙色快脉动)。
   用户要求「跟主题区分开，知道是在唤醒状态」，且「文字显示无所谓」——
   所以用纯视觉（边框颜色 + 呼吸节奏），完全不依赖中文字体。 */
#ifndef M_PI
#define M_PI 3.14159265f
#endif

/* 呼吸球画布尺寸（必须与 SetupUI 里申请缓冲用的尺寸一致） */
#define ORB_W 180
#define ORB_H 180

/* 大眼睛唤醒动画的区域尺寸（挂在 240x320 竖屏的唤醒层上，故按横屏 320x240） */
#define EYE_AREA_W 300
#define EYE_AREA_H 180
#define EYE_GAP    14

static inline uint16_t OrbRGB565(int r, int g, int b) {
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* 逐像素渲染唤醒球，参考 breathing-orb.html 的 GPT 语音球：
   径向渐变核心 + 表面流动形变(3/5/2 阶谐波) + 内阴影 + 高光 + 外发光
   + 频谱环 + 说话扩散圈。st: 1=聆听 2=说话。
   背景是纯黑，所以「颜色乘以 alpha」就直接等于混合结果，不用做真混合。 */
void DafeiyuDisplay::RenderWakeOrb(uint32_t ms, int st) {
    if (wake_canvas_buf_ == nullptr) return;
    /* 性能测量：定位「说话卡」的瓶颈到底在渲染还是 LVGL 刷新 */
    const int64_t t_render0 = esp_timer_get_time();
    const float t = ms / 1000.0f;
    const bool speaking = (st == 2);

    /* 配色照抄参考图：聆听 = 青/蓝/靛，说话 = 紫/粉/橙 */
    int pal[3][3];
    if (speaking) {
        pal[0][0]=168; pal[0][1]=85;  pal[0][2]=247;
        pal[1][0]=236; pal[1][1]=72;  pal[1][2]=153;
        pal[2][0]=251; pal[2][1]=146; pal[2][2]=60;
    } else {
        pal[0][0]=34;  pal[0][1]=211; pal[0][2]=238;
        pal[1][0]=59;  pal[1][1]=130; pal[1][2]=246;
        pal[2][0]=129; pal[2][1]=140; pal[2][2]=248;
    }

    /* 调色板构建移到 level 计算之后（见下方） */

    /* 合成能量包络（参考图 synthLevel） */
    float level;
    if (speaking) {
        const float a = sinf(t * 6.7f) * 0.5f + 0.5f;
        const float b = sinf(t * 2.9f + 1.7f) * 0.5f + 0.5f;
        const float burst = powf(fmaxf(0.0f, sinf(t * 2.15f + 0.4f)), 2.2f);
        const float jit = sinf(t * 23.3f) * 0.06f + sinf(t * 11.1f) * 0.05f;
        level = fminf(1.0f, 0.22f + 0.40f * a + 0.22f * b + 0.42f * burst + jit);
    } else {
        const float a = sinf(t * 2.3f) * 0.5f + 0.5f;
        const float b = sinf(t * 5.1f + 2.1f) * 0.5f + 0.5f;
        level = fminf(1.0f, 0.10f + 0.16f * a + 0.10f * b);
    }

    /* 建立 256 色调色板。2026-11-02 性能优化：不再每帧重建（256 次设置纯属
       浪费，level 变化很慢，肉眼追不上）—— 每 8 帧或聆听/说话切换时才重建。 */
    static uint8_t pal_frame = 0;
    static int8_t pal_speaking = -1;
    const bool pal_dirty = (pal_speaking != (int8_t)speaking) || ((pal_frame++ & 7) == 0);
    pal_speaking = (int8_t)speaking;
    if (pal_dirty)
    for (int i = 0; i < 256; i++) {
        int r = 0, g = 0, b = 0;
        if (i == 0) {
            r = g = b = 0;
        } else if (i < 80) {                       /* 外发光：1 最亮 → 79 最暗 */
            const float q = 1.0f - (float)(i - 1) / 78.0f;
            const float a = q * q * 0.22f * (speaking ? 1.15f : 0.85f)
                          * (0.6f + 0.4f * level);
            r = (int)(pal[0][0] * a); g = (int)(pal[0][1] * a); b = (int)(pal[0][2] * a);
        } else if (i < 230) {                      /* 核心径向渐变 + 内阴影 */
            const float u = (float)(i - 80) / 149.0f;
            if (u < 0.45f) {
                const float k = u / 0.45f;
                r = pal[0][0] + (int)((pal[1][0] - pal[0][0]) * k);
                g = pal[0][1] + (int)((pal[1][1] - pal[0][1]) * k);
                b = pal[0][2] + (int)((pal[1][2] - pal[0][2]) * k);
            } else {
                const float k = (u - 0.45f) / 0.55f;
                r = pal[1][0] + (int)((pal[2][0] - pal[1][0]) * k);
                g = pal[1][1] + (int)((pal[2][1] - pal[1][1]) * k);
                b = pal[1][2] + (int)((pal[2][2] - pal[1][2]) * k);
            }
            const float shade = 1.0f - 0.45f * u * u;
            r = (int)(r * shade); g = (int)(g * shade); b = (int)(b * shade);
        } else if (i < 245) {                      /* 频谱环 */
            const float a = (float)(i - 230) / 14.0f;
            r = (int)(pal[2][0] * a); g = (int)(pal[2][1] * a); b = (int)(pal[2][2] * a);
        } else if (i < 250) {                      /* 说话扩散圈 */
            const float a = (float)(i - 245) / 4.0f * 0.30f;
            r = (int)(pal[0][0] * a); g = (int)(pal[0][1] * a); b = (int)(pal[0][2] * a);
        } else {                                   /* 高光：核心色向白混合 */
            const float k = 0.15f + 0.45f * (float)(i - 250) / 5.0f;
            r = pal[0][0] + (int)((255 - pal[0][0]) * k);
            g = pal[0][1] + (int)((255 - pal[0][1]) * k);
            b = pal[0][2] + (int)((255 - pal[0][2]) * k);
        }
        lv_canvas_set_palette(wake_canvas_, (uint8_t)i,
                              lv_color32_make((uint8_t)r, (uint8_t)g, (uint8_t)b, 255));
    }

    /* 呼吸：说话更快、幅度更大 */
    const float spd = speaking ? 2.10f : 1.15f;
    const float breath = sinf(t * spd * 1.35f) * 0.6f
                       + sinf(t * spd * 0.53f + 1.2f) * 0.4f;
    const float baseR = 36.0f;
    const float R = baseR * (1.0f + (speaking ? 0.09f : 0.055f) * breath
                                  + level * 0.16f * (speaking ? 1.25f : 1.0f));

    /* 表面流动：相位常数每帧只算一次，像素循环里只剩乘加 */
    const float wobAmp = speaking ? 0.130f : 0.085f;
    const float ws = speaking ? 2.40f : 1.70f;
    const float c1  = cosf(t * ws),          s1  = sinf(t * ws);
    const float c2p = cosf(t * ws * 1.27f),  s2p = sinf(t * ws * 1.27f);
    const float c3p = cosf(t * ws * 0.63f),  s3p = sinf(t * ws * 0.63f);

    const float cx = ORB_W / 2.0f, cy = ORB_H / 2.0f;
    const float glowMax = R * 2.6f;
    const float glowMax2 = glowMax * glowMax;
    const float hx = cx - R * 0.38f, hy = cy - R * 0.46f;   /* 高光中心 */
    const float hlR = R * 0.85f;

    uint8_t *buf = wake_canvas_buf_;
    for (int y = 0; y < ORB_H; y++) {
        const float dy = (float)y - cy;
        const int row = y * ORB_W;
        for (int x = 0; x < ORB_W; x++) {
            const float dx = (float)x - cx;
            const float r2 = dx * dx + dy * dy;
            uint8_t px = 0;
            if (r2 <= glowMax2) {
                const float r = sqrtf(r2);
                float wob = 0.0f;
                if (r > 0.5f) {
                    const float ux = dx / r, uy = dy / r;
                    const float s2 = 2.0f * ux * uy;        /* sin2a */
                    const float c2 = ux * ux - uy * uy;     /* cos2a */
                    const float s3 = s2 * ux + c2 * uy;     /* sin3a */
                    const float c3 = c2 * ux - s2 * uy;     /* cos3a */
                    const float s5 = s2 * c3 + c2 * s3;     /* sin5a */
                    const float c5 = c2 * c3 - s2 * s3;     /* cos5a */
                    const float S3  = s3 * c1  + c3 * s1;   /* sin(3a+p1) */
                    const float S5m = s5 * c2p - c5 * s2p;  /* sin(5a-p2) */
                    const float S2p = s2 * c3p + c2 * s3p;  /* sin(2a+p3) */
                    wob = wobAmp * (0.55f * S3 + 0.30f * S5m + 0.18f * S2p);
                }
                const float Rw = R * (1.0f + wob);
                if (r <= Rw) {
                    /* 核心：pal0 → pal1 → pal2 径向渐变 */
                    const float u = r / Rw;
                    int cr, cg, cb;
                    if (u < 0.45f) {
                        const float k = u / 0.45f;
                        cr = pal[0][0] + (int)((pal[1][0] - pal[0][0]) * k);
                        cg = pal[0][1] + (int)((pal[1][1] - pal[0][1]) * k);
                        cb = pal[0][2] + (int)((pal[1][2] - pal[0][2]) * k);
                    } else {
                        const float k = (u - 0.45f) / 0.55f;
                        cr = pal[1][0] + (int)((pal[2][0] - pal[1][0]) * k);
                        cg = pal[1][1] + (int)((pal[2][1] - pal[1][1]) * k);
                        cb = pal[1][2] + (int)((pal[2][2] - pal[1][2]) * k);
                    }
                    /* 内阴影和高光都已折算进调色板，这里只写索引。
                       2026-11-02 性能：高光改用距离平方判断，省掉每像素
                       第二个 sqrtf —— 这是渲染循环里最贵的一刀。 */
                    px = (uint8_t)(80 + (int)(u * 149.0f));
                    const float dhx = (float)x - hx, dhy = (float)y - hy;
                    const float hl2 = (dhx * dhx + dhy * dhy) / (hlR * hlR);
                    if (hl2 < 0.9f) {
                        px = (uint8_t)(250 + (int)((1.0f - hl2) * 5.0f));
                    }
                } else {
                    /* 外发光 */
                    float g = (r - Rw) / (glowMax - Rw);
                    if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
                    float a = (1.0f - g);
                    a = a * a * 0.20f * (speaking ? 1.15f : 0.85f) * (0.6f + 0.4f * level);
                    px = (uint8_t)(1 + (int)(g * 78.0f));   /* 1..79 外发光 */
                }
            }
            buf[row + x] = px;
        }
    }

    /* 频谱环：48 根径向短线，长度由合成频段驱动（说话时更长更活） */
    {
        const float ringR = R * 1.52f;
        const int N = 48;
        const float alpha = 0.42f * (0.4f + level);
        for (int i = 0; i < N; i++) {
            const float a = (float)i / N * 2.0f * M_PI - M_PI / 2.0f + t * 0.12f;
            const int bi = (i * 6) / N;
            float bv = 0.25f
                     + 0.45f * (sinf(t * (2.1f + bi * 1.37f) + bi * 2.2f) * 0.5f + 0.5f)
                     + 0.30f * (sinf(t * (7.3f + bi * 0.9f) + bi) * 0.5f + 0.5f);
            float v = bv * (0.55f + 0.45f * sinf(i * 0.7f + t * 3.1f)) + 0.04f;
            if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
            const float len = R * 0.30f * v * (speaking ? 1.5f : 1.15f);
            const float ca = cosf(a), sa = sinf(a);
            const float av = alpha * (0.35f + v * 0.65f);
            const uint8_t col = (uint8_t)(230 + (int)(av * 14.0f));
            for (float s = 0.0f; s <= len; s += 1.0f) {
                const float rr = ringR + s;
                const int bx = (int)(cx + ca * rr), by = (int)(cy + sa * rr);
                for (int oy = 0; oy < 2; oy++) {
                    const int yy = by + oy;
                    if (yy < 0 || yy >= ORB_H) continue;
                    for (int ox = 0; ox < 2; ox++) {
                        const int xx = bx + ox;
                        if (xx < 0 || xx >= ORB_W) continue;
                        buf[yy * ORB_W + xx] = col;
                    }
                }
            }
        }
    }

    /* 说话扩散圈：两圈错开相位，由小到大并淡出 */
    if (speaking) {
        for (int k = 0; k < 2; k++) {
            const float ph = fmodf(t * 0.55f + k * 0.5f, 1.0f);
            const float rr = R * (1.02f + ph * 2.4f);
            const float a = 0.30f * (1.0f - ph);
            const uint8_t col = (uint8_t)(245 + (int)((1.0f - ph) * 4.0f));
            for (int i = 0; i < 120; i++) {
                const float ang = (float)i / 120.0f * 2.0f * M_PI;
                const int xx = (int)(cx + cosf(ang) * rr);
                const int yy = (int)(cy + sinf(ang) * rr);
                if (xx >= 0 && xx < ORB_W && yy >= 0 && yy < ORB_H) {
                    buf[yy * ORB_W + xx] = col;
                }
            }
        }
    }

    /* 性能测量：每 150 帧输出一次平均渲染耗时。
       30fps 下每帧预算 33ms：渲染 >15ms 就得降画布尺寸或降帧率，
       <10ms 说明瓶颈在别处（LVGL 刷新/总线），另想办法。 */
    {
        static uint32_t perf_n = 0;
        static int64_t perf_acc = 0;
        perf_acc += esp_timer_get_time() - t_render0;
        if (++perf_n >= 150) {
            ESP_LOGI(TAG, "orb render avg %.1f ms/frame (%u frames)",
                     (float)perf_acc / 1000.0f / perf_n, perf_n);
            perf_n = 0;
            perf_acc = 0;
        }
    }
}

/* 主题容器显隐，实现在 _themes/pet_themes.c（纯 C），所以要用 extern "C"。
   必须在文件作用域声明 —— 写进函数体里会报
   "expected unqualified-id before string constant"。 */
extern "C" void pet_theme_show(int on);

/* ==================== 大眼睛待机形象 ====================
   参考 xiaozhi-esp32 里的 FluxGarage_RoboEyes：不画图、不用资源，
   眼白是圆角矩形，眼睑是同色（黑）矩形盖在上面 —— 因为眼睑是眼白的
   子对象，会被父对象裁剪，天然只作用在眼睛范围内。
   每个情绪只改 4 个数字：宽、高、圆角、颜色（+ 眼睑占高度百分比）。 */
void DafeiyuDisplay::BuildEyesFace() {
    if (face_root_ != nullptr) {
        return;
    }
    lv_obj_t* screen = lv_screen_active();
    if (screen == nullptr) {
        return;
    }
    /* 挂在唤醒层上：天然只在唤醒时可见，也不会盖到待机的大肥鱼角色。 */
    lv_obj_t* parent = (wake_overlay_ != nullptr) ? wake_overlay_ : screen;
    face_root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(face_root_);
    lv_obj_set_size(face_root_, EYE_AREA_W, EYE_AREA_H);
    /* 整体上移：脸在屏幕中偏上更顺眼（原来居中显得"沉"） */
    lv_obj_align(face_root_, LV_ALIGN_CENTER, 0, -14);
    lv_obj_remove_flag(face_root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(face_root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face_root_, LV_OBJ_FLAG_HIDDEN);   // 由 ApplyFaceMode 决定显隐

    for (int i = 0; i < 2; i++) {
        eye_[i] = lv_obj_create(face_root_);
        lv_obj_remove_style_all(eye_[i]);
        lv_obj_set_style_bg_opa(eye_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(eye_[i], 0, 0);
        lv_obj_remove_flag(eye_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(eye_[i], LV_OBJ_FLAG_SCROLLABLE);

        /* 瞳孔先建、眼睑后建：这样眼睑天然压在瞳孔上面（生气时压住上半眼） */
        pupil_[i] = lv_obj_create(eye_[i]);
        lv_obj_remove_style_all(pupil_[i]);
        lv_obj_set_style_bg_color(pupil_[i], lv_color_hex(0x101A2A), 0);
        lv_obj_set_style_bg_opa(pupil_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(pupil_[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_remove_flag(pupil_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(pupil_[i], LV_OBJ_FLAG_SCROLLABLE);

        lid_[i] = lv_obj_create(eye_[i]);
        lv_obj_remove_style_all(lid_[i]);
        lv_obj_set_style_bg_color(lid_[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(lid_[i], LV_OPA_COVER, 0);
        lv_obj_remove_flag(lid_[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(lid_[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(lid_[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* 嘴：细弧线（lv_line），不是填充色块 —— 参考 C3 小车的 RoboEyes，
       真正的表情靠眼睑和眼珠，嘴只是一条淡淡的弧，太粗就成了"香肠嘴"。 */
    mouth_ = lv_line_create(face_root_);
    lv_obj_remove_style_all(mouth_);
    lv_obj_set_style_line_width(mouth_, 6, 0);
    lv_obj_set_style_line_color(mouth_, lv_color_hex(0xD8ECFF), 0);
    lv_obj_set_style_line_opa(mouth_, LV_OPA_80, 0);
    lv_obj_set_style_line_rounded(mouth_, true, 0);
    lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(mouth_, LV_OBJ_FLAG_SCROLLABLE);

    UpdateEyesFace(0);
    ESP_LOGI(TAG, "大眼睛形象已建立(眼+瞳+嘴, 纯图元 0 额外内存)");
}

void DafeiyuDisplay::UpdateEyesFace(int mood) {
    if (face_root_ == nullptr) {
        return;
    }
    face_mood_ = mood;
    /* 表情参数表：宽 / 高 / 圆角 / 眼白颜色 / 眼睑覆盖高度百分比
       参考 RoboEyes 的 8 种 mood，这里合并成 6 种最常见、辨识度最高的。 */
    struct EyePose {
        int16_t  w, h, r;
        uint32_t color;
        int16_t  lid_pct;      // >0 时画眼睑（从上往下盖）
    };
    EyePose p;
    switch (mood) {
        case 1: p = {84,  64, 32, 0xF2FFF8,  0}; break;   // 开心：眯成弯弯的一条
        case 2: p = {84,  98, 22, 0xFFF1E6, 42}; break;   // 生气：上眼睑压下来
        case 3: p = {96, 130, 48, 0xFFFFFF,  0}; break;   // 惊讶：瞪大
        case 4: p = {84,  40, 20, 0xE6EEF8,  0}; break;   // 困倦：半闭
        case 5: p = {88, 100, 44, 0xFF5C8A,  0}; break;   // 爱心：粉红大眼
        default: p = {84, 112, 24, 0xEAF6FF, 0}; break;   // 默认
    }
    for (int i = 0; i < 2; i++) {
        if (eye_[i] == nullptr) {
            continue;
        }
        lv_obj_set_size(eye_[i], p.w, p.h);
        lv_obj_set_style_radius(eye_[i], p.r, 0);
        lv_obj_set_style_bg_color(eye_[i], lv_color_hex(p.color), 0);
        const int16_t x = (i == 0) ? (EYE_AREA_W / 2 - EYE_GAP - p.w) : (EYE_AREA_W / 2 + EYE_GAP);
        /* 眼睛在容器内偏上 16px，给下面的嘴留位置 */
        lv_obj_set_pos(eye_[i], x, (EYE_AREA_H - p.h) / 2 - 16);
        if (lid_[i] != nullptr) {
            if (p.lid_pct > 0) {
                lv_obj_set_size(lid_[i], p.w + 4, p.h * p.lid_pct / 100);
                lv_obj_set_pos(lid_[i], -2, 0);
                /* 生气时给眼睑加角度：内角低、外角高，才像"怒目"；
                   不加角度只是一条水平横杠，看着像犯困。单位 0.1°。 */
                lv_obj_set_style_transform_rotation(
                    lid_[i], (mood == 2) ? (i == 0 ? 220 : -220) : 0, 0);
                lv_obj_remove_flag(lid_[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(lid_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    /* 嘴形交给我 DrawMouth：细弧线，开心往上弯、生气往下撇、惊讶画圆 */
    DrawMouth(mood, 0);

    /* 瞳孔：惊讶放大、生气缩小；眯眼（开心/困倦）时藏起来，否则显得怪 */
    int16_t pw = 30, ph = 34;
    if (mood == 3)      { pw = 34; ph = 42; }
    else if (mood == 2) { pw = 22; ph = 24; }
    else if (mood == 5) { pw = 34; ph = 34; }
    for (int i = 0; i < 2; i++) {
        if (pupil_[i] == nullptr) continue;
        lv_obj_set_size(pupil_[i], pw, ph);
        lv_obj_align(pupil_[i], LV_ALIGN_CENTER, face_look_dx_, face_look_dy_);
        if (mood == 1 || mood == 4) lv_obj_add_flag(pupil_[i], LV_OBJ_FLAG_HIDDEN);
        else                        lv_obj_remove_flag(pupil_[i], LV_OBJ_FLAG_HIDDEN);
    }
    face_talking_ = false;   // 切表情时嘴形已重置，等下一帧说话动画重新接管
}

/* 单位圆 16 等分（×100），用于「惊讶 / 说话」的圆嘴 */
static const int8_t kMouthCircle[16][2] = {
    {100, 0}, {92, 38}, {71, 71}, {38, 92}, {0, 100}, {-38, 92}, {-71, 71}, {-92, 38},
    {-100, 0}, {-92, -38}, {-71, -71}, {-38, -92}, {0, -100}, {38, -92}, {71, -71}, {92, -38}};

/* lv_line 只保存指针，点数组必须活得够久 —— 脸上只有一个嘴，文件级静态即可 */
static lv_point_precise_t s_mouth_pts[16];

void DafeiyuDisplay::DrawMouth(int mood, int open) {
    if (mouth_ == nullptr) {
        return;
    }
    const int cx = EYE_AREA_W / 2;
    const int cy = 150;

    /* 张嘴（说话中 / 惊讶）：画一圈椭圆轮廓，像「O」 */
    if (mood == 3 || open > 45) {
        const int rx = (mood == 3) ? 13 : 15;
        const int ry = (mood == 3) ? 17 : (5 + open / 7);
        for (int i = 0; i < 16; i++) {
            s_mouth_pts[i].x = (lv_value_precise_t)(cx + rx * kMouthCircle[i][0] / 100);
            s_mouth_pts[i].y = (lv_value_precise_t)(cy + ry * kMouthCircle[i][1] / 100);
        }
        lv_line_set_points(mouth_, s_mouth_pts, 16);
        return;
    }

    /* 其余：9 点抛物线。depth > 0 上弯（微笑），< 0 下撇（不高兴），0 = 平嘴。
       抛物线中间低、两端高，看起来就是一条柔和的笑弧，不会像色块那样"香肠嘴"。 */
    int w = 34, depth = 3;
    switch (mood) {
        case 1: w = 64; depth = 13; break;    // 开心：明显上扬
        case 2: w = 30; depth = -7; break;    // 生气：下撇
        case 4: w = 24; depth = 2;  break;    // 困倦：几乎平
        case 5: w = 50; depth = 11; break;    // 爱心：温柔微笑
        default: w = 34; depth = 3; break;    // 默认：淡淡的笑
    }
    for (int i = 0; i < 9; i++) {
        const int u = i * 200 / 8 - 100;                       // -100..100
        s_mouth_pts[i].x = (lv_value_precise_t)(cx + w * u / 200);
        s_mouth_pts[i].y = (lv_value_precise_t)(cy + depth * (10000 - u * u) / 10000);
    }
    lv_line_set_points(mouth_, s_mouth_pts, 9);
}

void DafeiyuDisplay::AnimateEyes(uint32_t now_ms) {
    /* 让脸「活」起来的三件事：眼珠乱看、整体轻微浮动、说话时嘴动。
       全部用整数运算，不建任何动画对象/定时器。 */
    const bool speaking = (wake_state_ == 2);

    /* 呼吸浮动：整数三角波，±3px，周期约 3.2 秒 */
    const uint32_t bp = (now_ms / 40) % 80;
    const int btri = (bp < 40) ? (int)bp : (int)(79 - bp);      // 0..39
    lv_obj_align(face_root_, LV_ALIGN_CENTER, 0, -14 - (btri - 20) / 8);

    /* 眼珠漂移：0.9~2.6 秒随机看一个方向 —— 这一项专治「呆愣愣」 */
    if (now_ms >= face_look_ms_) {
        const uint32_t r = now_ms * 1103515245u + 12345u;
        face_look_dx_ = (int8_t)((int)((r >> 15) % 13) - 6);    // -6..6
        face_look_dy_ = (int8_t)((int)((r >> 7)  % 9)  - 4);    // -4..4
        face_look_ms_ = now_ms + 900 + (r % 1700);
        for (int i = 0; i < 2; i++) {
            if (pupil_[i] != nullptr) {
                lv_obj_align(pupil_[i], LV_ALIGN_CENTER, face_look_dx_, face_look_dy_);
            }
        }
    }

    /* 嘴：说话时开口度 0~88 抖动，停下恢复当前表情的弧线 */
    if (mouth_ != nullptr) {
        if (speaking) {
            const uint32_t p = (now_ms / 90) % 10;
            const int open = ((p < 5) ? (int)p : (int)(10 - p)) * 22;
            DrawMouth(face_mood_, open);
            face_talking_ = true;
        } else if (face_talking_) {
            UpdateEyesFace(face_mood_);   // 恢复嘴形，同时把 talking 复位
        }
    }

    /* 眨眼：2.6~5.8 秒一次，压扁 110ms 再恢复 */
    static uint32_t blink_end_ms = 0;
    if (blink_next_ms_ == 0) {
        blink_next_ms_ = now_ms + 2600;
    } else if (now_ms >= blink_next_ms_) {
        for (int i = 0; i < 2; i++) {
            if (eye_[i] != nullptr) lv_obj_set_height(eye_[i], 12);
        }
        blink_end_ms = now_ms + 110;
        blink_next_ms_ = now_ms + 2600 + (now_ms % 3200);
    } else if (blink_end_ms != 0 && now_ms > blink_end_ms) {
        blink_end_ms = 0;
        UpdateEyesFace(face_mood_);
    }
}

void DafeiyuDisplay::ApplyFaceMode() {
    /* ⚠️ 2026-11-02 教训：这里绝不能 early-return（眼睛不创建了，
       以前那行 `if (face_root_ == nullptr) return;` 会让呼吸球的显隐没人管，
       结果唤醒时球要么不出现、要么一直挂在屏幕上）。 */
    const uint8_t style  = GetConfig().wake_style;
    const bool    waking = (wake_state_ != 0);

    /* 大眼睛对象已不再创建，这段自然空转 */
    if (face_root_ != nullptr) {
        const bool show_face = (style == 1) && waking;
        const bool face_now  = !lv_obj_has_flag(face_root_, LV_OBJ_FLAG_HIDDEN);
        if (show_face != face_now) {
            if (show_face) lv_obj_remove_flag(face_root_, LV_OBJ_FLAG_HIDDEN);
            else           lv_obj_add_flag(face_root_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (wake_orb_lv_ != nullptr && wake_orb_lv_->canvas != nullptr) {
        /* 2026-11-06：频谱接管期间（说话态）球必须让位 —— 本函数每帧都跑，
           不加这个条件的话会把 UpdateSpectrum 里的隐藏每帧覆盖回去，
           表现就是「频谱后面还透着唤醒球」。 */
        const bool show_orb = (style == 0) && waking && !spec_visible_;
        const bool orb_now  = !lv_obj_has_flag(wake_orb_lv_->canvas, LV_OBJ_FLAG_HIDDEN);
        if (show_orb != orb_now) {
            if (show_orb) lv_obj_remove_flag(wake_orb_lv_->canvas, LV_OBJ_FLAG_HIDDEN);
            else          lv_obj_add_flag(wake_orb_lv_->canvas, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void DafeiyuDisplay::SetWakeIndicator(int st) {
    /* ⚠️ 2026-11-02 并发修复（第二处）：本函数由 Application 状态机任务调用，
       内部覆盖层显隐、lv_obj_move_foreground、改一堆 flag，全是 LVGL 内部链表
       操作。而双击切主题会在 tick 里删建 28 个对象 —— 两边并发就会踩坏链表，
       表现正是「双击偶尔崩」。锁是递归锁，和安全上下文嵌套无碍。 */
    DisplayLockGuard lock(this);
    /* 一旦进入聆听/说话，菜单必须让位：唤醒界面和菜单都是整屏覆盖层，
       叠在一起会互相遮。 */
    if (st != 0 && menu_open_) {
        MenuClose();
    }
    /* ⚠️ 2026-11-06 第二版唤醒联动（用户实测第一版「会闪一下主题页」）：
       第一版是唤醒时切回大肥鱼主题、对话结束再切回时钟/壁纸 —— 每次都是
       完整重建界面（pet_theme_start 删建 28 个对象 + 260ms 淡入），所以闪。
       第二版根本不切主题：唤醒本来就要整屏压黑，把时钟/壁纸**临时藏掉**
       就够了，ext_theme_ 不动，对话结束原样恢复（恢复点在 st==0 分支尾部，
       必须放在角色 remove_flag 之后，否则角色会被放出来浮在壁纸上）。 */
    if (st != 0 && InExtTheme()) {
        if (fclock_root_ != nullptr) {
            lv_obj_add_flag(fclock_root_, LV_OBJ_FLAG_HIDDEN);
        }
        if (wallpaper_image_ != nullptr) {
            lv_obj_add_flag(wallpaper_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (clock_panel_ != nullptr) {
            lv_obj_add_flag(clock_panel_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* 唤醒动画二选一（网页「唤醒动画」开关）：大眼睛 或 呼吸球。
       两者都挂在唤醒层，唤醒结束一起收起；待机角色完全不受影响。 */
    ApplyFaceMode();
    /* 呼吸球状态跟随：0=待机 1=聆听 2=说话（颜色/形变/涟漪都由组件内部负责） */
    if (wake_orb_lv_ != nullptr) {
        orb_lv_set_state(wake_orb_lv_,
                         st == 2 ? ORB_SPEAKING : (st == 1 ? ORB_LISTENING : ORB_IDLE));
    }
    if (wake_overlay_ == nullptr) {
        return;
    }
    wake_state_ = st;
    s_wake_state_ = st;      /* 供 /api/state 的 wake 字段诊断 */
    s_ring_made_  = 1;       /* 能走到这里说明光环对象不是 nullptr */
    if (st == 0) {
        /* 待命：收回黑屏，恢复主题、角色和状态栏 */
        lv_obj_add_flag(wake_overlay_, LV_OBJ_FLAG_HIDDEN);
        /* 2026-11-06 回到主题时把音乐可视化的浮层全部清干净 ——
           用户报过「回到主题还有胶囊条残留」，任何状态下的残留都不该带进待机。 */
        music_playing_ = false;
        if (song_label_ != nullptr) {
            song_until_ms_ = 0;
            lv_obj_add_flag(song_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (subtitle_label_ != nullptr) {
            subtitle_until_ms_ = 0;
            lv_obj_add_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (spec_root_ != nullptr) {
            lv_obj_add_flag(spec_root_, LV_OBJ_FLAG_HIDDEN);
        }
        spec_visible_ = false;
        if (ext_theme_ >= 0) {
            /* 扩展主题（时钟/壁纸）下的待机恢复：装饰容器和角色保持隐藏，
               下面 ext_theme_ 分流段自会恢复对应图层（ShowWallpaperTheme /
               fclock），不能在这里把肥鱼放出来 —— 2026-11-06 用户报的
               「壁纸出现后肥鱼在左下角出现几秒」就是这条路径干的。 */
        } else {
            pet_theme_show(1);
            /* ⚠️ 只在开机过渡已经发生后才放角色（boot_finish_ms_ == 0 表示白幕那一下已走完）。
               否则开机后 Application 立刻调一次 SetWakeIndicator(0) 就会把角色提前放出来，
               白幕还没闪，人物先站在那儿了。 */
            if (theme_pet_ != nullptr && boot_finished_ &&
                (boot_finish_ms_ == 0 || boot_flash_ == nullptr)) {
                lv_obj_remove_flag(theme_pet_, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (sys_bar_ != nullptr) {
            lv_obj_remove_flag(sys_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        /* 扩展主题（时钟/壁纸）恢复视图：唤醒时只是临时藏了、没切主题。
           ⚠️ 必须放在上面角色 remove_flag 之后 —— 否则角色会被放出来
           浮在壁纸上面（角色创建晚于壁纸，LVGL 后建的在上）。 */
        if (ext_theme_ == kThemeClockExt) {
            if (fclock_root_ != nullptr) {
                lv_obj_remove_flag(fclock_root_, LV_OBJ_FLAG_HIDDEN);
            }
            if (theme_pet_ != nullptr) {
                lv_obj_add_flag(theme_pet_, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (ext_theme_ == kThemeWallpaperExt) {
            ShowWallpaperTheme();   // 幂等：重显壁纸 + 提时钟/状态栏层 + 重藏角色
        }
        return;
    }
    /* 聆听/说话：整屏压黑 + 呼吸光球。主题容器和角色一起隐藏 ——
       LVGL 会跳过隐藏对象的渲染，省掉约 28 个装饰对象的逐帧刷新。 */
    const lv_color_t c = lv_color_hex(st == 2 ? 0xFF7A29 : 0x2FE0FF);
    pet_theme_show(0);
    if (theme_pet_ != nullptr) {
        lv_obj_add_flag(theme_pet_, LV_OBJ_FLAG_HIDDEN);
    }
    if (sys_bar_ != nullptr) {
        lv_obj_add_flag(sys_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    /* 图元方案（旧兜底）：聆听 = 光球 + 三圈扩散涟漪，说话 = 五条声波条。
       ⚠️ 2026-11-02：以前这里是无条件显示的，选了「大眼睛」唤醒动画后，
       这个 56x56 的青球（wake_orb_）以及光晕/涟漪仍然露出来，
       正好叠在眼睛下面 —— 用户看到的「下层还有个小圆球」就是它。
       现在只在 wake_style == 2（图元方案）时才让它们出场。 */
    const bool speaking = (st == 2);
    const bool legacy   = (GetConfig().wake_style == 2);
    if (wake_halo_ != nullptr) {
        lv_obj_set_style_bg_color(wake_halo_, c, 0);
        if (legacy) lv_obj_remove_flag(wake_halo_, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(wake_halo_, LV_OBJ_FLAG_HIDDEN);
    }
    if (wake_orb_ != nullptr) {
        lv_obj_set_style_bg_color(wake_orb_, c, 0);
        lv_obj_set_style_shadow_color(wake_orb_, c, 0);
        if (legacy && !speaking) lv_obj_remove_flag(wake_orb_, LV_OBJ_FLAG_HIDDEN);
        else                     lv_obj_add_flag(wake_orb_, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 3; i++) {
        if (wake_ripple_[i] == nullptr) continue;
        lv_obj_set_style_border_color(wake_ripple_[i], c, 0);
        if (legacy && !speaking) lv_obj_remove_flag(wake_ripple_[i], LV_OBJ_FLAG_HIDDEN);
        else                     lv_obj_add_flag(wake_ripple_[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 5; i++) {
        if (wake_bar_[i] == nullptr) continue;
        lv_obj_set_style_bg_color(wake_bar_[i], c, 0);
        if (legacy && speaking) lv_obj_remove_flag(wake_bar_[i], LV_OBJ_FLAG_HIDDEN);
        else                    lv_obj_add_flag(wake_bar_[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_remove_flag(wake_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wake_overlay_);
}

uint32_t DafeiyuDisplay::s_status_calls_ = 0;
int  DafeiyuDisplay::s_wake_state_ = -1;
int  DafeiyuDisplay::s_ring_made_  = 0;
char DafeiyuDisplay::s_last_status_[32] = "(无)";

const char* DafeiyuDisplay::WakeDebug() {
    static char buf[128];
    snprintf(buf, sizeof(buf), "wake=%d 次数=%u 末次=%s 栈低=%u字节",
             s_wake_state_, (unsigned)s_status_calls_, s_last_status_,
             (unsigned)s_panic_trace.rsv);
    return buf;
}

void DafeiyuDisplay::SetStatus(const char* status) {
    /* ⚠️ 2026-11-02 并发修复：本函数由 application/协议任务调用，内部 Play()
       会操作 LVGL 对象（切贴图、建/删定时器）。不加锁就和 LVGL 任务并发
       抢同一批内部链表，把 event_head 之类踩坏；之后切主题删除对象、
       lv_event_mark_deleted() 遍历链表时就 LoadProhibited 重启。
       esp_lvgl_port 的锁是递归锁(xSemaphoreCreateRecursiveMutex)，嵌套调用安全。 */
    DisplayLockGuard lock(this);
    LcdDisplay::SetStatus(status);  // 原生标签已隐藏，仅保留内部状态记录
    if (status == nullptr) {
        return;
    }
    /* 诊断计数：网页 /api/state 的 "wake" 字段会显示调用次数和最后一个状态，
       用来确认「唤醒后没变化」到底是 SetStatus 没被调用，还是光环绘制的问题。 */
    s_status_calls_++;
    snprintf(s_last_status_, sizeof(s_last_status_), "%s", status);

    if (StatusIs(status, Lang::Strings::STANDBY)) {
        base_ = Clip::kIdle;
        think_switched_ = false;
        ScheduleNextMicro();
        Play(Clip::kIdle);
        SetWakeIndicator(0);   // 待命：光环收起
    } else if (StatusIs(status, Lang::Strings::CONNECTING)) {
        // 唤醒后的入场动画：伸懒腰，单次播完回待机
        base_ = Clip::kIdle;
        think_switched_ = false;
        ScheduleNextMicro();
        PlayOnce(Clip::kWake);
        SetWakeIndicator(1);   // 刚唤醒：青色
    } else if (StatusIs(status, Lang::Strings::LISTENING)) {
        base_ = Clip::kWait;
        think_switched_ = false;
        if (current_ != Clip::kNone && !IsLoopClip(current_) && gif_ && gif_->IsPlaying()) {
            return_clip_ = Clip::kWait;  // 入场动画还在播，播完接「等待」
        } else {
            Play(Clip::kWait);
        }
        SetWakeIndicator(1);   // 聆听：青色慢呼吸
    } else if (StatusIs(status, Lang::Strings::SPEAKING)) {
        // 说话用「对屏碎碎念」——真的在叨叨，嘴会动
        base_ = Clip::kSpeak;
        think_switched_ = false;
        Play(Clip::kSpeak);
        SetWakeIndicator(2);   // 说话：橙色快脉动
    }
    // 其它（时钟文本、通知等）不切动画
}

void DafeiyuDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);   /* 协议任务调用，必须加锁再碰 LVGL，理由同 SetStatus */

    /* 大眼睛形象：情绪直接映射成眼睛表情。
       放在最前面，是因为它不依赖动画资源、也不受后面那个「反应冷却」影响 ——
       表情本该跟着每句话变。 */
    if (emotion != nullptr) {
        int mood = 0;
        if (std::strstr(emotion, "happy") || std::strstr(emotion, "laugh") ||
            std::strstr(emotion, "funny") || std::strstr(emotion, "smile") ||
            std::strstr(emotion, "joy") || std::strstr(emotion, "wink")) {
            mood = 1;
        } else if (std::strstr(emotion, "angry") || std::strstr(emotion, "annoy") ||
                   std::strstr(emotion, "confident")) {
            mood = 2;
        } else if (std::strstr(emotion, "surprise") || std::strstr(emotion, "shock") ||
                   std::strstr(emotion, "amaz") || std::strstr(emotion, "confus")) {
            mood = 3;
        } else if (std::strstr(emotion, "sleep") || std::strstr(emotion, "tired") ||
                   std::strstr(emotion, "bored") || std::strstr(emotion, "relax")) {
            mood = 4;
        } else if (std::strstr(emotion, "love") || std::strstr(emotion, "heart") ||
                   std::strstr(emotion, "kiss")) {
            mood = 5;
        }
        UpdateEyesFace(mood);
    }
    if (emotion == nullptr || !assets_ready_) {
        return;
    }
    // 正在播一次性片段（入场/情绪反应）就让它播完，别互相打断
    if (current_ != Clip::kNone && !IsLoopClip(current_)) {
        return;
    }
    // 冷却：一句话里服务端可能连发好几个 emotion，不冷却就是反复插播
    if (last_reaction_ms_ != 0 && lv_tick_elaps(last_reaction_ms_) < kReactionCooldownMs) {
        ESP_LOGD(TAG, "emotion '%s' dropped (reaction cooldown %ums)", emotion,
                 kReactionCooldownMs);
        return;
    }
    Clip reaction = EmotionToClip(emotion);
    if (reaction == Clip::kNone) {
        return;
    }
    last_reaction_ms_ = lv_tick_get();
    ESP_LOGI(TAG, "emotion '%s' -> reaction clip", emotion);
    PlayOnce(reaction);
}

void DafeiyuDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);   /* 协议任务调用，必须加锁再碰 LVGL，理由同 SetStatus */
    // 系统消息（配网提示 / 激活验证码 / 升级进度）必须显示，否则这些界面全空
    if (role != nullptr && std::strcmp(role, "system") == 0) {
        ShowSystemMessage(content);
        return;
    }

    // 全屏立绘：不显示对话字幕
    if (content == nullptr) {
        return;
    }

    /* 2026-11-06 TTS 字幕撤掉（用户明确：说话时不显示文字）。
       但**歌词恢复**：放音乐时服务端把唱词当作 TTS 文本随音频一起下发
       （设备端没有独立 LRC 通道），music_playing_ 时显示在屏底，
       字体用 assets 全量字库（6845 字，之前的方块问题已解决）。
       一行歌词顶到下一行或音乐结束，不设倒计时。 */
    if (role != nullptr && std::strcmp(role, "assistant") == 0 &&
        music_playing_ && subtitle_label_ != nullptr) {
        char buf[160];
        snprintf(buf, sizeof(buf), "%s", content);
        buf[sizeof(buf) - 1] = '\0';
        auto theme = static_cast<LvglTheme*>(GetTheme());
        if (theme != nullptr) {
            auto tf = theme->text_font();
            if (tf != nullptr && tf->font() != nullptr) {
                lv_obj_set_style_text_font(subtitle_label_, tf->font(), 0);
            }
        }
        lv_label_set_text(subtitle_label_, buf);
        lv_obj_remove_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(subtitle_label_);
        subtitle_until_ms_ = 0;
    }

    /* 养成玩法（PetGame）已删除：陪聊加成 + 拍照喂食整段逻辑不再需要。
       原为 OnChat() / ConsumeVisionPending() / Feed()。 */
}

void DafeiyuDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);   /* 协议任务调用，理由同 SetStatus */
    // 同上，没有字幕可清
}

void DafeiyuDisplay::SetEmojiCollection(std::shared_ptr<EmojiCollection> collection) {
    /* ⚠️ 2026-11-03 并发修复：本函数由资源/协议任务调用，末尾 FinishBootScreen()
       会建动画、lv_obj_move_foreground —— 这些都会往 LVGL 的全局事件链表 push。
       不加锁就可能和 LVGL 任务同时 push/pop，链头永久悬空到某个任务的栈帧上，
       之后任何 lv_obj_delete 进 lv_event_mark_deleted 就 LoadProhibited（实测切主题必崩）。 */
    DisplayLockGuard lock(this);
    Display::SetEmojiCollection(collection);
    assets_ready_ = true;
    ESP_LOGI(TAG, "emoji collection ready");
    FinishBootScreen();
    if (base_ == Clip::kNone) {
        base_ = Clip::kIdle;
        ScheduleNextMicro();
    }
    if (!booted_) {
        // 第一次拿到素材：先来一段屈膝礼当开机问候，再进待机
        booted_ = true;
        ESP_LOGI(TAG, "boot greeting clip");
        PlayOnce(Clip::kBoot);
        return;
    }
    if (pending_ == Clip::kNone) {
        pending_ = base_;
    }
    Play(pending_);
}
