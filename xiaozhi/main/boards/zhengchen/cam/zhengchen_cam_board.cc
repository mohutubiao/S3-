#include "wifi_board.h"
#include "audio/codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "dafeiyu_display.h"
#include "dafeiyu_web.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"     // 框架 Led 基类经它间接可用（彩灯用 ws2812_led.h）
#include "ws2812_led.h"         // 板载 WS2812 彩灯（语音/网页控制）
#include "config.h"
#include "i2c_device.h"
#include "esp32_camera.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_system.h>          // esp_restart：菜单里的「重启」
#include <esp_ota_ops.h>         // 2026-11-04 双固件：开机切游戏机
#include <esp_partition.h>
#include <esp_heap_caps.h>       // 开机菜单位图要拷进 PSRAM（DMA 内存）
#include <cstring>
// 2026-11-08 菜单宿主移交 GameBox，dafeiyu_bootmenu.h 不再引用（位图保留在仓库备用）
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
constexpr int kDefaultLcdRgbSwap = 1;

static esp_lcd_panel_handle_t s_lcd_panel = nullptr;
static esp_lcd_panel_io_handle_t s_lcd_panel_io = nullptr;
static int s_lcd_invert = kDefaultLcdInvert;
static int s_lcd_rgb_swap = kDefaultLcdRgbSwap;

static void LcdApplyMadctl() {
    /* MADCTL: MY=0x80 MX=0x40 MV=0x20(行列互换) BGR=0x08。
       esp_lcd 的 swap_xy/mirror 只写 MV/MX/MY，BGR 位只在 init 时按
       rgb_ele_order 写一次 —— 运行期改红蓝顺序必须整字节重发。 */
    if (s_lcd_panel_io == nullptr) return;
    bool landscape = Settings("lcd_display", true).GetInt("lcd_mode", 1) != 0;
    bool mx = landscape ? DISPLAY_MIRROR_X : DISPLAY_MIRROR_X_1;
    bool my = landscape ? DISPLAY_MIRROR_Y : DISPLAY_MIRROR_Y_1;
    bool mv = landscape ? DISPLAY_SWAP_XY : DISPLAY_SWAP_XY_1;
    uint8_t madctl = (uint8_t)((my ? 0x80 : 0) | (mx ? 0x40 : 0) |
                               (mv ? 0x20 : 0) | (s_lcd_rgb_swap ? 0x08 : 0));
    esp_lcd_panel_io_tx_param(s_lcd_panel_io, 0x36, (const uint8_t[]){madctl}, 1);
    ESP_LOGI(TAG, "MADCTL 下发: 0x%02X (BGR=%d invert=%d) NVS rgb_swap=%d", madctl,
             (madctl & 0x08) ? 1 : 0, s_lcd_invert, s_lcd_rgb_swap);
}

void DafeiyuSetLcdColorMode(int invert, int rgb_swap) {
    if (s_lcd_panel == nullptr) return;
    s_lcd_invert = invert ? 1 : 0;
    s_lcd_rgb_swap = rgb_swap ? 1 : 0;
    esp_lcd_panel_invert_color(s_lcd_panel, s_lcd_invert != 0);
    LcdApplyMadctl();
    /* ⚠️ Settings 第二个参数是 read_write：必须传 true 才写得进去。
       之前误传 false（只读），SetInt 被静默丢弃，重启就打回默认值。 */
    Settings settings("lcd_display", true);
    settings.SetInt("lcd_invert", s_lcd_invert);
    settings.SetInt("lcd_rgb_swap", s_lcd_rgb_swap);
    ESP_LOGI(TAG, "LCD 颜色模式: invert=%d rgb_swap=%d", s_lcd_invert, s_lcd_rgb_swap);
}

int DafeiyuGetLcdInvert() { return s_lcd_invert; }
int DafeiyuGetLcdRgbSwap() { return s_lcd_rgb_swap; }

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
    Esp32Camera* camera_;
    Ws2812Led* ws2812_ = nullptr;   // 板载彩灯，见 InitializeLed()
    PowerManager* power_manager_ = new PowerManager(GPIO_NUM_47);

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

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            /* 菜单打开时：BOOT = 确认当前项 */
            auto* d = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (d != nullptr && d->MenuIsOpen()) {
                d->MenuRequest(DafeiyuDisplay::kMenuConfirm);
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
            app.ToggleChatState();
        });

        // ← 这里曾经被我改成「双击 = 重启」，那是误读了需求：用户说的是
        //   「按 BOOT 会自动重启」这个 bug，不是要这个功能。已还原。
        boot_button_.OnDoubleClick([this]() {
            /* 菜单打开时双击无意义，别让它在背后切 AEC / 拍照 */
            auto* d = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (d != nullptr && d->MenuIsOpen()) {
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
                if (app.GetDeviceState() != kDeviceStateIdle) {
                    app.ToggleChatState();   // 撤销第一下单击打开的对话
                }
                const auto next = (app.GetAecMode() == kAecOff) ? kAecOnDeviceSide : kAecOff;
                app.SetAecMode(next);
                GetAudioCodec()->SetOutputVolume(60);
                GetDisplay()->ShowNotification(next == kAecOff ? "AEC 关" : "AEC 开");
            }
        });

        /* 长按 BOOT：开关菜单（2026-11-02 用户要求）。
           例外：设备还在启动阶段（没连上）时长按仍是配网 —— 刷完机第一件事
           就是配网，比菜单更要紧，这个入口不能丢。配网也搬进了菜单里。 */
        boot_button_.OnLongPress([this]() {
            auto* d = static_cast<DafeiyuDisplay*>(GetDisplay());
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

        /* 菜单打开时，音量键变成「上下移动光标」；没开菜单就是原来的调音量。 */
        volume_up_button_.OnClick([this]() {
            auto* md = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (md != nullptr && md->MenuIsOpen()) {
                md->MenuRequest(DafeiyuDisplay::kMenuUp);
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

        volume_up_button_.OnLongPress([this]() {
            auto* md = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (md != nullptr && md->MenuIsOpen()) {
                md->MenuRequest(DafeiyuDisplay::kMenuUp);
                return;
            }
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto* md = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (md != nullptr && md->MenuIsOpen()) {
                md->MenuRequest(DafeiyuDisplay::kMenuDown);
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

        volume_down_button_.OnLongPress([this]() {
            auto* md = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (md != nullptr && md->MenuIsOpen()) {
                md->MenuRequest(DafeiyuDisplay::kMenuDown);
                return;
            }
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });

        /* 双击音量 +/- 切换主题（2026-11-01 用户要求）。
           单击/长按仍是调音量，不冲突；菜单打开时让位给光标移动。 */
        volume_up_button_.OnDoubleClick([this]() {
            auto* d = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (d != nullptr && !d->MenuIsOpen()) { d->SwitchTheme(+1); }
        });
        volume_down_button_.OnDoubleClick([this]() {
            auto* d = static_cast<DafeiyuDisplay*>(GetDisplay());
            if (d != nullptr && !d->MenuIsOpen()) { d->SwitchTheme(-1); }
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
        /* 2026-11-06 v3 迁移：v2 误写了 rgb_swap=1（实测红蓝反色），
           升版本号强制覆盖为 v3 正确值。 */
        /* 2026-11-08 v4 迁移：SWRESET 使面板恒回 POR 态，POR 下正确参数是
           invert=1 + rgb_swap=1（BGR=1）。升版本强制覆盖 NVS 旧值。 */
        if (lcd_settings.GetInt("color_ver", 0) != 4) {
            lcd_settings.SetInt("lcd_invert", kDefaultLcdInvert);
            lcd_settings.SetInt("lcd_rgb_swap", kDefaultLcdRgbSwap);
            lcd_settings.SetInt("color_ver", 4);
            ESP_LOGW(TAG, "颜色参数迁移到 v4: invert=%d rgb_swap=%d",
                     kDefaultLcdInvert, kDefaultLcdRgbSwap);
        }
        s_lcd_invert = lcd_settings.GetInt("lcd_invert", kDefaultLcdInvert) != 0 ? 1 : 0;
        s_lcd_rgb_swap = lcd_settings.GetInt("lcd_rgb_swap", kDefaultLcdRgbSwap) != 0 ? 1 : 0;
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
    }

    void InitializeCamera() {
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
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 9;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

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

        /* 板载 WS2812 彩灯：语音入口（网页入口在 dafeiyu_web.cc 的 /api/led） */
        mcp.AddTool("self.led.set",
            "控制板载 RGB 彩灯。\n"
            "Args:\n"
            "  `color`: 颜色，英文名(red/green/blue/white/yellow/purple/pink/orange/cyan/gold)、"
            "中文名(红/绿/蓝/白/黄/紫/粉/橙)或 #RRGGBB\n"
            "  `brightness`: 亮度 0-100，可省略\n"
            "  `effect`: 效果 on(常亮)/off(关)/breathe(呼吸)/blink(闪烁)/rainbow(彩虹)，可省略",
            PropertyList({
                Property("color", kPropertyTypeString, std::string("#3D5AFE")),
                Property("brightness", kPropertyTypeInteger, 50, 0, 100),
                Property("effect", kPropertyTypeString, std::string("on")),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto color = properties["color"].value<std::string>();
                int brightness = properties["brightness"].value<int>();
                auto effect = properties["effect"].value<std::string>();
                if (ws2812_ == nullptr || !ws2812_->Set(color, brightness, effect)) {
                    throw std::runtime_error("Invalid color or effect");
                }
                return true;
            });

        /* 2026-11-05 歌名上报：设备端音乐是云端 Opus 流、拿不到歌名，
           约定模型点歌后主动调这个工具把歌名报到屏幕上（同时视为音乐开始）。 */
        mcp.AddTool("self.music.now",
            "上报当前播放的歌名，显示在设备屏幕顶部。"
            "每当开始播放一首歌（点歌/换歌）时调用一次，参数为歌名（可带歌手）。",
            PropertyList({Property("title", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ReturnValue {
                auto* d = DafeiyuDisplay::Instance();
                if (d == nullptr) {
                    return false;
                }
                d->SetSongTitle(properties["title"].value<std::string>().c_str());
                return true;
            });

        /* 2026-11-06 音乐起/停：音乐放完或被打断时必须上报 false，
           否则屏幕频谱不收。 */
        mcp.AddTool("self.music.playing",
            "上报音乐的播放状态。开始播放（已由 self.music.now 隐式上报）之外，"
            "音乐播完、被暂停或被停止时调用一次，参数 playing=false。",
            PropertyList({Property("playing", kPropertyTypeBoolean)}),
            [](const PropertyList& properties) -> ReturnValue {
                auto* d = DafeiyuDisplay::Instance();
                if (d == nullptr) {
                    return false;
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

    /* 2026-11-08 独立开机菜单方案（用户定案：不要两套菜单，只留一个）：
       菜单唯一宿主 = GameBox 固件（开机必经）。小智启动时把 otadata 回写
       指向 ota_1，下次上电先进 GameBox 的列表菜单 → 选完进目标固件。
       循环：上电 → GameBox 菜单 → 选小智 → 小智跑（回写 otadata→ota_1）
             → 下次上电又是菜单；选游戏盒则 otadata 不变继续跑。
       ⚠️ 防御：回写前校验 ota_1 槽头 magic(0xE9)，GameBox 槽空/坏时跳过，
       避免每次上电都启动进空槽变砖。 */
    void ScheduleBootmenuEntry() {
        /* v7 对照组：按住 BOOT 上电 = 跳过菜单直进小智（不经 GameBox 热切），
           用于分辨反色在寄存器层还是数据层：直进正色 → 寄存器层；直进也反 → 数据层。 */
        gpio_config_t bk = {};
        bk.pin_bit_mask = 1ULL << GPIO_NUM_0;
        bk.mode = GPIO_MODE_INPUT;
        bk.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&bk);
        if (gpio_get_level(GPIO_NUM_0) == 0) {
            ESP_LOGW(TAG, "BOOT 按住上电：跳过菜单，直接进小智（诊断对照组）");
            return;
        }
        const esp_partition_t* game = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
        if (game == nullptr) return;
        uint32_t magic = 0;
        if (esp_partition_read(game, 0, &magic, sizeof(magic)) != ESP_OK ||
            (magic & 0xFF) != 0xE9) {
            ESP_LOGW(TAG, "ota_1 槽空/坏，跳过开机菜单回写");
            return;
        }
        const esp_partition_t* cur = esp_ota_get_running_partition();
        if (cur != nullptr && cur->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) {
            return;   // 自己就是菜单宿主时不用回写
        }
        if (esp_ota_set_boot_partition(game) == ESP_OK) {
            ESP_LOGI(TAG, "下次上电进入开机菜单(GameBox @ota_1)");
        }
    }

public:
    ZhengchenCamBoard() : 
    boot_button_(BOOT_BUTTON_GPIO),
    volume_up_button_(VOLUME_UP_BUTTON_GPIO),
    volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        ScheduleBootmenuEntry();   // 2026-11-08 独立菜单：下次上电先进 GameBox 菜单
        InitializeButtons();
        InitializeCamera();
        InitializeLed();
		InitializeController();
        InitializePetTools();
        GetBacklight()->RestoreBrightness();
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
