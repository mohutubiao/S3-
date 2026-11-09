#ifndef DAFEIYU_DISPLAY_H
#define DAFEIYU_DISPLAY_H

#include "display/lcd_display.h"

#include <functional>
#include <memory>

/**
 * 全屏角色（大肥鱼 / 鲸尾女仆）显示层。
 *
 * 与官方 SpiLcdDisplay 的区别：
 *  - 不显示状态栏、不显示对话字幕，整屏只有一张角色动画
 *  - 动画由「设备状态 + 服务端情绪 + 待机小动作」三层驱动
 *  - 素材来自 assets 分区里的 dafeiyu_*.gif（由 _assets_src/make_all.py 生成）
 *
 * 状态层（循环片段）：
 *   STANDBY    -> dafeiyu_idle    待机呼吸
 *   LISTENING  -> dafeiyu_wait    等你说话；2.6 秒没下文 -> dafeiyu_think（在思考）
 *   SPEAKING   -> dafeiyu_speak   吹泡泡，像在回应/发声
 *
 * 情绪层（一次性片段，播完自动回状态层）：
 *   服务端 happy/laughing/funny/... -> dafeiyu_happy（被摸头的开心）
 *   服务端 sad/crying/angry/...     -> dafeiyu_error（冒汗 / 出错）
 *   服务端 sleepy                   -> dafeiyu_yawn（打哈欠）
 *
 * 待机层（随机小动作，25~45 秒一次，让待机不呆板）：
 *   dafeiyu_wake（伸懒腰）/ dafeiyu_yawn（哈欠）/ dafeiyu_eat（吃零食）/ dafeiyu_happy
 *   / dafeiyu_dance（女仆舞）/ dafeiyu_hum（哼歌）/ dafeiyu_pet（撸猫）
 *
 * 实时性约定（重要）：
 *   SetStatus()/SetEmotion() 是**协议任务**调进来的，一次 GIF 装载要分配 384KB
 *   （canvas 4*w*h + frame w*h）并解首帧，直接在回调里做会把协议任务卡住，
 *   表现为 TTS 播放卡顿、AFE 唤醒喂不上数据。所以这里只记 requested_，
 *   真正装载统一放到 LVGL tick（OnTick）里执行；情绪反应额外带冷却，
 *   避免服务端每句都下发情绪时反复重装。
 */
class DafeiyuDisplay : public SpiLcdDisplay {
public:
    DafeiyuDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
    ~DafeiyuDisplay() override;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetEmojiCollection(std::shared_ptr<EmojiCollection> collection) override;
    /** 摄像头拍照时：全屏显示预览，隐藏角色和时钟 */
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

    /** 验收/调试用：按片段名播一次，不用等待机随机触发。可用名字见 .cc 里
     *  ClipListForDebug()。返回 false = 名字不认识。
     *  ⚠️ 只置标记，真正的播放留给 LVGL tick —— HTTP 回调跑在别的线程，
     *  直接在这儿调 lv_obj_* 会跟刷屏抢同一份状态。 */
    bool PlayClipByName(const char* name);
    /** 供网页列出所有可用名字（逗号分隔） */
    static const char* ClipListForDebug();
    /** 网页验收接口要用到实例；HTTP 回调在另一个线程，只能通过它拿到 */
    static DafeiyuDisplay* Instance() { return instance_; }

    // ── 追查「音量胶囊不出现」用 ──────────────────────────────────────────
    // 我先后猜过「被立绘盖住」「被 SetupUI 隐藏」「基类显示逻辑有 bug」，三次全错。
    // 这次不猜了：接管 ShowNotification 记录调用次数，挂到网页 /api/state 的 notify 字段。
    // 按几下音量键读一次网页就能定性：
    //   notify 一直是 0  → 压根没走到显示这层，问题在按键/音量那条链路上
    //   notify 在涨但看不见 → 显示层还有问题，再往下查
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    static uint32_t NotifyCount() { return notify_count_; }
    /** 最近一次通知的原文。用自带 buffer 存一份，不能留调用方的指针（会悬空） */
    static const char* LastNotifyText() { return last_notify_buf_; }
    /** 上次复位原因的文字说明（追随机重启用，网页 /api/state 里能看到） */
    static const char* LastResetReason() { return last_reset_name_; }

    /** ── 网页控制页的视图切换（/api/view 调用，2026-11-06 用户要求）──
        id: 0..8 = 大肥鱼主题，100 = 翻页时钟，101 = 自定义壁纸。
        HTTP 线程调用；扩展主题/跨大扩展切换走 theme_go_req_（tick 消费），
        肥鱼内部直切走 ApplyThemeTo（自带 LVGL 递归锁），两边都线程安全。 */
    bool WebSetView(int id);
    /** 当前视图 id（0..8/100/101），供 /api/state 回显 */
    int  WebCurrentView() const;
    /** 时钟样式 0..3：n<0 只读，n>=0 设置并写 NVS。返回当前生效样式。 */
    int  WebClockStyle(int n);
    /** 大肥鱼主题名（0..8），网页下拉框用；越界返回空串 */
    const char* WebThemeName(int id) const;
    /** 重新从 storage 读壁纸并刷新 LVGL image（网页上传后热更新用）。
        HTTP 线程可调（自带 LVGL 递归锁）。之前只在开机/config 变更时加载，
        wallpaper_enabled 关着时 buf 永远是空的 —— 壁纸主题下就是一片黑。 */
    void RefreshWallpaper();

    /** 上次崩溃时执行到切主题的哪一步（RTC 内存记录，掉电才丢）。
        用来排查「拔掉串口才崩」这类抓不到串口现场的问题，网页 /api/state 可读。 */
    static const char* LastPanicStep();

    /** 唤醒状态诊断：/api/state 的 "wake" 字段。
        唤醒光环实测「没有任何变化」，用来确认 SetStatus() 到底有没有被调用、
        光环对象是否建成功、当前状态是几 —— 避免继续靠猜。 */
    static const char* WakeDebug();
    static uint32_t s_status_calls_;
    static int  s_wake_state_;    // -1=从未调用 0=待命 1=聆听 2=说话
    static int  s_ring_made_;     // 0=光环对象为 nullptr（没建成） 1=已建
    static char s_last_status_[32];

    /** 双击音量键切换主题：dir = +1 下一套，-1 上一套 */
    void SwitchTheme(int dir);

    /* ── 按键菜单（长按 BOOT 进入；音量键上下移动；BOOT 确认；长按 BOOT 退出）──
       ⚠️ 按键回调跑在按键任务里，直接碰 LVGL 会崩（双击切主题就踩过这个坑，
       见 SwitchTheme 的注释）。所以这里只写一个请求码，
       真正的绘制与动作全部在 LVGL tick 里执行（ApplyMenuRequest）。 */
    enum MenuReq {
        kMenuNone = 0,
        kMenuOpen,
        kMenuClose,
        kMenuUp,
        kMenuDown,
        kMenuConfirm,
    };
    /* 菜单项 id —— 板子侧的动作回调也用它认项（配网 / 重启） */
    enum MenuItem {
        kMenuVolume = 0,
        kMenuBright,
        kMenuTheme,     // 大肥鱼系列：只在这 9 套里循环，绝不落到时钟/壁纸
        kMenuClock,     // 2026-11-06 直达「翻页时钟」扩展主题（原来只是临时浮层）
        kMenuWallpaper, // 2026-11-06 直达「自定义壁纸」扩展主题
        kMenuQr,        // 控制页二维码
        kMenuWifi,
        kMenuReboot,
        kMenuCount
    };
    void MenuRequest(MenuReq req) { menu_req_ = static_cast<int>(req); }
    bool MenuIsOpen() const { return menu_open_; }
    /** 控制页二维码浮层是否可见。BOOT 单击在浮层显示时只负责关浮层，
        不许顺带触发对话（否则二维码还没看清就进了聊天）。 */
    bool ControlPageVisible() const { return qr_visible_; }
    /** 关闭控制页二维码浮层（可在任意线程调，内部置标志、tick 执行） */
    void CloseControlPage() { qr_close_req_ = true; }
    /** 2026-11-06 翻页时钟模式：全屏浮层，菜单「时钟」进入，
        BOOT 单击 / 长按退出（同二维码浮层的约定） */
    bool ClockModeVisible() const { return clock_mode_visible_; }
    void CloseClockModeReq() { clock_close_req_ = true; }
    /** MCP 工具 self.music.now：点歌后上报歌名，屏幕顶部显示约 20 秒 */
    void SetSongTitle(const char* title);
    void SetMusicPlaying(bool on);   /* 2026-11-06 音乐起/停：频谱只认这个标志 */
    /** 板级动作（配网 / 重启）交回板子实现：返回 true 表示已处理并收起菜单。
        参数是菜单项 id（见 MenuItem）。 */
    void SetMenuActionHandler(std::function<bool(int)> handler) { menu_action_ = std::move(handler); }

    /** 绝对切到某一套主题（id 见 pet_theme_id_t）。
        只置请求标志，真正切换在 tick 里做 —— 跨线程安全，同 SwitchTheme。
        供 MCP 工具 self.pet.set_theme 使用。 */
    void SetThemeById(int id);

    /** 动作自动变换开关（静态，网页 /api/auto 可直接调，不需要实例）。
        on=true   允许 pet_idle_plan 每 5~12 秒随机换动作；
        on=false  固定在当前主题配套动作。
        默认 false —— 用户要求「切到哭就一直是哭，不要过一会变别的」。 */
    static void SetAutoEmoteEnabled(bool on) { s_auto_emote_ = on; }
    static bool AutoEmoteEnabled() { return s_auto_emote_; }

private:
    enum class Clip {
        kNone = 0,
        // 循环（状态层）
        kIdle,
        kWait,
        kThink,
        kSpeak,
        // 一次性（情绪层 / 待机层 / 唤醒入场）
        kWake,
        kSuccess,
        kError,
        kHappy,
        kYawn,
        kEat,
        // 待机小动作专属
        kDance,
        kHum,
        kPet,
        // 开机问候
        kBoot,
        // 分出来的情绪（2026-10-31）：素材里有独立的「哭」和「生气」两张，
        // 原先它们跟尴尬、难过一起被并到「惊恐」那张脸上，表情区分度不够
        kSad,
        kAngry,
        // 侧身走路。只有 994/995 两张帧，素材里做成正-反-正重复，
        // 当作一次性片段播一小段再回正面站姿
        kWalk,
    };

    static void TickCallback(lv_timer_t* timer);
    void OnTick();

    /** 切换到某个片段；assets 还没就绪时只记录，等 tick 重试 */
    void Play(Clip clip);
    /** 播放一次性片段，播完回到当前状态层 */
    void PlayOnce(Clip clip);
    /** 按素材实际尺寸居中摆放角色图（素材比屏幕窄时不做拉伸） */
    void FitImageObject(lv_coord_t w, lv_coord_t h);
    /** 调用前必须已持有 LVGL 锁 */
    bool PlayLocked(Clip clip);
    void StopGifLocked();
    void ScheduleNextMicro();
    /** 待机时钟：只有待机且不在拍照预览时才显示 */
    void UpdateClock();
    void SetClockVisible(bool visible);
    /** 翻页时钟模式（2026-11-06）：canvas 像素直绘，分钟变化时卡片翻页。
       只能在 LVGL 线程调；浮层只能隐藏不能删除（同菜单约定）。 */
    void ShowClockMode();
    void CloseClockMode();
    void ClockTick();
    /** 扩展主题「自定义壁纸」：全屏壁纸 + 时钟卡片，角色隐藏。
        只能进扩展主题分流（ApplyThemeTo）和 tick 里调。 */
    void ShowWallpaperTheme();
    void CloseWallpaperTheme();
    /** 重新摆放各图层（桌面模式：角色居中 / 信息模式：时钟居中，角色让到右下角） */
    void ApplyLayout(bool info_mode);
    /** 按养成数值挑待机表现（饿→讨食 / 困→哈欠 / 熟→求摸 / 好→跳舞），kNone = 维持常态 */
    Clip PickIdleActionByStats();
    /** 实时参数化动作：对每个片段叠加 sine 变换（缩放/浮动/摆动/倾斜/弹跳/挤压），
     *  换片段时有统一的入场弹性 + 各状态自己的前摇。详见 .cc 里的 MotionParams。 */
    void ApplyMotionTransform();
    /** 说话时拉长 LVGL 刷新周期，把带宽让给音频（见 kRefrSpeakMs 的注释） */
    void ApplyRefrPeriod();
    /** 开机动画（LVGL 自绘：呼吸圆环 + 进度条 + 阶段文字），不依赖角色素材 */
    void CreateBootScreen();
    void UpdateBootScreen();
    void FinishBootScreen();
    /** 系统消息：配网 / 激活验证码 / 升级进度，底部中文 + 大号验证码 */
    void ShowSystemMessage(const char* text);
    /** 刷新屏幕上的 IP（每 5 秒查一次，拿到就一直显示） */
    void UpdateIpLabel();
    /** 应用网页配下来的外观（背景色 / 壁纸 / 时钟开关），配置版本号变了才做 */
    void ApplyConfigIfChanged();
    /** 养成系统产出的动作 -> 播对应片段 */
    void HandlePetAction();
    /** 右上角养成状态条 */
    void UpdateStatusBars(bool force);

    static const char* AssetName(Clip clip);
    static bool IsLoopClip(Clip clip);
    static Clip EmotionToClip(const char* emotion);
    static bool StatusIs(const char* status, const char* ref);

    lv_obj_t* pet_image_ = nullptr;
    lv_timer_t* tick_timer_ = nullptr;
    std::unique_ptr<LvglGif> gif_;
    Clip current_ = Clip::kNone;    // 正在播
    Clip base_ = Clip::kNone;       // 状态层（一次性播完回到这里）
    Clip return_clip_ = Clip::kNone;  // 一次性播完的落点（优先于 base_）
    Clip pending_ = Clip::kNone;    // 资产没就绪时暂存的期望片段
    Clip requested_ = Clip::kNone;  // 已请求、等 tick 真正装载的片段
    // 顶部状态栏：电量 + WiFi。
    // ⚠️ 创建顺序很重要 —— 必须排在 pet_image_ 之后！LVGL 是后建的盖在上面，
    // 立绘是全屏的，早建的会被整块遮住（音量胶囊看不见就是这个原因）。
    // ⚠️ 别再叫 status_bar_！基类 LcdDisplay 有一个同名成员（原生顶栏），
    // 同名会遮蔽它，导致 SetupUI 里那段 hidden[] 藏错对象（详见 .cc 里的注释）。
    lv_obj_t* sys_bar_ = nullptr;
    lv_obj_t* battery_outline_ = nullptr;
    lv_obj_t* battery_fill_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* wifi_signal_ = nullptr;    // 信号图标容器（三根竖条，替代原来的纯文字）
    lv_obj_t* wifi_bars_[3] = {nullptr};  // 由弱到强
    uint32_t last_status_ms_ = 0;

    // 开机动画（自绘：assets 分区约 9 秒后才挂载，开机阶段用不了 GIF 素材）
    lv_obj_t* boot_arc2_ = nullptr;   // 第二层环，反向转，制造层次
    lv_obj_t* boot_core_ = nullptr;   // 中心呼吸光点
    int boot_breath_ = 0;             // 呼吸相位计数器
    lv_obj_t* boot_cfg_label_ = nullptr;  // 配网信息（热点名 + 访问地址），融入开机动画用
    bool ui_settled_ = false;             // 首次 tick 才会 true：那时布局才算完，可以安全定位
    bool theme_ready_ = false;            // 主题引擎是否创建成功（失败则回退旧界面）
    lv_obj_t* theme_pet_ = nullptr;       // 主题的角色对象：需每帧提最前，见 tick 里的说明
    /* ── 扩展主题（2026-11-06 用户要求三大主题：大肥鱼系列/翻页时钟/自定义壁纸）──
       pet_themes.c 里那 9 套是「大肥鱼系列」。翻页时钟和自定义壁纸不走
       pet_theme_start（引擎只管角色主题），改由 ApplyThemeTo 分流到
       现成的 fclock 浮层 / 壁纸图层，但对外仍是同一个主题 id 空间：
       菜单循环、双击音量键、NVS 断电记忆、MCP self.pet.set_theme 全部通用。 */
    static constexpr int kThemeClockExt     = 100;  // 翻页时钟（复用 fclock 全屏浮层）
    static constexpr int kThemeWallpaperExt = 101;  // 自定义壁纸（全屏壁纸 + 时钟卡片）
    bool InExtTheme() const { return ext_theme_ >= 0; }
    int  ext_theme_ = -1;                 // 当前扩展主题，-1 = 普通大肥鱼主题
    /* 双击音量键的切主题请求（+1/-1）。按键回调只置这个标志，
       真正的切换在 tick 里做 —— LVGL 不是线程安全的，跨线程调用必崩。 */
    volatile int theme_switch_req_ = 0;
    void ApplyThemeSwitch(int dir);       // 真正的切主题，只可在 tick（LVGL 线程）里调

    /** 刷新电量/WiFi；内部自带 3 秒节流，可以每 tick 放心调 */
    void UpdateStatusBar();

    Clip debug_clip_ = Clip::kNone;  // 网页验收接口请求的片段（tick 里取播，避免跨线程碰 LVGL）
    static DafeiyuDisplay* instance_;  // 单例指针，见 Instance()
    static const char* last_reset_name_;  // 上次复位原因的文字说明，见 LastResetReason()
    static bool s_auto_emote_;            // 动作自动变换开关，见 SetAutoEmoteEnabled()

    /* 唤醒状态光环：屏幕四周一圈呼吸边框，让「唤醒/聆听/说话」和平时主题画面
       一眼区分开。用户明确说「文字显示无所谓」，所以完全不用文字，避免
       中文字体子集缺字变空白的问题。
       0=待命(隐藏) 1=聆听(青色慢呼吸) 2=说话(橙色快脉动) */
    /* 唤醒界面（2026-11-02 重做）：全黑屏 + 呼吸光球，取代原来的边框光环。
       唤醒时把主题容器和角色一并隐藏 —— LVGL 会跳过隐藏对象的渲染，
       省掉约 28 个装饰对象的逐帧刷新，对话时确实省 CPU。 */
    lv_obj_t* wake_overlay_ = nullptr;   // 全黑底
    lv_obj_t* wake_orb_     = nullptr;   // 中心光球（聆听时出现）
    lv_obj_t* wake_halo_    = nullptr;   // 外层光晕
    /* 2026-11-02 重做：聆听 = 三圈向外扩散的涟漪（像在收声）；
       说话 = 五条跳动的声波条（像均衡器在放声）。形状完全不同，一眼可分。 */
    lv_obj_t* wake_ripple_[3] = {};      // 备用：扩散涟漪（画布不可用时才用）
    lv_obj_t* wake_bar_[5]    = {};      // 备用：跳动声波条
    /* 2026-11-02：画布版呼吸球，参考 breathing-orb.html 的 GPT 语音球效果。
       逐像素写 RGB565 缓冲（放 PSRAM），能做出 LVGL 图元做不到的
       柔和渐变、有机形变和频谱环。缓冲申请失败则退回上面的图元方案。 */
    lv_obj_t*  wake_canvas_     = nullptr;
    uint8_t*   wake_canvas_buf_ = nullptr;   // I8 索引色：每像素 1 字节
    /* xiaozhi-orb 组件（main/orb/）：纯整数渲染，比手写浮点版省一个数量级。
       orb_lv_t 定义在 orb_lvgl.h，这里只前置声明，避免头文件互相依赖。 */
    struct orb_lv_s* wake_orb_lv_ = nullptr;

    /* ── 大眼睛待机形象（RoboEyes 风格）────────────────────────────────
       纯 LVGL 图元：圆角矩形当眼白，黑色子对象当眼睑（被子对象裁剪，
       天然只在眼睛里生效）。0 额外内存、0 图片资源，情绪直接改尺寸/颜色。
       网页控制端 "待机形象" 开关可在它和原来的角色主题之间切换。 */
    lv_obj_t* face_root_ = nullptr;                 // 脸容器（双眼+嘴）
    lv_obj_t* eye_[2]    = {nullptr, nullptr};      // 眼白
    lv_obj_t* pupil_[2]  = {nullptr, nullptr};      // 瞳孔（会左右飘，避免呆滞）
    lv_obj_t* lid_[2]    = {nullptr, nullptr};      // 眼睑（黑，盖在眼白上）
    lv_obj_t* mouth_     = nullptr;                 // 嘴（形状随情绪变）
    int       face_mood_ = 0;                       // 0默认 1开心 2生气 3惊讶 4困倦 5爱心
    uint32_t  blink_next_ms_ = 0;                   // 下次眨眼时间
    uint32_t  face_look_ms_  = 0;                   // 下次转眼珠时间
    int8_t    face_look_dx_  = 0;                   // 眼珠当前偏移
    int8_t    face_look_dy_  = 0;
    bool      face_talking_  = false;               // 上一帧是否在说话（用于恢复嘴形）
    void BuildEyesFace();
    void UpdateEyesFace(int mood);
    void AnimateEyes(uint32_t now_ms);
    void DrawMouth(int mood, int open);      // 细弧线嘴：抛物线(微笑/撇嘴) 或 椭圆(O)
    void ApplyFaceMode();

    /* 开机过渡：整屏白幕闪一下再淡出，遮住「人物已出现、时钟还没来」那段割裂，
       同时让角色淡入。只在开机动画收起时触发一次。 */
    lv_obj_t* boot_flash_ = nullptr;
    void PlayBootTransition();

    /* ── 按键菜单 ── */
    lv_obj_t* menu_root_ = nullptr;                 // 覆盖层（挂在 top layer，盖住角色）
    lv_obj_t* menu_row_[kMenuCount] = {nullptr};
    lv_obj_t* menu_tip_ = nullptr;                  // 底部操作提示
    int  menu_sel_ = 0;                             // 当前选中项
    volatile int menu_req_ = 0;                     // 按键任务写 / LVGL 任务读
    bool menu_open_ = false;
    bool menu_edit_ = false;                        // 正在调音量或亮度（此时音量键是加减）
    std::function<bool(int)> menu_action_;
    void BuildMenu();
    void ApplyMenuRequest();
    void UpdateMenuUI();
    void MenuClose();
    void MenuMove(int dir);
    void MenuConfirm();

    /* ── 2026-11-05 音乐可视化（频谱条 / TTS 字幕 / 歌名）──
       创建一次、只隐藏不删除（同菜单的约定）；LVGL 操作只发生在
       SetupUI / OnTick / SetChatMessage（后两者都持有 display 锁）。 */
    lv_obj_t* spec_root_  = nullptr;      // 频谱容器（240x72 居中）
    lv_obj_t* spec_canvas_ = nullptr;     // 2026-11-06 canvas 版频谱：像素直绘
    uint16_t* spec_buf_ = nullptr;        // canvas 缓冲（PSRAM，240x72 RGB565）
    uint8_t   spec_h_[16] = {0};          // 当前柱高（像素，含变更检测），16 根真 FFT
    uint8_t   spec_peak_[16] = {0};       // 峰值保持点（像素）
    bool spec_visible_ = false;
    uint32_t spec_last_active_ms_ = 0;
    void BuildSpectrum();                 // 只能在 LVGL 线程调
    void UpdateSpectrum();                // tick 里调，内部 40ms 节流

    lv_obj_t* subtitle_label_ = nullptr;  // 屏底字幕（TTS 文本，歌词替代）
    uint32_t subtitle_until_ms_ = 0;      // 0 = 不显示
    lv_obj_t* song_label_ = nullptr;      // 顶部歌名
    uint32_t song_until_ms_ = 0;
    volatile bool music_playing_ = false;   /* 2026-11-06 MCP 上报的音乐播放中 */

    /* ── 控制页二维码浮层（菜单「控制页」项打开）── */
    lv_obj_t* qr_root_ = nullptr;         // 全屏深色底
    lv_obj_t* qr_canvas_ = nullptr;       // 二维码本体（qrcodegen 点阵画进 canvas）
    uint16_t* qr_canvas_buf_ = nullptr;   // canvas 像素缓冲（SPIRAM）
    lv_obj_t* qr_url_label_ = nullptr;    // 二维码下方的 URL 文本
    bool qr_visible_ = false;
    volatile bool qr_close_req_ = false;  // 按键任务写、tick 执行
    void ShowControlPageQr();             // 只能在 LVGL 线程调

    /* ── 翻页时钟模式（菜单「时钟」项打开，BOOT 单击/长按退出）──
       canvas 像素直绘（与频谱同套路）：aurora 皮肤 4 卡 + 冒号 + 日期，
       分钟变化时变化卡两段式翻页（320ms，级联 110ms/卡）。 */
    lv_obj_t* fclock_root_ = nullptr;       // 全屏深色底
    lv_obj_t* fclock_canvas_ = nullptr;     // 卡片/数字/冒号 canvas
    uint16_t* fclock_buf_ = nullptr;        // canvas 缓冲（PSRAM 320x240 RGB565）
    lv_obj_t* fclock_date_ = nullptr;       // 日期行（全量字库）
    bool clock_mode_visible_ = false;
    volatile bool clock_close_req_ = false; // 按键任务写、tick 执行
    int8_t  fclock_digit_[4] = {-1, -1, -1, -1};   // 当前数字（-1=未同步）
    int8_t  fclock_prev_[4] = {-1, -1, -1, -1};    // 翻页前数字
    uint32_t fclock_anim_start_[4] = {0, 0, 0, 0}; // 翻页起始时刻（0=无动画）
    uint16_t fclock_last_min_ = 0xFFFF;     // 上次绘制分钟
    bool    fclock_colon_ = true;           // 冒号亮/灭
    uint32_t fclock_last_blink_ms_ = 0;
    int8_t  fclock_style_ = -1;             // 翻页时钟样式（NVS 记忆，-1=未载入）
    /** 双击音量键在时钟主题下的分流目标：在 4 套样式里循环（NVS 记忆） */
    void CycleClockStyle(int dir);

    /* 绝对切主题请求（-1 = 无）：MCP 工具写，tick 里执行 */
    volatile int theme_go_req_ = -1;
    void ApplyThemeTo(int id);
    void RenderWakeOrb(uint32_t ms, int st);
    int wake_state_ = 0;
    void SetWakeIndicator(int st);
    static uint32_t notify_count_;        // ShowNotification 被调用次数（追胶囊用）
    static char last_notify_buf_[64];     // 最近一次通知原文
    uint32_t clip_start_ms_ = 0;    // 当前片段起播时刻（判断聆听是否该切思考）
    uint32_t next_micro_ms_ = 0;    // 下一次待机小动作的时刻
    uint32_t last_reaction_ms_ = 0; // 上一次情绪反应的时刻（冷却用）
    bool think_switched_ = false;   // 本轮聆听是否已经切过思考
    bool assets_ready_ = false;
    bool booted_ = false;           // 开机问候是否播过
    bool previewing_ = false;       // 摄像头预览中（此时角色/时钟让位）

    lv_obj_t* clock_panel_ = nullptr;   // 时钟卡片（半透明圆角底）
    lv_obj_t* clock_label_ = nullptr;   // 待机时钟 HH:MM
    lv_obj_t* clock_date_label_ = nullptr;  // 时钟下方的日期/星期
    lv_obj_t* ip_label_ = nullptr;      // IP（常驻，方便访问网页控制页）
    Clip last_idle_clip_ = Clip::kNone; // 上一次待机演了哪段（防重复、像卡碟）
    uint32_t refr_period_ms_ = 0;       // 当前 LVGL 刷新周期（0 = 还没设过）
    int last_clock_min_ = -1;           // 分钟变了才刷文本
    bool clock_logged_ = false;         // 时间首次生效打一条日志，方便排查同步
    uint32_t last_ip_ms_ = 0;
    char ip_text_[24] = "";

    // 开机动画（自绘，assets 就绪后淡出销毁）
    lv_obj_t* boot_panel_ = nullptr;
    lv_obj_t* boot_arc_ = nullptr;
    lv_obj_t* boot_bar_ = nullptr;
    lv_obj_t* boot_text_ = nullptr;
    uint32_t boot_start_ms_ = 0;
    uint32_t boot_finish_ms_ = 0;   // assets 就绪、开始收尾的时刻
    uint32_t boot_spin_ = 0;        // 圆环转角（tick 里推进，不用 lv_anim，省得管生命周期）
    bool boot_finished_ = false;

    // 系统消息（配网 / 激活 / 升级）
    lv_obj_t* sys_label_ = nullptr;      // 中文说明（用主题字体，能渲染中文）
    lv_obj_t* sys_code_label_ = nullptr; // 大号验证码（数字，48 号字）
    uint32_t sys_msg_ms_ = 0;           // 系统消息显示时刻（20 秒后自动收）

    // 壁纸（RGB565 原始像素，网页上传，存在 storage 分区）
    lv_obj_t* wallpaper_image_ = nullptr;
    lv_image_dsc_t wallpaper_dsc_{};
    uint8_t* wallpaper_buf_ = nullptr;
    /** 壁纸主题的左上大时钟（2026-11-06 用户要求：时间放左边、
        右上胶囊只留电量信号）。半透明胶囊底 + montserrat_48 大字 HH:MM。 */
    lv_obj_t* wp_clock_ = nullptr;
    lv_obj_t* wp_clock_label_ = nullptr;
    uint16_t last_wp_min_ = 0xFFFF;    // 分钟变了才重写
    uint32_t config_version_ = 0;

    // 养成状态卡片
    lv_obj_t* stat_panel_ = nullptr;
    lv_obj_t* hunger_bar_ = nullptr;
    lv_obj_t* mood_bar_ = nullptr;
    lv_obj_t* bond_bar_ = nullptr;
    lv_obj_t* hunger_val_ = nullptr;
    lv_obj_t* mood_val_ = nullptr;
    lv_obj_t* bond_val_ = nullptr;
    lv_obj_t* level_label_ = nullptr;
    uint32_t last_bar_ms_ = 0;
};

#endif  // DAFEIYU_DISPLAY_H
