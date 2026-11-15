#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/circular_strip.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <algorithm>

#define TAG "S3RobotBoard"

// ───────────────────────────────────────────────────────────────────
//  舵机 x2（抬头 / 转头）
//  LEDC_TIMER_0 + CH0/CH1，50Hz
//  ⚠️ CH0/CH1 只能属于一个定时器，电机的 TIMER_1 不能再用这两个通道
// ───────────────────────────────────────────────────────────────────
class ServoController {
public:
    ServoController(gpio_num_t pan_gpio, gpio_num_t tilt_gpio) {
        ledc_timer_config_t timer_cfg = {};
        timer_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
        timer_cfg.duty_resolution = SERVO_TIMER_BITS;
        timer_cfg.timer_num = SERVO_TIMER;
        timer_cfg.freq_hz = SERVO_FREQ_HZ;
        timer_cfg.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

        gpio_num_t pins[2] = { pan_gpio, tilt_gpio };
        ledc_channel_t chs[2] = { SERVO_PAN_CHANNEL, SERVO_TILT_CHANNEL };
        for (int i = 0; i < 2; i++) {
            ledc_channel_config_t ch_cfg = {};
            ch_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
            ch_cfg.channel = chs[i];
            ch_cfg.timer_sel = SERVO_TIMER;
            ch_cfg.intr_type = LEDC_INTR_DISABLE;
            ch_cfg.gpio_num = pins[i];
            ch_cfg.duty = 0;
            ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
        }

        pan_ = SERVO_PAN_CENTER;
        tilt_ = SERVO_TILT_CENTER;
        Apply(0, pan_);
        Apply(1, tilt_);
    }

    // degree: 0~180
    void SetAngle(int index, int degree) {
        degree = std::clamp(degree, 0, 180);
        if (index == 0) {
            pan_ = degree;
        } else {
            tilt_ = degree;
        }
        Apply(index, degree);
    }

    void SetPan(int degree)   { SetAngle(0, degree); }
    void SetTilt(int degree)  { SetAngle(1, degree); }
    int pan() const  { return pan_; }
    int tilt() const { return tilt_; }

private:
    void Apply(int index, int degree) {
        int us = SERVO_MIN_PULSEWIDTH_US +
                 (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US) * degree / 180;
        uint32_t max_duty = (1UL << SERVO_TIMER_BITS) - 1;
        uint32_t duty = (uint32_t)us * max_duty / (1000000 / SERVO_FREQ_HZ);
        ledc_channel_t ch = (index == 0) ? SERVO_PAN_CHANNEL : SERVO_TILT_CHANNEL;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
    }

    int pan_ = SERVO_PAN_CENTER;
    int tilt_ = SERVO_TILT_CENTER;
};

// ───────────────────────────────────────────────────────────────────
//  电机 2 轮（4 信号，跟 C3 小车同一接法）
//  LEDC_TIMER_1 + CH2~CH5，10-bit，5kHz
//
//  speed: -100~100（负=后退）
//  steer: -100~100（负=左转）
// ───────────────────────────────────────────────────────────────────
class Motor2WD {
public:
    Motor2WD(gpio_num_t lf, gpio_num_t lb, gpio_num_t rf, gpio_num_t rb) {
        ledc_timer_config_t timer_cfg = {};
        timer_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
        timer_cfg.duty_resolution = MOTOR_TIMER_BITS;
        timer_cfg.timer_num = MOTOR_TIMER;
        timer_cfg.freq_hz = MOTOR_FREQ_HZ;
        timer_cfg.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

        gpio_num_t pins[4] = { lf, lb, rf, rb };
        ledc_channel_t chs[4] = {
            MOTOR_LF_CHANNEL, MOTOR_LB_CHANNEL, MOTOR_RF_CHANNEL, MOTOR_RB_CHANNEL
        };
        for (int i = 0; i < 4; i++) {
            ledc_channel_config_t ch_cfg = {};
            ch_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
            ch_cfg.channel = chs[i];
            ch_cfg.timer_sel = MOTOR_TIMER;
            ch_cfg.intr_type = LEDC_INTR_DISABLE;
            ch_cfg.gpio_num = pins[i];
            ch_cfg.duty = 0;
            ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
        }
    }

    void Drive(int steer, int speed) {
        int left  = std::clamp(speed + steer, -100, 100);
        int right = std::clamp(speed - steer, -100, 100);
        SetWheel(MOTOR_LF_CHANNEL, MOTOR_LB_CHANNEL, left);
        SetWheel(MOTOR_RF_CHANNEL, MOTOR_RB_CHANNEL, right);
    }

    void Stop() {
        SetWheel(MOTOR_LF_CHANNEL, MOTOR_LB_CHANNEL, 0);
        SetWheel(MOTOR_RF_CHANNEL, MOTOR_RB_CHANNEL, 0);
    }

private:
    // 一路轮子用两个通道：前进打 fwd，后退打 bwd，另一个拉低
    static void SetWheel(ledc_channel_t fwd_ch, ledc_channel_t bwd_ch, int value) {
        uint32_t max_duty = (1UL << MOTOR_TIMER_BITS) - 1;
        uint32_t duty = max_duty * (uint32_t)std::abs(value) / 100;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, fwd_ch, value >= 0 ? duty : 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, fwd_ch);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, bwd_ch, value <= 0 ? duty : 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, bwd_ch);
    }
};

class S3RobotBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_  = nullptr;  // I2C1: ES8311 独占
    bool es8311_present_ = false;                        // 音频模块在不在总线上
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    CircularStrip* strip_ = nullptr;
    ServoController* servo_ = nullptr;
    Motor2WD* motor_ = nullptr;

    // 屏幕已换成 SPI TFT，不再占用 I2C。
    // I2C 现在只剩一路：ES8311 音频 codec
    // （ToF 以后单独挂 I2C0，引脚见 config.h）
    void InitializeI2c() {
        i2c_master_bus_config_t codec_bus_config = {
            .i2c_port = AUDIO_CODEC_I2C_PORT,
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
        ESP_ERROR_CHECK(i2c_new_master_bus(&codec_bus_config, &codec_i2c_bus_));

        // 先探测音频模块在不在。不在就不能去构造 Es8311AudioCodec：
        // 它的 ResetCodec() 里是 ESP_ERROR_CHECK，写失败会 abort，
        // 整机就会陷入"abort->重启->abort"的死循环（屏幕表现为不停闪黑）
        // i2c_master_probe 要的是 7 位地址，ES8311 的 0x30 是 8 位，右移 1 位
        esp_err_t probe_ret = i2c_master_probe(codec_i2c_bus_,
                                               AUDIO_CODEC_ES8311_ADDR >> 1, 100);
        es8311_present_ = (probe_ret == ESP_OK);
        if (es8311_present_) {
            ESP_LOGI(TAG, "ES8311 found on I2C1 (0x18)");
        } else {
            ESP_LOGW(TAG, "ES8311 not found on I2C1, boot without audio");
        }
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SDA;
        buscfg.miso_io_num = GPIO_NUM_NC;      // 只写不读，省一根线
        buscfg.sclk_io_num = DISPLAY_SCL;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        // 一次能 DMA 传完整屏，避免分块拖慢刷新
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeSt7789Display() {
        // RES 低有效：先拉低复位，再拉高进入正常工作
        gpio_config_t res_conf = {
            .pin_bit_mask = (1ULL << DISPLAY_RES),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&res_conf));
        gpio_set_level(DISPLAY_RES, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(DISPLAY_RES, 1);
        vTaskDelay(pdMS_TO_TICKS(10));

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS;    // 低有效
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = 3;
        io_config.pclk_hz = 80 * 1000 * 1000;  // 80MHz，配合 DMA
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install ST7789 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RES;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        // 多数 1.54 寸 ST7789 模块需要反色。若显示出来像底片/发白，把这里改成 false
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, true));
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new SpiLcdDisplay(panel_io_, panel_,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                                     DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        // BOOT 是这台设备唯一的按键
        //   单击 = 开关对话（还没连上网时改成进配网）
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    S3RobotBoard() :
        boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeButtons();
        InitializeSt7789Display();
        GetBacklight()->RestoreBrightness();

        strip_ = new CircularStrip(STRIP_LIGHT_GPIO, STRIP_LIGHT_MAX_LEDS);
        servo_ = new ServoController(SERVO_PAN_GPIO, SERVO_TILT_GPIO);
        motor_ = new Motor2WD(MOTOR_LF_GPIO, MOTOR_LB_GPIO, MOTOR_RF_GPIO, MOTOR_RB_GPIO);
    }

    virtual Led* GetLed() override {
        return strip_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        if (!es8311_present_) {
            // 音频模块没接：用哑 codec 顶上，I2S 参数照实传，接上模块重启即可恢复
            static NoAudioCodecDuplex no_codec(
                AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
            return &no_codec;
        }
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, AUDIO_CODEC_I2C_PORT,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        // BLK 高电平点亮 -> invert = false
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    ServoController& servo() { return *servo_; }
    Motor2WD& motor() { return *motor_; }
};

DECLARE_BOARD(S3RobotBoard);
