#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/ledc.h>

// ===================================================================
//  音频：ES8311 + NS4150B 模块（9 针）
//  模块丝印顺序: SDA SCL MCLK SCLK DOUT LRCK DIN 5V GND
//
//  ⚠️ 方向别搞反：
//     模块 DOUT = codec 的 ADC 输出 -> 接 S3 的 I2S DIN
//     模块 DIN  = codec 的 DAC 输入 -> 接 S3 的 I2S DOUT
// ===================================================================
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_12   // 模块 MCLK
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_11   // 模块 SCLK
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_9    // 模块 LRCK
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_10   // 模块 DOUT -> S3 收
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_8    // 模块 DIN  <- S3 发

// ⚠️ 音频单独占一路 I2C（I2C1），不与屏幕共用：
//    - 屏幕刷新和 codec 配置各走各的 400kHz，互不抢带宽
//    - 任一设备 I2C 挂死不会连累另一路
//    S3 有两个 I2C 控制器，引脚也不缺，没必要省这两根
//    实际接线：SDA->IO14  SCL->IO13
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_14
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_13
#define AUDIO_CODEC_I2C_PORT     I2C_NUM_1
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR

// ⚠️ 模块没引出功放使能脚，NS4150B 常开 -> 不控 PA，音量只能靠 ES8311 数字音量
#define AUDIO_CODEC_PA_PIN       GPIO_NUM_NC

// ===================================================================
//  显示：1.54 寸 TFT 240x240（ST7789，8 针，SPI3 + DMA）
//  模块丝印顺序：1 GND  2 VCC  3 SCL  4 SDA  5 RES  6 DC  7 CS  8 BLK
//    - SCL/SDA 是 SPI 的时钟/数据（模块叫 I2C 的名字，实际是 SPI 4 线）
//    - CS 低有效；RES 低有效复位；BLK 高电平点亮背光
//
//  ⚠️ 引脚分配避开：26~32(flash/PSRAM)、35~37(Octal PSRAM)、19/20(USB)
// ===================================================================
#define DISPLAY_SCL GPIO_NUM_18   // 模块 SCL -> SPI SCK
#define DISPLAY_SDA GPIO_NUM_17   // 模块 SDA -> SPI MOSI
#define DISPLAY_RES GPIO_NUM_4    // 模块 RES -> 硬件复位
#define DISPLAY_DC  GPIO_NUM_15   // 模块 DC  -> 数据/命令选择
#define DISPLAY_CS  GPIO_NUM_16   // 模块 CS  -> 片选（低有效）

#define DISPLAY_SPI_HOST SPI3_HOST

#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
#define DISPLAY_SWAP_XY  false
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0
// 画面方向不对就改这两个（X=左右, Y=上下），SWAP_XY 用来横竖屏切换
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false

// 背光：BLK 高电平点亮 -> 不反相
#define DISPLAY_BACKLIGHT_PIN          GPIO_NUM_7
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

// ===================================================================
//  距离传感器：VL53L0X / VL53L1X（ToF，I2C0）
//  屏幕改 SPI 后不再占 I2C，ToF 单独用一路 I2C0
//  接线：VCC GND SDA->IO1 SCL->IO2（XSHUT 悬空即可，单个传感器不用控）
// ===================================================================
#define TOF_I2C_ADDR        0x29
#define TOF_XSHUT_GPIO      GPIO_NUM_NC   // 单个传感器不需要，悬空/接VCC
#define TOF_I2C_PORT        I2C_NUM_0
#define TOF_I2C_SDA_PIN     GPIO_NUM_1
#define TOF_I2C_SCL_PIN     GPIO_NUM_2

// ===================================================================
//  按键：只有板载 BOOT
// ===================================================================
#define BOOT_BUTTON_GPIO GPIO_NUM_0

// ===================================================================
//  WS2812 彩灯
// ===================================================================
#define STRIP_LIGHT_GPIO     GPIO_NUM_21
#define STRIP_LIGHT_MAX_LEDS 12

// ===================================================================
//  舵机 x2：转头(水平) / 抬头(垂直)
//  占 LEDC_TIMER_0 + CH0/CH1，50Hz
// ===================================================================
#define SERVO_PAN_GPIO   GPIO_NUM_5      // 转头（水平）
#define SERVO_TILT_GPIO  GPIO_NUM_6      // 抬头（垂直）
#define SERVO_PAN_CENTER  90
#define SERVO_TILT_CENTER 90
#define SERVO_MIN_PULSEWIDTH_US 500
#define SERVO_MAX_PULSEWIDTH_US 2500
#define SERVO_TIMER      LEDC_TIMER_0
#define SERVO_FREQ_HZ    50
#define SERVO_TIMER_BITS LEDC_TIMER_14_BIT
#define SERVO_PAN_CHANNEL  LEDC_CHANNEL_0
#define SERVO_TILT_CHANNEL LEDC_CHANNEL_1

// ===================================================================
//  电机 2 轮（TC1508 / L298N 接法，跟 C3 小车一样 4 信号）
//
//  ⚠️ 通道必须避开舵机的 CH0/CH1：LEDC 的通道和定时器是一对一绑定的，
//     同一个通道被两次 ledc_channel_config 绑到不同 timer，后一次会把
//     前一次顶掉 —— 表现是舵机抽搐或电机不转。所以电机用 CH2~CH5。
// ===================================================================
#define MOTOR_LF_GPIO GPIO_NUM_48
#define MOTOR_LB_GPIO GPIO_NUM_47
#define MOTOR_RF_GPIO GPIO_NUM_38
#define MOTOR_RB_GPIO GPIO_NUM_39
#define MOTOR_TIMER      LEDC_TIMER_1
#define MOTOR_FREQ_HZ    5000
#define MOTOR_TIMER_BITS LEDC_TIMER_10_BIT
#define MOTOR_LF_CHANNEL LEDC_CHANNEL_2
#define MOTOR_LB_CHANNEL LEDC_CHANNEL_3
#define MOTOR_RF_CHANNEL LEDC_CHANNEL_4
#define MOTOR_RB_CHANNEL LEDC_CHANNEL_5

#endif // _BOARD_CONFIG_H_
