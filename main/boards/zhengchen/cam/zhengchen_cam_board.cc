#include "wifi_board.h"
#include "audio/codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "dafeiyu_display.h"
#include "dafeiyu_web.h"
#include "cam_stream.h"         // 图传（81 口独立 httpd）：配网时要停掉让内存
#include "application.h"
#include "button.h"
#include "led/single_led.h"     // 框架 Led 基类经它间接可用（彩灯用 ws2812_led.h）
#include "ws2812_led.h"         // 板载 WS2812 彩灯（语音/网页控制）
#include "esp_now_link.h"       // 小车语音遥控：self.car.move → ESP-NOW 发 C3 从机
#include "car_action.h"         // 动作预设/序列调度（移植 C3MINI 那套 self.car.act）
#include "config.h"
#include "i2c_device.h"
#include "esp32_camera.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_timer.h>           // 进车模式后延迟启动 ESP-NOW 心跳
#include <esp_wifi.h>            // esp_wifi_sta_get_ap_info：WiFi 真连上才启动 ESP-NOW
#include <esp_system.h>          // esp_restart：菜单里的「重启」
#include <esp_lcd_panel_vendor.h>
#include <esp_sntp.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include "power_manager.h"
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include "settings.h"
#include "mcp_server.h"          // 自定义 MCP 工具：self.pet.set_theme
#include "pet_themes.h"          // PET_THEME_*：主题 id 映射

#define FIRST_BOOT_NS "boot_config"  
#define FIRST_BOOT_KEY "is_first"    


#define TAG "ZhengchenCamBoard"

/* 2026-11-15 修复「按 BOOT 回待机，10ms 后自己又进对话」：
   双击处理里那段"撤销第一下单击打开的对话"假设第一下是**从待机进入**。
   实际用户第一下是**退出对话**，撤销方向就反了 —— 退出后立刻又被拉进来
   （日志：`listening -> idle` 10ms 后 `idle -> connecting`，且没有 Wake word）。
   这里记住单击发生时是不是待机态，双击撤销只在"确实是进入"时才做。 */
static bool s_boot_click_from_idle = false;

// ---------------------------------------------------------------- LCD 颜色模式（2026-11-04 反色修复开关）
// 网页 /api/config?invert=0&rgb_swap=1 实时可切，NVS("lcd_display") 持久化。
// 背景热切换继承 GameBox 的面板状态 / 部分面板 RGB-BGR 顺序相反，都能在这里拨正。
//
/* ✅ 本板（正辰/大飞鱼 320x240 ST7789）出厂默认颜色参数。
   invert=1（面板需要反色指令才显示正色）。
   rgb_swap=0（RGB 顺序，MADCTL BGR 位=0）。
   ⚠️ 2026-11-06 最终定案（用户在控制页点「红蓝互换」后颜色立刻正确）：
      正确值是 swap=0。11-05 记的「swap=1 正确」是误判（当时和 invert 的
      修正混在一起观察了）。GameBox 侧同步逻辑已同升 color_ver=3。
   ⚠️ 这两个数字是「别人烧录后第一眼看到的颜色」，改之前务必真机确认；
      所有用到默认值的地方（静态初始化 / GetInt 缺省值）都必须引用这两个常量，
      不要再写裸数字。 */
constexpr int kDefaultLcdInvert = 1;
/* 2026-11-06 实测定案（用户网页「红蓝互换」后颜色立刻正确）：
   这块 ST7789 要 RGB 顺序（MADCTL BGR=0），swap=1 是错的。
   之前 v2 迁移写 swap=1 是误判，这里改回 0 并升 color_ver=3 强制覆盖。 */
/* 2026-11-08 v4 定案：SWRESET 让面板每次都回真出厂 POR 态，实验证明
   POR 正确参数 = invert=1 + BGR=1（11-06 的 swap=0 是在被污染状态校准的）。 */
/* 2026-11-10 用户再次实测反色 → 回到 11-06 用户亲手点网页验证过的值：
   RGB 顺序（BGR=0）。11-08 的 BGR=1 定案是混在 GameBox 的 bswap 逻辑里
   一起校的，小智 esp_lcd 路径没有 bswap，不能照搬。 */
constexpr int kDefaultLcdRgbSwap = 0;

static esp_lcd_panel_handle_t s_lcd_panel = nullptr;
static esp_lcd_panel_io_handle_t s_lcd_panel_io = nullptr;
static int s_lcd_invert = kDefaultLcdInvert;
static int s_lcd_rgb_swap = kDefaultLcdRgbSwap;

static void LcdApplyMadctl() {
    /* MADCTL: MY=0x80 MX=0x40 MV=0x20(行列互换) BGR=0x08。
       esp_lcd 的 swap_xy/mirror 只写 MV/MX/MY，BGR 位只在 init 时按
       rgb_ele_order 写一次 —— 运行期改红蓝顺序必须整字节重发。 */
    if (s_lcd_panel_io == nullptr) return;
    Settings disp_cfg("lcd_display", true);
    bool landscape = disp_cfg.GetInt("lcd_mode", 1) != 0;
    /* 2026-11-11 小车模式固定竖屏：表情按 240x320 渲染。NVS 的 lcd_mode
       可能是旧值（横屏实验版写过 1），这里强制覆盖，避免屏幕方向对不上。 */
    /* 2026-11-15 三模式：ui_mode=1 就是小车，同样强制竖屏（老 NVS 没 ui_mode 时按 car_mode） */
    if (disp_cfg.GetInt("ui_mode", disp_cfg.GetInt("car_mode", 0)) == 1) landscape = false;
    bool mx = landscape ? DISPLAY_MIRROR_X : DISPLAY_MIRROR_X_1;
    bool my = landscape ? DISPLAY_MIRROR_Y : DISPLAY_MIRROR_Y_1;
    bool mv = landscape ? DISPLAY_SWAP_XY : DISPLAY_SWAP_XY_1;
    uint8_t madctl = (uint8_t)((my ? 0x80 : 0) | (mx ? 0x40 : 0) |
                               (mv ? 0x20 : 0) | (s_lcd_rgb_swap ? 0x08 : 0));
    esp_lcd_panel_io_tx_param(s_lcd_panel_io, 0x36, (const uint8_t[]){madctl}, 1);
    ESP_LOGI(TAG, "MADCTL 下发: 0x%02X (BGR=%d invert=%d) NVS rgb_swap=%d", madctl,
             (madctl & 0x08) ? 1 : 0, s_lcd_invert, s_lcd_rgb_swap);
}

/* 2026-11-10 用户定案：DafeiyuSetLcdColorMode / Get 系列已删 ——
   颜色参数板级修死不读 NVS，网页不再提供颜色开关（错值进 NVS 是
   红蓝反色反复出现的帮凶）。 */

/* 2026-11-08 反色复发彻底修复：不再依赖单一时序点。启动后 3 秒、8 秒
   各补发一次全套颜色命令（INVOFF/INVON + MADCTL）—— 无论 GameBox 热切
   时哪条命令被总线残留吞掉，最终都会被纠正（重复发同一值无副作用）。 */
static void LcdReapplyColor() {
    if (s_lcd_panel == nullptr) return;
    esp_lcd_panel_invert_color(s_lcd_panel, s_lcd_invert != 0);
    LcdApplyMadctl();

    /* v7 读回诊断：另建 2MHz 低速 panel_io（80MHz 下 SPI 读不可靠），
       读 RDDPM/RDDMADCTL/RDDCOLMOD 与写入值对比 */
    esp_lcd_panel_io_spi_config_t rd_cfg = {};
    rd_cfg.cs_gpio_num = GPIO_NUM_NC;
    rd_cfg.dc_gpio_num = GPIO_NUM_39;
    rd_cfg.spi_mode = 0;
    rd_cfg.pclk_hz = 2 * 1000 * 1000;
    rd_cfg.trans_queue_depth = 1;
    rd_cfg.lcd_cmd_bits = 8;
    rd_cfg.lcd_param_bits = 8;
    esp_lcd_panel_io_handle_t rd_io = nullptr;
    if (esp_lcd_new_panel_io_spi(SPI3_HOST, &rd_cfg, &rd_io) == ESP_OK) {
        uint8_t pm = 0, mad = 0, col = 0;
        esp_err_t e1 = esp_lcd_panel_io_rx_param(rd_io, 0x0A, &pm, 1);
        esp_err_t e2 = esp_lcd_panel_io_rx_param(rd_io, 0x0B, &mad, 1);
        esp_err_t e3 = esp_lcd_panel_io_rx_param(rd_io, 0x0C, &col, 1);
        ESP_LOGW(TAG, "PANEL RD: err=%d/%d/%d PM=0x%02X MADCTL=0x%02X COLMOD=0x%02X (期望 BGR=0 COLMOD=0x55)",
                 (int)e1, (int)e2, (int)e3, pm, mad, col);
        esp_lcd_panel_io_del(rd_io);
    }
}

static void LcdScheduleColorReapply() {
    for (int delaySec : {3, 8}) {
        const esp_timer_create_args_t args = {
            .callback = [](void*) { LcdReapplyColor(); },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "lcd_color_reapply",
            .skip_unhandled_events = true,
        };
        esp_timer_handle_t timer = nullptr;
        if (esp_timer_create(&args, &timer) == ESP_OK) {
            esp_timer_start_once(timer, (uint64_t)delaySec * 1000000ULL);
        }
    }
}

//控制器初始化函数声明
void InitializeMCPController();

LV_FONT_DECLARE(font_noto_sans_basic_20_4);
LV_FONT_DECLARE(font_material_symbols_20_4);

class Pca9557 : public I2cDevice {
public:
    Pca9557(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        /* ⚠️ 2026-11-08 反色复发防御：esp_restart 热重启时 GameBox 的 I2C
           事务可能没收尾，首笔写会失败且无报错 —— 方向寄存器写不进去，
           后面的复位脉冲全变成 no-op，屏继承 GameBox 脏寄存器 → 反色。
           每笔写加重试。 */
        for (int i = 0; i < 3; i++) {
            WriteReg(0x01, 0x03);
            WriteReg(0x03, 0xf8);
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    void SetOutputState(uint8_t bit, uint8_t level) {
        uint8_t data = ReadReg(0x01);
        data = (data & ~(1 << bit)) | (level << bit);
        WriteReg(0x01, data);
    }
};

class CustomAudioCodec : public BoxAudioCodec {
private:
    Pca9557* pca9557_;

public:
    CustomAudioCodec(i2c_master_bus_handle_t i2c_bus, Pca9557* pca9557) 
        : BoxAudioCodec(i2c_bus, 
                       AUDIO_INPUT_SAMPLE_RATE, 
                       AUDIO_OUTPUT_SAMPLE_RATE,
                       AUDIO_I2S_GPIO_MCLK, 
                       AUDIO_I2S_GPIO_BCLK, 
                       AUDIO_I2S_GPIO_WS, 
                       AUDIO_I2S_GPIO_DOUT, 
                       AUDIO_I2S_GPIO_DIN,
                       GPIO_NUM_NC, 
                       AUDIO_CODEC_ES8311_ADDR, 
                       AUDIO_CODEC_ES7210_ADDR, 
                       AUDIO_INPUT_REFERENCE),
          pca9557_(pca9557) {
    }

    virtual void EnableOutput(bool enable) override {
        BoxAudioCodec::EnableOutput(enable);
        if (enable) {
            pca9557_->SetOutputState(1, 1);
        } else {
            pca9557_->SetOutputState(1, 0);
        }
    }
};

/**
 * 对时 + 开网页服务。
 *
 * 服务端在 OTA 应答里带 server_time 并用 TZ 表达时区（见 ota.cc），
 * 这里再补一路 SNTP：万一服务端没给时间，待机时钟也不能一直显示 "--:--"。
 * SNTP 写的是 UTC，localtime() 会按 TZ 换算，两边口径一致。
 */
/* 控制页的重试定时器（2026-11-03）
   配网模式下官方配网页先占了 80 端口，我们的控制页在开机 12 秒去起必然失败；
   等配网成功、官方 httpd_stop 让出端口后，这里每 3 秒试一次，一起来就停。
   否则用户配完网还得手动重启才能进控制页。 */
static esp_timer_handle_t s_web_retry_timer = nullptr;

static void OnWebRetry(void*) {
    if (dafeiyu::StartWebServer()) {
        ESP_LOGI(TAG, "配网页已让出 80 端口，控制页启动成功");
        esp_timer_stop(s_web_retry_timer);
    }
}

static void StartSntpAndWeb() {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_set_sync_interval(6 * 60 * 60 * 1000);  // 6 小时再对一次
    esp_sntp_init();
    ESP_LOGI(TAG, "sntp started");

    if (dafeiyu::StartWebServer()) {
        return;
    }
    if (s_web_retry_timer == nullptr) {
        esp_timer_create_args_t retry_args = {
            .callback = OnWebRetry,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "web_retry",
        };
        if (esp_timer_create(&retry_args, &s_web_retry_timer) != ESP_OK) {
            ESP_LOGE(TAG, "web retry timer create failed");
            return;
        }
    }
    esp_timer_start_periodic(s_web_retry_timer, 3000000);
    ESP_LOGW(TAG, "控制页暂未启动（配网模式占着 80 端口），每 3 秒重试一次");
}

class ZhengchenCamBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t pca9557_handle_;
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    LcdDisplay* display_ = nullptr;
    Pca9557* pca9557_;
    /* ⚠️ 2026-11-11 必须初始化为 nullptr：小車模式走 InitializeCamera() 的
       early-return，不会执行 camera_ = new ...，此前这里是裸指针（板对象在堆上
       由 new 分配，值是不确定的垃圾）。MCP 通了以后服务端每次会话都会下发
       initialize + vision，ParseCapabilities 里 `if (camera) camera->SetExplainUrl()`
       会通过垃圾指针调用虚函数 → vtable 跳飞 → InstrFetchProhibited（唤醒即重启）。 */
    Esp32Camera* camera_ = nullptr;
    Ws2812Led* ws2812_ = nullptr;   // 板载彩灯，见 InitializeLed()
    PowerManager* power_manager_ = new PowerManager(GPIO_NUM_47);
    /* 2026-11-10 小车模式：NVS "car_mode"=1 时启动即进入
       （竖屏 lcd_mode=0 + 跳过相机 + 显示层眼睛接管）。 */
    bool car_mode_ = false;

    /* ── 2026-11-15 待机省电（网页可配，默认 3 分钟）──────────────────
       idle（不在对话）超过 N 分钟就把背光降到最暗；唤醒、按键、打开菜单、
       图传中都会立刻恢复。N 由网页 /api/config?standby=N 写入
       NVS「screen/standby_min」，这里每秒取内存缓存（不读 NVS）。
       0 = 关闭该功能。 */
    static constexpr int kStandbyDimBrightness = 3;   // 1~100，越小越暗
    esp_timer_handle_t standby_timer_ = nullptr;
    int standby_ticks_ = 0;        // 连续 idle 的秒数
    int standby_min_seen_ = -1;    // 上次看到的分钟数（网页改了就重新计时）
    bool standby_dimmed_ = false;

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // Initialize PCA9557
        pca9557_ = new Pca9557(i2c_bus_, 0x19);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_40;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_41;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    /* 结束省电 + 重新计时。按键处理里先调它，保证菜单/唤醒时屏幕是亮的。 */
    void WakeScreenFromStandby() {
        standby_ticks_ = 0;
        if (standby_dimmed_) {
            standby_dimmed_ = false;
            GetBacklight()->RestoreBrightness();
            ESP_LOGI(TAG, "待机省电：唤醒，恢复亮度");
        }
    }

    void StandbyDimTick() {
        auto& app = Application::GetInstance();
        auto* d = static_cast<DafeiyuDisplay*>(GetDisplay());
        /* 活跃条件：不在 idle、菜单/模式选择开着、正在图传 —— 都不许变暗 */
        const bool busy = !app.CanEnterSleepMode() ||
                          (d != nullptr && (d->MenuIsOpen() || d->ModeSelectActive())) ||
                          dafeiyu::CamStreamRunning();

        const int minutes = dafeiyu::StandbyMinutes();
        if (minutes != standby_min_seen_) {
            standby_min_seen_ = minutes;
            standby_ticks_ = 0;
            ESP_LOGI(TAG, "待机省电：%d 分钟（0=关闭）", minutes);
        }
        if (busy) {
            WakeScreenFromStandby();
            return;
        }
        if (minutes <= 0) {                 // 网页关掉了这个功能
            if (standby_dimmed_) {
                WakeScreenFromStandby();
            }
            return;
        }
        if (standby_dimmed_) {
            return;                         // 已经暗了，别每秒重复下发
        }
        if (++standby_ticks_ >= minutes * 60) {
            standby_dimmed_ = true;
            GetBacklight()->SetBrightness(kStandbyDimBrightness);   // 不写 NVS，只是临时变暗
            ESP_LOGW(TAG, "待机 %d 分钟，屏幕降到最暗（亮度 %d）",
                     minutes, kStandbyDimBrightness);
        }
    }

    void InitializeStandbyDim() {
        esp_timer_create_args_t args = {
            .callback = [](void* arg) {
                static_cast<ZhengchenCamBoard*>(arg)->StandbyDimTick();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "standby_dim",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &standby_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "待机省电：定时器创建失败");
            return;
        }
        esp_timer_start_periodic(standby_timer_, 1000000);   // 每秒 tick
    }

    void InitializeButtons() {
        /* 2026-11-15 开机模式选择列表在显示时，所有键先交给它：
           音量± = 上下移动，BOOT = 确认。其余功能一律让位。 */
        auto mode_select_key = [this](DafeiyuDisplay::ModeReq req) -> bool {
            auto* d = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (d != nullptr && d->ModeSelectActive()) {
                d->ModeSelectRequest(req);
                return true;
            }
            return false;
        };

        boot_button_.OnClick([this, mode_select_key]() {
            WakeScreenFromStandby();   // 按任意键先亮屏（可能正处在待机省电）
            auto& app = Application::GetInstance();
            if (mode_select_key(DafeiyuDisplay::kModeConfirm)) {
                return;
            }
            /* 菜单打开时：BOOT = 确认当前项（2026-11-15 已挪到 OnPressDown，
               按下即确认；这里直接返回，否则一次按键会确认两次）。 */
            auto* d = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (d != nullptr && d->MenuIsOpen()) {
                return;
            }
            /* 2026-11-14 用户要求：图传模式中单击 BOOT = 退出图传（等同页面「返回」）。
               排在菜单判断之后 —— 菜单开着时 BOOT 仍然是确认键。
               ⚠️ 必须丢独立任务：CamModeExit → StopCamStreamServer 最坏要等推流任务
               6 秒（/stream 的 handler 得自己退出），直接调会把按键任务连同音量键
               一起卡住，用户会觉得「按键失灵」。 */
            if (dafeiyu::CamStreamRunning()) {
                ESP_LOGW(TAG, "BOOT：退出图传模式");
                xTaskCreate([](void*) {
                    dafeiyu::CamModeExit();
                    vTaskDelete(nullptr);
                }, "cam_exit", 5120, nullptr, 5, nullptr);
                return;
            }
            /* 2026-11-05 控制页二维码浮层显示时：BOOT 单击只关浮层，
               不许顺带 ToggleChatState（扫码还没扫就进对话很难受） */
            if (d != nullptr && d->ControlPageVisible()) {
                d->CloseControlPage();
                return;
            }
            /* 2026-11-06 第二版：时钟/壁纸已是常驻主题且唤醒时会自动让位，
               BOOT 单击恢复成和大肥鱼主题完全一样的语义 = 唤醒/打断对话
               （用户反馈：单击弹菜单会把唤醒按键废掉）。
               想调菜单就长按 BOOT —— 菜单浮层会盖在时钟上面。 */
            // During startup (before connected), pressing BOOT button enters Wi-Fi config mode without reboot
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterConfigMode();
                return;
            }
            s_boot_click_from_idle = (app.GetDeviceState() == kDeviceStateIdle);
            app.ToggleChatState();
        });

        // ← 这里曾经被我改成「双击 = 重启」，那是误读了需求：用户说的是
        //   「按 BOOT 会自动重启」这个 bug，不是要这个功能。已还原。
        boot_button_.OnDoubleClick([this]() {
            /* 菜单打开时双击无意义，别让它在背后切 AEC / 拍照 */
            auto* d = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (d != nullptr && (d->MenuIsOpen() || d->ModeSelectActive())) {
                return;
            }
            Settings settings(FIRST_BOOT_NS, true);
            bool is_first_boot = settings.GetInt(FIRST_BOOT_KEY, 1) != 0;
            if (is_first_boot) {
                ESP_LOGI(TAG, "首次启动，启用双击拍照功能");
                auto camera = GetCamera();
                if (!camera->Capture()) {
                    ESP_LOGE(TAG, "Camera capture failed");
                }
                settings.SetInt(FIRST_BOOT_KEY, 0);
            } else {
                ESP_LOGI(TAG, "非首次启动，双击切 AEC");
                auto& app = Application::GetInstance();
                /* ⚠️ 2026-11-06 修复「时钟下双击 AEC 失效」：双击的第一下单击
                   已经触发了 OnClick 的 ToggleChatState，把设备带进了
                   connecting —— 到这里 state != idle，老代码直接跳过 AEC，
                   表现就是「开关 AEC 功能不见了」。先把第一下打开的对话
                   关回去，再切 AEC。屏幕弹通知反馈结果，切没切一眼可见。 */
                /* 2026-11-15：只有第一下**确实是从待机进入**时才撤销。
                   否则（第一下是退出对话）撤销会把设备又推进对话 ——
                   用户看到的就是"按 BOOT 回待机后自己又进对话"。 */
                if (s_boot_click_from_idle && app.GetDeviceState() != kDeviceStateIdle) {
                    app.ToggleChatState();   // 撤销第一下单击打开的对话
                }
                /* 2026-11-15：双击 BOOT 不再单独切 AEC —— AEC 与「语音打断」
                   是同一个开关（打断靠 AEC 压掉喇叭回声，AEC 关着喊了没反应）。
                   统一走 SetVoiceInterruptEnabled，网页/菜单/语音工具看到同一状态。 */
                const bool next_interrupt = (app.GetAecMode() == kAecOff);
                app.SetVoiceInterruptEnabled(next_interrupt);
                GetAudioCodec()->SetOutputVolume(60);
                GetDisplay()->ShowNotification(next_interrupt ? "语音打断 开" : "语音打断 关");
            }
        });

        /* 长按 BOOT：开关菜单（2026-11-02 用户要求）。
           例外：设备还在启动阶段（没连上）时长按仍是配网 —— 刷完机第一件事
           就是配网，比菜单更要紧，这个入口不能丢。配网也搬进了菜单里。 */
        boot_button_.OnLongPress([this, mode_select_key]() {
            WakeScreenFromStandby();   // 长按开菜单前先亮屏，否则菜单在暗屏上看不清
            if (mode_select_key(DafeiyuDisplay::kModeConfirm)) {
                return;
            }
            auto* d = static_cast<DafeiyuDisplay*>(GetDisplay());
            /* ⚠️ 2026-11-11 顺序铁律：小车模式的退出判断必须排在配网之前。
               之前在 starting 状态下一律进配网 —— 用户重启进小车模式后
               （设备还没连上、状态仍是 starting）长按就弹配网，退不出去。
               小车模式是独立显示模式，长按语义只有「退出」一个，不看设备状态。 */
            if (car_mode_) {
                Settings s("lcd_display", true);
                s.SetInt("car_mode", 0);
                s.SetInt("ui_mode", 0);   /* 2026-11-15 退出小车 = 回主题模式 */
                /* 2026-11-13：还原进小车前方向，不要硬掰横屏 */
                s.SetInt("lcd_mode", s.GetInt("car_prev_lcd", 1) != 0 ? 1 : 0);
                s.Commit();
                ESP_LOGW(TAG, "长按 BOOT：退出小车模式，已落盘，重启生效");
                vTaskDelay(pdMS_TO_TICKS(150));
                esp_restart();
                return;
            }
            if (d == nullptr ||
                Application::GetInstance().GetDeviceState() == kDeviceStateStarting) {
                EnterConfigMode();
                return;
            }
            /* 2026-11-06 时钟已是常驻主题，长按不再「先退出时钟」：
               直接开关菜单（退出时钟 = 菜单里选别的主题）。
               菜单浮层会提到时钟之上，见 ApplyMenuRequest 的 kMenuOpen。 */
            d->MenuRequest(d->MenuIsOpen() ? DafeiyuDisplay::kMenuClose
                                           : DafeiyuDisplay::kMenuOpen);
        });

        /* ── 2026-11-15 菜单更跟手：移动用「按下」而不是「单击」──
           iot_button 的 SINGLE_CLICK 要等 CONFIG_BUTTON_SHORT_PRESS_TIME_MS
           （180ms）的双击判定窗口才发，菜单里每翻一格都像卡了一下。
           按下事件只需消抖（10ms）就触发，配合显示层把 tick 从 120ms 提到
           30ms，整体从 ~300ms 压到 ~40ms。
           ⚠️ 单击/长按里的菜单分支必须直接 return，否则一次按键走两格。 */
        volume_up_button_.OnPressDown([this]() {
            auto* md = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (md != nullptr && md->MenuIsOpen()) {
                WakeScreenFromStandby();
                md->MenuRequest(DafeiyuDisplay::kMenuUp);
            }
        });
        volume_down_button_.OnPressDown([this]() {
            auto* md = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (md != nullptr && md->MenuIsOpen()) {
                WakeScreenFromStandby();
                md->MenuRequest(DafeiyuDisplay::kMenuDown);
            }
        });
        /* 菜单里的 BOOT 确认同理：按下即确认。菜单没开时按下什么都不做，
           单击/长按语义完全不变。 */
        boot_button_.OnPressDown([this]() {
            auto* d = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (d != nullptr && d->MenuIsOpen()) {
                WakeScreenFromStandby();
                d->MenuRequest(DafeiyuDisplay::kMenuConfirm);
            }
        });

        /* 菜单打开时，音量键变成「上下移动光标」；没开菜单就是原来的调音量。 */
        volume_up_button_.OnClick([this, mode_select_key]() {
            WakeScreenFromStandby();
            if (mode_select_key(DafeiyuDisplay::kModeUp)) {
                return;
            }
            auto* md = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (md != nullptr && md->MenuIsOpen()) {
                /* 已在 OnPressDown 里移动过，不能再动一次 */
                return;
            }
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
            codec->SetOutputVolume(volume);
            
        });

        volume_up_button_.OnLongPress([this, mode_select_key]() {
            WakeScreenFromStandby();
            if (mode_select_key(DafeiyuDisplay::kModeUp)) {
                return;
            }
            auto* md = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (md != nullptr && md->MenuIsOpen()) {
                return;   // 移动已在 OnPressDown 处理，长按不再重复
            }
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this, mode_select_key]() {
            WakeScreenFromStandby();
            if (mode_select_key(DafeiyuDisplay::kModeDown)) {
                return;
            }
            auto* md = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (md != nullptr && md->MenuIsOpen()) {
                return;
            }
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
        });

        volume_down_button_.OnLongPress([this, mode_select_key]() {
            WakeScreenFromStandby();
            if (mode_select_key(DafeiyuDisplay::kModeDown)) {
                return;
            }
            auto* md = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (md != nullptr && md->MenuIsOpen()) {
                return;
            }
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });

        /* 双击音量 +/- 切换主题（2026-11-01 用户要求）。
           单击/长按仍是调音量，不冲突；菜单打开时让位给光标移动。 */
        volume_up_button_.OnDoubleClick([this]() {
            auto* d = static_cast<DafeiyuDisplay*>(GetDisplay());
            /* 字幕模式下屏上只有文字，切主题看不见却会改 NVS 记忆 → 直接忽略 */
            if (d != nullptr && !d->MenuIsOpen() && !d->ModeSelectActive() &&
                d->UiModeId() != DafeiyuDisplay::kUiSubtitle) { d->SwitchTheme(+1); }
        });
        volume_down_button_.OnDoubleClick([this]() {
            auto* d = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (d != nullptr && !d->MenuIsOpen() && !d->ModeSelectActive() &&
                d->UiModeId() != DafeiyuDisplay::kUiSubtitle) { d->SwitchTheme(-1); }
        });

        /* 菜单里的板级动作：配网 / 重启。
           菜单本体在显示层，跑在 LVGL tick 里；这两个动作只有板子会做，
           所以用回调交回来，而不是让显示层去认识 Board 的私有方法。 */
        if (auto* d = static_cast<DafeiyuDisplay*>(GetDisplay())) {
            d->SetMenuActionHandler([this](int item) -> bool {
                if (item == DafeiyuDisplay::kMenuWifi) {
                    ESP_LOGI(TAG, "菜单：进入配网模式");
                    EnterConfigMode();
                    return true;
                }
                /* 2026-11-10 小车模式：进=竖屏+car_mode=1，退=回横屏。
                   两套分辨率切换必须重建 LVGL，最稳是写 NVS 后重启
                   （与菜单「重启」同款流程，语音断几秒可接受）。 */
                if (item == DafeiyuDisplay::kMenuCar) {
                    Settings s("lcd_display", true);
                    const bool entering = !car_mode_;
                    if (entering) {
                        /* 记住进小车前的方向，退出时还原（2026-11-13） */
                        s.SetInt("car_prev_lcd", s.GetInt("lcd_mode", 1) != 0 ? 1 : 0);
                    }
                    s.SetInt("car_mode", entering ? 1 : 0);
                    /* 2026-11-15：ui_mode 必须同步写，否则重启后显示初始化
                       以 ui_mode 为准，会把这个 car_mode 覆盖掉（小车模式失效） */
                    s.SetInt("ui_mode", entering ? 1 : 0);
                    s.SetInt("lcd_mode", entering ? 0
                             : (s.GetInt("car_prev_lcd", 1) != 0 ? 1 : 0));
                    /* ⚠️ 2026-11-10 关键修复：SetInt 只是标脏，nvs_commit 在
                       析构里；不等析构就 esp_restart → 写入丢失 → 永远退不出
                       小车模式（用户报"菜单切不回去"的真因）。 */
                    s.Commit();
                    ESP_LOGW(TAG, "菜单：%s小车模式（竖屏=%d），已落盘，重启生效",
                             entering ? "进入" : "退出", entering ? 1 : 0);
                    vTaskDelay(pdMS_TO_TICKS(150));
                    esp_restart();
                    return true;
                }
                /* 2026-11-14 横竖屏切换：两套分辨率必须重建 LVGL 和显示对象，
                   运行期切不了 —— 写 NVS 后重启（与小车模式同款流程）。
                   小车模式固定竖屏，显示层已在 MenuConfirm 里拦掉该项。 */
                if (item == DafeiyuDisplay::kMenuOrient) {
                    Settings s("lcd_display", true);
                    const bool land = s.GetInt("lcd_mode", 1) != 0;
                    s.SetInt("lcd_mode", land ? 0 : 1);
                    /* ⚠️ 必须 Commit 再重启：SetInt 只标脏，析构才提交，
                       不等落盘就 esp_restart 会丢写入（小车模式踩过）。 */
                    s.Commit();
                    ESP_LOGW(TAG, "菜单：切到%s，已落盘，重启生效",
                             land ? "竖屏" : "横屏");
                    vTaskDelay(pdMS_TO_TICKS(150));
                    esp_restart();
                    return true;
                }
                if (item == DafeiyuDisplay::kMenuReboot) {
                    ESP_LOGW(TAG, "菜单：重启设备");
                    vTaskDelay(pdMS_TO_TICKS(150));   // 先让日志和画面落地再重启
                    esp_restart();
                    return true;
                }
                return false;
            });
        }
    }

    /* 进配网模式：先停控制页、停重试定时器，把 80 端口让给官方配网页，
       否则官方配网页 httpd_start 失败，用户就配不了网（表现为打不开 192.168.4.1 的页面）。 */
    void EnterConfigMode() {
        dafeiyu::StopWebServer();
        /* 图传（81 口）也一起停：配网时内存要给官方配网页 + AP 栈让路，
           16KB 的流任务栈在这里很值钱。配网成功后 StartWebServer 会重新拉起。 */
        dafeiyu::StopCamStreamServer();
        if (s_web_retry_timer != nullptr) {
            esp_timer_stop(s_web_retry_timer);
        }
        EnterWifiConfigMode();
    }

    // 网页控制页（养成玩法 PetGame 已于 2026-11-01 删除）
    void InitializePetExtras() {
        // dafeiyu::PetGame::GetInstance().Start();  ← 已删除，别再调
        // 等网络栈起来再开 HTTP 服务 + 对时（构造阶段 esp_netif 还没初始化完）
        static esp_timer_handle_t timer = nullptr;
        esp_timer_create_args_t args = {
            .callback = [](void*) { StartSntpAndWeb(); },
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "pet_web",
        };
        if (esp_timer_create(&args, &timer) == ESP_OK) {
            esp_timer_start_once(timer, 12000000);
        }
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_NC;
        io_config.dc_gpio_num = GPIO_NUM_39;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 16;
        // 颜色模式（反色/红蓝互换）存 NVS，网页可实时改
        Settings lcd_settings("lcd_display", true);
        /* 2026-11-10 用户定案：颜色参数修死，不读 NVS —— 网页改出来的错值
           一旦进 NVS 每次启动都生效，就是红蓝反色反复出现的帮凶。
           参数 = 11-08 真机定案（SWRESET+硬复位 → 面板恒回 POR 态，
           POR 正确参数 = invert=1 + BGR=1），配下方 3s/8s 双重补发兜底。 */
        s_lcd_invert = kDefaultLcdInvert;
        s_lcd_rgb_swap = kDefaultLcdRgbSwap;
        panel_config.rgb_ele_order = s_lcd_rgb_swap ? LCD_RGB_ELEMENT_ORDER_BGR
                                                    : LCD_RGB_ELEMENT_ORDER_RGB;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        s_lcd_panel = panel;
        s_lcd_panel_io = panel_io;

        esp_lcd_panel_reset(panel);            // 官方路径：NC 时发 SWRESET+20ms
        /* v6: SWRESET 放脉冲前(面板稳定态必达)+等200ms清完寄存器。
           v5教训: 脉冲后再发SWRESET=面板异常态=黑屏。 */
        esp_lcd_panel_io_tx_param(panel_io, 0x01, NULL, 0);   // SWRESET
        vTaskDelay(pdMS_TO_TICKS(200));
        /* ⚠️ 2026-11-04 双固件反色修复：屏的复位线接在 PCA9557 pin0（高=复位），
           ESP 重启根本不复位屏幕。GameBox 的 TFT_eSPI 会全量改写伽马/VCOM 等
           寄存器，而小智的 esp_lcd 初始化不重写它们 —— 从游戏机热切回小智后
           就会继承脏寄存器，表现为颜色发反。这里先做一次真正的硬复位脉冲，
           让面板回到 POR 状态再初始化。 */
        pca9557_->SetOutputState(0, 1);        // 拉复位
        vTaskDelay(pdMS_TO_TICKS(20));
        pca9557_->SetOutputState(0, 0);        // 释放
        vTaskDelay(pdMS_TO_TICKS(150));        // ST7789 复位后需等 >120ms 再发命令

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, s_lcd_invert != 0);
        /* ⚠️ 2026-11-08 黑屏修复：ST7789 上电/复位后处于「显示关闭」态，
           esp_lcd 的 panel_init 只发 SLPOUT 不发 DISPON —— 之前一直靠开机
           菜单里的 disp_on_off 点屏，菜单移交 GameBox 后没人点了 → 全黑。
           在这里显式点亮，LVGL 第一帧一到就直接可见。 */
        esp_lcd_panel_disp_on_off(panel, true);

        bool is_landscape = lcd_settings.GetInt("lcd_mode", 1) != 0;
        /* 2026-11-10 小车模式标志（菜单进入时与 lcd_mode=0 一起写入） */
        car_mode_ = lcd_settings.GetInt("car_mode", 0) != 0;
        /* 2026-11-15 三模式：0=主题 1=小车 2=字幕（开机选择列表写入）。
           老 NVS 没有 ui_mode，按 car_mode 推导；两者不一致时以 ui_mode 为准。 */
        const int ui_mode = lcd_settings.GetInt("ui_mode", car_mode_ ? 1 : 0);
        if (ui_mode == 1) {
            car_mode_ = true;
        } else if (ui_mode == 0 || ui_mode == 2) {
            car_mode_ = false;
        }
        /* 2026-11-11 小车模式固定竖屏 240x320：表情引擎在后端做 0.75 缩放适配。
           NVS 里 lcd_mode 可能是旧值（横屏实验版写过 1），这里强制纠正。
           字幕模式横竖屏都支持，直接跟 lcd_mode 走。 */
        if (car_mode_) is_landscape = false;
        ESP_LOGI(TAG, "界面模式: %s（ui_mode=%d, lcd_mode=%d → 实际%s）",
                 car_mode_ ? "小车" : (ui_mode == 2 ? "字幕" : "主题"), ui_mode,
                 lcd_settings.GetInt("lcd_mode", 1), is_landscape ? "横屏" : "竖屏");

        /* 方向先设好（菜单是横屏 320x240 位图，坐标依赖 swap_xy 生效）。 */
        if(is_landscape) {
            esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
            esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        } else {
            esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY_1);
            esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X_1, DISPLAY_MIRROR_Y_1);
        }

        /* ⚠️ 2026-11-04 顺序铁律：开机菜单必须在 new DafeiyuDisplay()（LVGL 初始化）
           之前跑 —— LVGL 任务一旦启动就会自己刷屏，菜单线程再直接画 panel 会和它
           抢 SPI 总线，实测直接卡死在菜单里。 */
        LcdApplyMadctl();
        /* 2026-11-08 升级为双重补发（3 秒 + 8 秒，invert+MADCTL 全套）：
           替代 11-06 只补 MADCTL 的单保险 —— 反色从 GameBox 热切复发，
           根因是热重启时总线残留吞命令，补发两次彻底兜住。 */
        LcdScheduleColorReapply();
        /* 2026-11-08 菜单职责移交 GameBox（独立菜单）：小智开机直接启动，
           不再显示固件选择列表。
           ⚠️ 黑屏第二环：菜单原来还有 SetBrightness(80) 先开背光 ——
           删菜单后 ctor 末尾才 RestoreBrightness，中间无人开灯。
           这里显式点亮背光（RestoreBrightness 随后仍会按用户设置覆盖）。 */
        GetBacklight()->SetBrightness(80);

        if(is_landscape) {
            // 横屏模式
            display_ = new DafeiyuDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

        } else {
            // 竖屏模式
            esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY_1);
            esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X_1, DISPLAY_MIRROR_Y_1);
            display_ = new DafeiyuDisplay(panel_io, panel,
                                        DISPLAY_WIDTH_1, DISPLAY_HEIGHT_1, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X_1, DISPLAY_MIRROR_Y_1, DISPLAY_SWAP_XY_1);
        }

        /* 2026-11-10 小车模式：启动即让全屏眼睛接管（守卫在显示层 OnTick）。 */
        if (car_mode_) {
            static_cast<DafeiyuDisplay*>(display_)->EnterCarMode();
        }

        /* 2026-11-12：ESP-NOW 改成**无条件 + 失败重试**（原来只在车模式、单次 10 秒）。
           两处坑一起修：
             ① 主机的「从机连接状态 + 实时距离」全靠从机 5Hz 遥测，而接收回调
                只有 StartLocked() 里才注册。不在车模式时主机从没调过 Start
                → 遥测全丢 → 主页永远「从机未连接」。
             ② 单次 10 秒启动太赌：WiFi 有时 13 秒才 ready，esp_now_init 会失败，
                之后就再也没人重试 —— 等于永久离线。
           现在每 2 秒重试一次，成功后自己停表。 */
        {
            static esp_timer_handle_t s_espnow_timer = nullptr;
            const esp_timer_create_args_t args = {
                .callback = [](void*) {
                    /* ⚠️ 必须等 WiFi 真的连上（拿到 AP）再 init。
                       实测：2.8 秒时 WiFi 还在 scanning，esp_now_init 返回 OK、
                       日志还打印「就绪，广播信道=10」，但 4 秒后 esp_wifi_connect
                       建立连接会切信道/重置接口 → ESP-NOW 实际不通，
                       从机永远锁不上，主机永远显示未连接。 */
                    wifi_ap_record_t ap;
                    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
                        return;   /* 还没连上，2 秒后的下一次 tick 再试 */
                    }
                    if (EspNowLink::Get().Start()) {
                        ESP_LOGI(TAG, "ESP-NOW 已启动（WiFi 已连，信道=%u，从机 5 秒内锁定）",
                                 ap.primary);
                        /* 2026-11-14：从机的全局速度只放在 RAM（s_speed_pct，默认 100，
                           不存 NVS）。从机一重启就回到 100%，而网页滑块还记着 30%
                           → 摇杆和装饰动作都比滑块显示的快。主控这边存了值，起来后
                           补发一次，保证「滑块显示 = 从机实际」。 */
                        Settings spd("lcd_display", false);
                        const int saved_pct = spd.GetInt("speed_pct", 0);
                        if (saved_pct >= 20 && saved_pct <= 100) {
                            if (EspNowLink::Get().SendSpeed(saved_pct)) {
                                ESP_LOGI(TAG, "补发全局速度 %d%%", saved_pct);
                            }
                        }
                        if (s_espnow_timer != nullptr) {
                            esp_timer_stop(s_espnow_timer);
                        }
                    } else {
                        ESP_LOGW(TAG, "ESP-NOW 启动失败，2 秒后重试");
                    }
                },
                .arg = nullptr,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "espnow_boot",
                .skip_unhandled_events = true,
            };
            if (esp_timer_create(&args, &s_espnow_timer) == ESP_OK) {
                esp_timer_start_periodic(s_espnow_timer, 2ULL * 1000 * 1000);
            }
        }
    }

    void InitializeCamera() {
        /* 2026-11-11 小车模式也开相机（用户要拍照识别）。
           代价：VGA RGB565 单帧 600KB + 编码/预览缓冲，全在 PSRAM；
           带宽约 9MB/s，和表情 canvas 的 4.6MB/s 叠加远低于八线 PSRAM 上限。
           拍照时会自动把脸藏起来、全屏显示刚拍的画面（见 SetPreviewImage）。 */
        if (car_mode_) {
            ESP_LOGI(TAG, "小车模式：相机照常初始化（拍照时临时隐藏表情）");
        }
        // Open camera power
        pca9557_->SetOutputState(2, 0);

        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_2;  // LEDC通道选择  用于生成XCLK时钟 但是S3不用
        config.ledc_timer = LEDC_TIMER_2; // LEDC timer选择  用于生成XCLK时钟 但是S3不用
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = -1;   // 这里写-1 表示使用已经初始化的I2C接口
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        /* ⚠️ 2026-11-11 小车模式降到 QVGA(320x240)：
           用户要求图传 10fps。S3 没有硬件 JPEG 编码器（那是 P4），RGB565→JPEG
           全靠软件编码，VGA 一帧要 200ms 上下（≈3~5fps），QVGA 才 40~80ms。
           320x240 正好等于 LCD 分辨率，拍照识别也够用。
           fb_count 2 + GRAB_LATEST：图传取最新帧不排长队，也给拍照留一个帧位。 */
        config.frame_size = car_mode_ ? FRAMESIZE_QVGA : FRAMESIZE_VGA;
        config.jpeg_quality = 9;
        config.fb_count = car_mode_ ? 2 : 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = car_mode_ ? CAMERA_GRAB_LATEST : CAMERA_GRAB_WHEN_EMPTY;

        camera_ = new Esp32Camera(config);
    }

	void InitializeController() { InitializeMCPController(); }

    /* ── 板级 MCP 工具（2026-11-02 起）──────────────────────────────
       让服务端的大模型能直接操作这台设备特有的东西：
         · self.pet.set_theme  换界面主题（用户说"换个表情"时）
         · self.led.set        控制板载 WS2812 彩灯（颜色/亮度/效果）
         · self.location.get   读用户位置（答天气/当地时间前先查）
       设备端只做翻译和执行，不参与判断。 */
    void InitializePetTools() {
        auto& mcp = McpServer::GetInstance();

        /* 2026-11-10 小车模式语音开关：用户说「进入/退出小车模式」时调用。
           切换 = 写 NVS（car_mode + lcd_mode）+ 重启生效（与菜单「小车」同款）。 */
        mcp.AddTool("self.car.mode",
            "进入或退出小车模式（竖屏大眼睛界面，用于遥控小车）。"
            "用户说「进入小车模式」「打开小车模式」「去开小车」时 action=enter；"
            "说「退出小车模式」「关闭小车模式」「回到正常模式」「退出小车」时 action=exit。"
            "切换会重启设备，需要几秒，属正常现象。",
            PropertyList({Property("action", kPropertyTypeString)}),
            [this](const PropertyList& properties) -> ReturnValue {
                auto action = properties["action"].value<std::string>();
                bool entering;
                if (action == "enter") {
                    entering = true;
                } else if (action == "exit") {
                    entering = false;
                } else {
                    return std::string("参数无效：请用 enter 或 exit");
                }
                if (entering == car_mode_) {
                    return std::string(entering ? "已经在小车模式里了" : "现在不在小车模式");
                }
                Settings s("lcd_display", true);
                /* 2026-11-13：进小车前记住方向，退出还原。
                   （原来不分 enter/exit 一律写 0 → 退出后还是竖屏；
                     后来改成 0/1 → 又把主题竖屏硬掰成横屏，两个都不对。） */
                if (entering) {
                    s.SetInt("car_prev_lcd", s.GetInt("lcd_mode", 1) != 0 ? 1 : 0);
                }
                s.SetInt("car_mode", entering ? 1 : 0);
                s.SetInt("ui_mode", entering ? 1 : 0);   /* 2026-11-15 与 car_mode 同步 */
                s.SetInt("lcd_mode", entering ? 0
                         : (s.GetInt("car_prev_lcd", 1) != 0 ? 1 : 0));
                /* ⚠️ 2026-11-10：必须显式落盘再重启（同菜单路径的坑）。 */
                s.Commit();
                ESP_LOGW(TAG, "语音：%s小车模式（竖屏=%d），已落盘，重启生效",
                         entering ? "进入" : "退出", entering ? 1 : 0);
                vTaskDelay(pdMS_TO_TICKS(150));
                esp_restart();
                return true;   /* 不会真正返回（已重启） */
            });

        /* ── 2026-11-13 屏幕方向（主题模式横竖屏）──
           ⚠️ 用户说「竖屏模式」时，AI 以前会误以为要进小车模式（小车也是竖的）。
           这里明确：只切主题显示方向，不进小车模式。
           两套分辨率必须重建 LVGL，所以写 NVS 后重启（同小车模式流程）。 */
        mcp.AddTool("self.display.orientation",
            "切换主题界面的屏幕方向：横屏 / 竖屏。只调整显示方向，不会进入小车模式。"
            "用户说「竖屏模式」「横屏模式」「屏幕竖过来」「屏幕横过来」「我要竖屏显示」时使用。"
            "切换会重启设备，几秒后恢复。",
            PropertyList({Property("mode", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                const std::string mode = properties["mode"].value<std::string>();
                Settings s("lcd_display", true);
                if (s.GetInt("car_mode", 0) != 0) {
                    return std::string("现在在小车模式里，小车模式固定竖屏。"
                                       "要先退出小车模式，才能切主题界面的横竖屏");
                }
                if (mode != "landscape" && mode != "portrait") {
                    return std::string("参数无效：请用 landscape 或 portrait");
                }
                const bool want_land = (mode == "landscape");
                const bool cur_land = s.GetInt("lcd_mode", 1) != 0;
                if (want_land == cur_land) {
                    return std::string(want_land ? "现在已经是横屏了" : "现在已经是竖屏了");
                }
                s.SetInt("lcd_mode", want_land ? 1 : 0);
                s.Commit();   /* ⚠️ 必须先落盘再重启，否则写入丢失 */
                ESP_LOGW(TAG, "语音：切到%s，已落盘，重启生效", want_land ? "横屏" : "竖屏");
                vTaskDelay(pdMS_TO_TICKS(150));
                esp_restart();
                return true;   /* 不会真正返回（已重启） */
            });

        /* 2026-11-11 语音开车 v2：动作集照 qebabe-xiaoche-c3（C3MINI）的 self.car.act。
           从机只吃单步 {steer,throttle,ms}，拆段/节奏全在 car_action 里做。
           旧版只有 5 个动作（前进/后退/左/右/停），「转个圈」「扭一扭」「跳个舞」
           「往前挪一点」这类说法模型根本没法表达 —— 这就是"动作理解不够"的根因。 */
        mcp.AddTool("self.car.move",
            "控制小车动作。根据用户说的话选一个 action：\n"
            "位移类：\n"
            "  forward        前进、往前走、往前开（默认 1.5 秒）\n"
            "  backward       后退、倒车、往后退（1.5 秒）\n"
            "  turn_left      左拐、往左转、往左打方向（边走边拐，0.6 秒）\n"
            "  turn_right     右拐、往右转、往右打方向（0.6 秒）\n"
            "  spin_left      原地左转、掉头、往左绕一圈、转个圈（1.5 秒）\n"
            "  spin_right     原地右转、往右绕一圈（1.5 秒）\n"
            "  quick_forward  往前挪一点、冲一下、往前一点（0.3 秒）\n"
            "  quick_backward 往后挪一点、退一点（0.3 秒）\n"
            "  wiggle         扭一扭、左右摇一摇、晃两下（0.8 秒）\n"
            "  dance          跳舞、跳个舞、来一段（默认 1.1 秒）\n"
            "  stop           停、停下、别动、站住、停下别走\n"
            "情绪类（会连带做一个小车体动作配合表情）：\n"
            "  happy / loving / excited / wake_up / sad / angry / surprised /"
            " confused / thinking / listening / speaking\n"
            "用户说「转个圈再回来」「先往前走再左拐」这种组合动作，"
            "改用 self.car.sequence，不要连调本工具。\n"
            "Args:\n"
            "  `action`: 上面任一名字\n"
            "  `duration_ms`: 可选，0 = 用动作默认时长；用户说「走 3 秒」「跳 5 秒」时填 3000 / 5000\n"
            "调用后必须开口说一句很短的口语（8 个字以内、带点情绪），例如："
            "「走喽～」「倒车小心」「转起来咯」「好，停住」。不要复述参数，不要说「正在为您执行」。\n",
            PropertyList({
                Property("action", kPropertyTypeString),
                Property("duration_ms", kPropertyTypeInteger, 0, 0, 10000),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto action = properties["action"].value<std::string>();
                int ms = properties["duration_ms"].value<int>();

                if (action == "stop") {
                    car_action::Stop();
                    ESP_LOGI(TAG, "语音动作: stop");
                    return std::string("{\"success\": true, \"message\": \"车已经停住\", "
                                       "\"reply_hint\": \"用一句很短的口语回应，例如：好，停住\"}");
                }
                if (!car_action::Run(action.c_str(), ms)) {
                    return std::string("{\"success\": false, \"message\": \"不认识这个动作名，"
                                       "请跟用户说一句「这个动作我还不会」\"}");
                }
                ESP_LOGI(TAG, "语音动作: %s dur=%d", action.c_str(), ms);
                std::string out = "{\"success\": true, \"message\": \"动作已开始：";
                out += action;
                out += "\", \"reply_hint\": \"用一句很短的口语回应，带点情绪，不要复述参数\"}";
                return out;
            });

        /* 组合动作：一次下发多步，步与步之间无缝（对应 C3MINI 的 self.motor.sequence）。
           那边实测：连调 4 次单步中间会有缝，"跳舞"看起来一顿一顿。 */
        mcp.AddTool("self.car.sequence",
            "一次下发多步动作，用于组合指令（「转个圈再回来」「先前进再左拐再停」）。"
            "组合动作优先用它，不要连续多次调 self.car.move。\n"
            "Args:\n"
            "  `steps`: 字符串，格式 \"steer,throttle,ms;steer,throttle,ms;...\"\n"
            "    steer    -100..100  正=右转；±100 且 throttle=0 = 原地转圈\n"
            "    throttle -100..100  正=前进\n"
            "    ms       100..5000  单段时长，最多 64 段，总时长建议 ≤15000\n"
            "示例：\n"
            "  原地转一圈：\"100,0,1500\"\n"
            "  绕个弯：\"60,70,1200;0,70,800\"\n"
            "  扭一扭：\"-70,35,200;70,35,200;-70,35,200;70,35,200\"\n"
            "调用后说一句很短的口语回应。\n",
            PropertyList({Property("steps", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                auto steps = properties["steps"].value<std::string>();
                if (!car_action::RunSequence(steps.c_str())) {
                    return std::string("{\"success\": false, \"message\": \"steps 格式不对或超出限制，"
                                       "需要 steer,throttle,ms;... 格式\"}");
                }
                ESP_LOGI(TAG, "语音动作序列: %s", steps.c_str());
                return std::string("{\"success\": true, \"message\": \"组合动作已开始\", "
                                   "\"reply_hint\": \"用一句很短的口语回应\"}");
            });

        mcp.AddTool("self.pet.set_theme",
            "切换屏幕上的角色主题（表情/场景）。"
            "可选：idle(待机) walk(走路) cry(哭) happy(开心) think(思考) "
            "angry(生气) eat(吃) yawn(困) dance(跳舞)。"
            "当用户要你换表情/换场景时调用。",
            PropertyList({Property("theme", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                auto name = properties["theme"].value<std::string>();
                int id = -1;
                if (name == "idle" || name == "待机")        id = PET_THEME_IDLE;
                else if (name == "walk" || name == "走路")   id = PET_THEME_WALK;
                else if (name == "cry"  || name == "哭")     id = PET_THEME_CRY;
                else if (name == "happy"|| name == "开心")   id = PET_THEME_HAPPY;
                else if (name == "think"|| name == "思考")   id = PET_THEME_THINK;
                else if (name == "angry"|| name == "生气")   id = PET_THEME_ANGRY;
                else if (name == "eat"  || name == "吃")     id = PET_THEME_EAT;
                else if (name == "yawn" || name == "困")     id = PET_THEME_YAWN;
                else if (name == "dance"|| name == "跳舞")   id = PET_THEME_DANCE;
                if (id < 0) {
                    throw std::runtime_error("Unknown theme: " + name);
                }
                auto* d = DafeiyuDisplay::Instance();
                if (d == nullptr) {
                    return false;
                }
                d->SetThemeById(id);
                ESP_LOGI(TAG, "MCP: 主题请求 -> %s(%d)", name.c_str(), id);
                return true;
            });

        /* 板载 WS2812 彩灯：语音入口（网页入口在 dafeiyu_web.cc 的 /api/led）
           2026-11-15：合并原 self.led.control 的能力 —— dance 参数控制音乐律动，
           effect=off 时一并关律动（律动开着直接关灯会前功尽弃）。 */
        mcp.AddTool("self.led.set",
            "控制板载 RGB 彩灯：颜色、亮度、效果、音乐律动。"
            "用户说「开灯」「关灯」「把灯变成红色」「亮一点」「跟着音乐律动」时使用。\n"
            "Args:\n"
            "  `color`: 颜色，英文名(red/green/blue/white/yellow/purple/pink/orange/cyan/gold)、"
            "中文名(红/绿/蓝/白/黄/紫/粉/橙)或 #RRGGBB\n"
            "  `brightness`: 亮度 0-100，可省略\n"
            "  `effect`: on(常亮)/off(关)/breathe(呼吸)/blink(闪烁)/rainbow(彩虹)，可省略\n"
            "  `dance`: true=跟着音乐和人声律动，false=关闭律动，可省略",
            PropertyList({
                Property("color", kPropertyTypeString, std::string("#3D5AFE")),
                Property("brightness", kPropertyTypeInteger, 50, 0, 100),
                Property("effect", kPropertyTypeString, std::string("on")),
                Property("dance", kPropertyTypeBoolean, false),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto color = properties["color"].value<std::string>();
                int brightness = properties["brightness"].value<int>();
                auto effect = properties["effect"].value<std::string>();
                const bool dance = properties["dance"].value<bool>();
                if (ws2812_ == nullptr) {
                    throw std::runtime_error("LED not initialized");
                }
                /* 只说「跟着音乐律动」时不要顺手把用户已设的颜色改回默认蓝 */
                if (dance && color == "#3D5AFE" && effect == "on") {
                    ws2812_->Set("", -1, "on");
                    ws2812_->SetDanceMode(true);
                    return true;
                }
                if (!ws2812_->Set(color, brightness, effect)) {
                    throw std::runtime_error("Invalid color or effect");
                }
                if (effect == "off") {
                    ws2812_->SetDanceMode(false);
                } else if (dance) {
                    ws2812_->SetDanceMode(true);
                }
                return true;
            });

        /* 2026-11-05 歌名上报 + 2026-11-06 起/停：设备端音乐是云端 Opus 流、
           拿不到歌名，约定模型点歌后调这个工具把歌名报到屏幕上（同时视为开始）。
           2026-11-15 合并 now / playing 两个工具：参数都可选，不传就不动那一项。
           音乐播完/被打断时必须上报 playing=false，否则屏幕频谱不收。 */
        mcp.AddTool("self.music.info",
            "上报音乐信息：开始播放/换歌时传 title（歌名，可带歌手）；"
            "播完、暂停或被停止时传 playing=false。两者可同时传。",
            PropertyList({
                Property("title", kPropertyTypeString, std::string("")),
                Property("playing", kPropertyTypeBoolean, true),
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto* d = DafeiyuDisplay::Instance();
                if (d == nullptr) {
                    return false;
                }
                const auto title = properties["title"].value<std::string>();
                if (!title.empty()) {
                    d->SetSongTitle(title.c_str());
                }
                d->SetMusicPlaying(properties["playing"].value<bool>());
                return true;
            });

        /* 位置：网页上设置或 IP 自动定位的结果。答天气/当地时间前先查这个 */
        mcp.AddTool("self.location.get",
            "获取用户设备所在的城市位置（用户在设备网页上设置，或通过 IP 自动定位）。"
            "回答天气、当地时间、附近推荐等与地点相关的问题前，先调用这个工具。",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                Settings s("location", false);
                std::string loc = s.GetString("text", "");
                return loc.empty() ? std::string("未设置") : loc;
            });
    }

    /* 板载 WS2812 彩灯（2026-11-03）。
       原来是 SingleLed（GPIO 拉高拉低），那套驱动点不亮 WS2812 ——
       WS2812 是单线时序协议，必须走 RMT。 */
    void InitializeLed() {
        ws2812_ = new Ws2812Led((gpio_num_t)BUILTIN_LED_GPIO, 1);
        SetWs2812Instance(ws2812_);
        /* 2026-11-05 开机恢复律动开关（网页控制页存过的话） */
        if (dafeiyu::GetConfig().led_dance_enabled) {
            ws2812_->SetDanceMode(true);
        }
    }

    /* 2026-11-10 用户定案：删除 GameBox 双固件与开机选择菜单，只跑小智。
       原 ScheduleBootmenuEntry（每次上电回写 otadata 指向 GameBox 菜单）
       已整体移除；flash 上 ota_1/romfs 分区数据也已擦除。 */

public:
    ZhengchenCamBoard() : 
    boot_button_(BOOT_BUTTON_GPIO),
    volume_up_button_(VOLUME_UP_BUTTON_GPIO),
    volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeButtons();
        InitializeCamera();
        InitializeLed();
		InitializeController();
        InitializePetTools();
        GetBacklight()->RestoreBrightness();
        InitializeStandbyDim();   // 待机省电：idle 超时降亮度（网页可配，默认 3 分钟）
        InitializePetExtras();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CustomAudioCodec audio_codec(
            i2c_bus_, 
            pca9557_);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Led* GetLed() override {
        /* WS2812：见 InitializeLed()。返回它只为满足框架的 Led* 接口，
           OnStateChanged 是空的 —— 彩灯完全由用户（语音/网页）控制。 */
        return ws2812_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging)  override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            last_discharging = discharging;
        }
        // ⚠️ 2026-10-31：这里原先是 std::max<...>(GetBatteryLevel(), 20)，
        // 把真实电量强行抬到 20% 以上再上报 —— 用户问「电量准不准」，这当然不准，
        // 电量从 19% 掉到 2% 界面上都纹丝不动地显示 20%。按用户要求去掉下限，
        // 显示真实读数（代价是低电量时会显示很小的数字，但那是实情）。
        level = power_manager_->GetBatteryLevel();
        return true;
    }
    
    virtual bool GetTemperature(float& esp32temp)  override {
        esp32temp = power_manager_->GetTemperature();
        return true;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(ZhengchenCamBoard);
