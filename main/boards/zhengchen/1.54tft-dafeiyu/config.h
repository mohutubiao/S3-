/**
 * @file config.h
 * @brief 征辰 1.54 寸 TFT（240x240）· 大飞鱼精简版 硬件配置
 *
 * 2026-11-16 新建。硬件来源：COM30 实测（ESP32-S3 rev0.2 / 16MB flash /
 * 8MB PSRAM / MAC 84:c7:bb:77:08:e0），引脚与官方 zhengchen-1.54tft-wifi
 * 完全一致，已用开机日志里的 GPIO 初始化记录核对过。
 *
 * 与 zhengchen/cam（大飞鱼完整版）的差别：
 *   · 屏幕 240x240 方屏（cam 是 320x240 / 竖屏 240x320）
 *   · 音频是 I2S 直连（NoAudioCodecSimplex），无 ES7210/ES8311/PCA9557
 *   · 没有摄像头
 *   · 没有板载 WS2812
 */

#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

/* ==================== 音频 ==================== */
/* 注意：输入 16000 / 输出 24000。服务端下发 24000，输出会被重采样，
   会有轻微失真（官方固件日志里也有这条 warning），先保持与原厂一致。 */
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

/* 麦克风 I2S（单向输入） */
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6

/* 喇叭 I2S（单向输出） */
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16

/* ==================== 按键 ==================== */
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_10
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_39

/* ==================== 屏幕 SPI ==================== */
#define DISPLAY_SDA GPIO_NUM_41   /* MOSI */
#define DISPLAY_SCL GPIO_NUM_42   /* CLK  */
#define DISPLAY_RES GPIO_NUM_45
#define DISPLAY_DC  GPIO_NUM_40
#define DISPLAY_CS  GPIO_NUM_21

#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  240
#define DISPLAY_SWAP_XY  false
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define BACKLIGHT_INVERT false
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0

/* ==================== 背光 ==================== */
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_20
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

/* ==================== 电源 / 电池 ==================== */
/* 与官方 zhengchen-1.54tft-wifi 同 PCB 同引脚（已核对其源码）：
     · 充电检测（CHRG）→ GPIO9，高电平 = 正在充电
     · 电池电压采样   → GPIO8（ADC1_CH7）
     · GPIO2 上电拉高给外设供电
   ⚠️ 2026-11-16 修正（用户报「电量不准 + 音量加失效」）：
     原先照抄 cam 板写成 POWER_ADC_GPIO=GPIO9，且 power_manager.h 里
     硬编码 ADC_CHANNEL_9。ESP32-S3 的 ADC1_CH9 就是 GPIO10 —— 正好是
     音量加键。ADC 一旦初始化，GPIO10 被抢成模拟输入，按键事件永远读
     不到（音量加完全失灵）；同时电量读的是按键脚上的电压，数值当然
     不准。cam 板的电池 ADC 确实在 GPIO10，这块板不是。 */
#define POWER_CHARGE_DETECT_GPIO GPIO_NUM_9
#define POWER_ADC_GPIO           GPIO_NUM_8
#define POWER_HOLD_GPIO          GPIO_NUM_2

/* ==================== 和风天气（2026-11-16 接入）====================
   必须用账号专属 API Host：公共域名 devapi/api.qweather.com 对新凭据
   一律返回 403 Invalid Host。
   改城市只改经纬度两行（经度在前、十进制度）。当前是吉林市。
   接口走 v1 新版（带 astro 日出日落/月相），老 v7 已弃用部分路径。 */
#define WEATHER_API_HOST  "p94ewudxdf.re.qweatherapi.com"
#define WEATHER_API_KEY   "45f2eaef66a54ddfa08998cedc733886"
#define WEATHER_LAT       43.83
#define WEATHER_LON       126.55

#endif // _BOARD_CONFIG_H_
