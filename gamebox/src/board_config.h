#pragma once

#include <stdint.h>

// Every firmware environment must select exactly one board. Keeping the
// original ESP32-S3 hardware explicit prevents a new environment from
// silently inheriting its GPIO map by falling through to a default branch.
#if (defined(DIJI_BOARD_ORIGINAL_ESP32S3) + \
     defined(DIJI_BOARD_LCDWIKI_ES3C28P) + \
     defined(DIJI_BOARD_ZHENGCHEN_DAFEIYU) + \
     defined(DIJI_BOARD_WOKWI_LCDWIKI)) != 1
#error "Select exactly one DIJI board in the PlatformIO build flags"
#endif

#if defined(DIJI_BOARD_WOKWI_LCDWIKI)

#define DIJI_DISABLE_SD 1
#define DIJI_USE_SD_MMC 0
#define DIJI_LCD_DRIVER_ILI9341 1
#define DIJI_LCD_DRIVER_ST7789 0

// Wokwi preflight uses simulator-friendly SPI pins, but skips SDIO.
constexpr int DIJI_LCD_CS_PIN = 5;
constexpr int DIJI_LCD_DC_PIN = 2;
constexpr int DIJI_LCD_RST_PIN = 13;
constexpr int DIJI_LCD_BL_PIN = -1;    // Wokwi ILI9341 backlight is virtual.
constexpr int DIJI_LCD_SCLK_PIN = 14;
constexpr int DIJI_LCD_MOSI_PIN = 12;
constexpr int DIJI_LCD_MISO_PIN = 4;
constexpr int DIJI_LCD_PANEL_WIDTH = 240;
constexpr int DIJI_LCD_PANEL_HEIGHT = 320;
constexpr int DIJI_LCD_FREQ_WRITE = 40000000;
constexpr int DIJI_LCD_FREQ_READ = 6000000;
constexpr bool DIJI_LCD_SPI_3WIRE = false;
constexpr bool DIJI_LCD_INVERT = false;
constexpr bool DIJI_LCD_RGB_ORDER = false;
constexpr uint8_t DIJI_TFT_ROTATION = 3;

constexpr int DIJI_SD_CS_PIN = -1;
constexpr int DIJI_SD_SCLK_PIN = -1;
constexpr int DIJI_SD_MISO_PIN = -1;
constexpr int DIJI_SD_MOSI_PIN = -1;
constexpr uint32_t DIJI_SD_FREQ = 10000000;

constexpr int DIJI_AMP_ENABLE_PIN = -1;
constexpr int DIJI_AMP_ENABLE_LEVEL = 0;
constexpr bool DIJI_AUDIO_CODEC_ES8311 = false;
constexpr int DIJI_AUDIO_CODEC_I2C_SDA_PIN = -1;
constexpr int DIJI_AUDIO_CODEC_I2C_SCL_PIN = -1;
constexpr uint8_t DIJI_AUDIO_CODEC_ES8311_ADDR = 0x18;
constexpr bool DIJI_TOUCH_FT6336 = false;
constexpr int DIJI_TOUCH_I2C_SDA_PIN = -1;
constexpr int DIJI_TOUCH_I2C_SCL_PIN = -1;
constexpr int DIJI_TOUCH_INT_PIN = -1;
constexpr int DIJI_TOUCH_RST_PIN = -1;
constexpr int DIJI_I2S_MCLK_PIN = -1;
constexpr int DIJI_I2S_BCLK_PIN = 5;
constexpr int DIJI_I2S_LRCLK_PIN = 7;
constexpr int DIJI_I2S_DATA_PIN = 6;
constexpr int DIJI_I2S_INPUT_PIN = -1;

constexpr int DIJI_A_BUTTON_PIN = 6;
constexpr int DIJI_B_BUTTON_PIN = -1;
constexpr int DIJI_LEFT_BUTTON_PIN = -1;
constexpr int DIJI_RIGHT_BUTTON_PIN = -1;
constexpr int DIJI_UP_BUTTON_PIN = 15;
constexpr int DIJI_DOWN_BUTTON_PIN = 16;
constexpr int DIJI_START_BUTTON_PIN = 7;
constexpr int DIJI_SELECT_BUTTON_PIN = -1;

#elif defined(DIJI_BOARD_LCDWIKI_ES3C28P)

#define DIJI_DISABLE_SD 0
#define DIJI_USE_SD_MMC 1
#define DIJI_LCD_DRIVER_ILI9341 1
#define DIJI_LCD_DRIVER_ST7789 0

// LCDWIKI ILI9341 LCD SPI
constexpr int DIJI_LCD_CS_PIN = 10;
constexpr int DIJI_LCD_DC_PIN = 46;
constexpr int DIJI_LCD_RST_PIN = -1;   // LCD reset is tied to CHIP_PU on this board.
constexpr int DIJI_LCD_BL_PIN = 45;    // High = backlight on.
constexpr int DIJI_LCD_SCLK_PIN = 12;
constexpr int DIJI_LCD_MOSI_PIN = 11;
constexpr int DIJI_LCD_MISO_PIN = 13;
constexpr int DIJI_LCD_PANEL_WIDTH = 240;
constexpr int DIJI_LCD_PANEL_HEIGHT = 320;
constexpr int DIJI_LCD_FREQ_WRITE = 40000000;
constexpr int DIJI_LCD_FREQ_READ = 6000000;
constexpr bool DIJI_LCD_SPI_3WIRE = false;
constexpr bool DIJI_LCD_INVERT = true;
constexpr bool DIJI_LCD_RGB_ORDER = false;
constexpr uint8_t DIJI_TFT_ROTATION = 3;

// LCDWIKI MicroSD SDIO wiring.
constexpr int DIJI_SDMMC_CLK_PIN = 38;
constexpr int DIJI_SDMMC_CMD_PIN = 40;
constexpr int DIJI_SDMMC_D0_PIN = 39;
constexpr int DIJI_SDMMC_D1_PIN = 41;
constexpr int DIJI_SDMMC_D2_PIN = 48;
constexpr int DIJI_SDMMC_D3_PIN = 47;
constexpr bool DIJI_SDMMC_1BIT_MODE = false;

// LCDWIKI QuanDong ES8311 codec + SC8002 amplifier wiring.
constexpr int DIJI_AMP_ENABLE_PIN = 1;
constexpr int DIJI_AMP_ENABLE_LEVEL = 0; // Low = enabled, high = disabled.
constexpr bool DIJI_AUDIO_CODEC_ES8311 = true;
constexpr int DIJI_AUDIO_CODEC_I2C_SDA_PIN = 16;
constexpr int DIJI_AUDIO_CODEC_I2C_SCL_PIN = 15;
constexpr uint8_t DIJI_AUDIO_CODEC_ES8311_ADDR = 0x18; // 7-bit I2C address.
constexpr bool DIJI_TOUCH_FT6336 = true;
constexpr int DIJI_TOUCH_I2C_SDA_PIN = 16;
constexpr int DIJI_TOUCH_I2C_SCL_PIN = 15;
constexpr int DIJI_TOUCH_INT_PIN = 17;
constexpr int DIJI_TOUCH_RST_PIN = 18;
constexpr int DIJI_I2S_MCLK_PIN = 4;
constexpr int DIJI_I2S_BCLK_PIN = 5;
constexpr int DIJI_I2S_LRCLK_PIN = 7;
constexpr int DIJI_I2S_DATA_PIN = 8;
constexpr int DIJI_I2S_INPUT_PIN = 6;

// Minimal four-button wiring via the exposed GPIO header.
// This is enough for menu navigation and basic game testing.
constexpr int DIJI_A_BUTTON_PIN = 2;
constexpr int DIJI_B_BUTTON_PIN = -1;
constexpr int DIJI_LEFT_BUTTON_PIN = -1;
constexpr int DIJI_RIGHT_BUTTON_PIN = -1;
constexpr int DIJI_UP_BUTTON_PIN = 14;
constexpr int DIJI_DOWN_BUTTON_PIN = 21;
constexpr int DIJI_START_BUTTON_PIN = 3;
constexpr int DIJI_SELECT_BUTTON_PIN = -1;

#elif defined(DIJI_BOARD_ZHENGCHEN_DAFEIYU)

// ── 大肥鱼（小智 S3 相机板）双固件适配（2026-11-04）──
// 与小智固件共享一块 16MB flash：小智在 ota_0，本固件烧在 ota_1，
// romfs（LittleFS 游戏库）是独立分区。屏幕复位/功放挂 PCA9557(0x19)：
//   pin0 = LCD 复位（高电平=复位，低=运行，见 zhengchenLcdResetRelease）
//   pin1 = 功放使能（高=开）
// 本板无 SD 卡、无触摸，游戏操作靠 ESP-NOW 无线手柄。
#define DIJI_DISABLE_SD 1
#define DIJI_USE_SD_MMC 0
#define DIJI_LCD_DRIVER_ILI9341 0
#define DIJI_LCD_DRIVER_ST7789 1

// ST7789 SPI：与小智固件同引脚（80MHz 实测稳定）。
constexpr int DIJI_LCD_CS_PIN = -1;      // 无 CS（固定拉低）
constexpr int DIJI_LCD_DC_PIN = 39;
constexpr int DIJI_LCD_RST_PIN = -1;     // 复位走 PCA9557 pin0，不用 GPIO
constexpr int DIJI_LCD_BL_PIN = 42;      // 背光，低电平点亮（见 device_runtime_impl.inc）
constexpr int DIJI_LCD_SCLK_PIN = 41;
constexpr int DIJI_LCD_MOSI_PIN = 40;
constexpr int DIJI_LCD_MISO_PIN = -1;
constexpr int DIJI_LCD_PANEL_WIDTH = 240;
constexpr int DIJI_LCD_PANEL_HEIGHT = 320;
constexpr int DIJI_LCD_FREQ_WRITE = 80000000;
constexpr int DIJI_LCD_FREQ_READ = 6000000;
constexpr bool DIJI_LCD_SPI_3WIRE = true;
constexpr bool DIJI_LCD_INVERT = true;
// 2026-11-05 红蓝反色修正：本机面板实际要 BGR（MADCTL 的 BGR 位）。
// 之前设 true 时整机所有 16bit 数据都被面板当 BGR 解释 → 红蓝互换；
// 因为菜单配色（0x1082/0x2945/0x2124/0xEF7D）红蓝分量恰好对称，只有青色点缀发黄，
// 一直没看出来。游戏画面用的是纯正 RGB565 调色板，所以一眼就是红变蓝、天变粉。
// 2026-11-08 v4 重定案：TFT_eSPI init 自带 SWRESET，面板每次回真出厂 POR 态，
// 与小智侧同参数：POR 下 rgb_order=true（MADCTL BGR=1）才是正色。
// 2026-11-08 澄清：游戏帧走 pushPixelsDMA（raw 直推无字节序转换）→
// CPU 小端 vs 面板大端 = R/B 对调，面板 BGR=1 恰好抵消。11-05 定案正确，
// 2026-11-08 曾误翻 true，已撤回。
constexpr bool DIJI_LCD_RGB_ORDER = false;  // false = BGR（抵消 raw 帧字节反）
constexpr uint8_t DIJI_TFT_ROTATION = 1;    // 240x320 面板转 320x240 横屏

// 无 SD 卡。
constexpr int DIJI_SD_CS_PIN = -1;
constexpr int DIJI_SD_SCLK_PIN = -1;
constexpr int DIJI_SD_MISO_PIN = -1;
constexpr int DIJI_SD_MOSI_PIN = -1;
constexpr uint32_t DIJI_SD_FREQ = 10000000;

// ES8311 输出 codec（与小智固件同一颗，I2C 与它共用总线）。
constexpr int DIJI_AMP_ENABLE_PIN = -1;     // 功放走 PCA9557 pin1，不用 GPIO
constexpr int DIJI_AMP_ENABLE_LEVEL = 0;
constexpr bool DIJI_AUDIO_CODEC_ES8311 = true;
constexpr int DIJI_AUDIO_CODEC_I2C_SDA_PIN = 1;
constexpr int DIJI_AUDIO_CODEC_I2C_SCL_PIN = 2;
constexpr uint8_t DIJI_AUDIO_CODEC_ES8311_ADDR = 0x18;
constexpr bool DIJI_TOUCH_FT6336 = false;
constexpr int DIJI_TOUCH_I2C_SDA_PIN = -1;
constexpr int DIJI_TOUCH_I2C_SCL_PIN = -1;
constexpr int DIJI_TOUCH_INT_PIN = -1;
constexpr int DIJI_TOUCH_RST_PIN = -1;
constexpr int DIJI_I2S_MCLK_PIN = 38;
constexpr int DIJI_I2S_BCLK_PIN = 14;
constexpr int DIJI_I2S_LRCLK_PIN = 13;
constexpr int DIJI_I2S_DATA_PIN = 45;       // DOUT → ES8311
constexpr int DIJI_I2S_INPUT_PIN = -1;      // NES 不用麦

// 板上只有 3 个实体键：BOOT=GPIO0（A）、音量+=GPIO3（上）、音量-=GPIO46（下）。
// 够进菜单选游戏；打游戏用手柄。
constexpr int DIJI_A_BUTTON_PIN = 0;
constexpr int DIJI_B_BUTTON_PIN = -1;
constexpr int DIJI_LEFT_BUTTON_PIN = -1;
constexpr int DIJI_RIGHT_BUTTON_PIN = -1;
constexpr int DIJI_UP_BUTTON_PIN = 3;
constexpr int DIJI_DOWN_BUTTON_PIN = 46;
constexpr int DIJI_START_BUTTON_PIN = -1;
constexpr int DIJI_SELECT_BUTTON_PIN = -1;

#elif defined(DIJI_BOARD_ORIGINAL_ESP32S3)

#define DIJI_DISABLE_SD 0
#define DIJI_USE_SD_MMC 0
#define DIJI_LCD_DRIVER_ILI9341 0
#define DIJI_LCD_DRIVER_ST7789 1

// Original ST7789 LCD SPI.
constexpr int DIJI_LCD_CS_PIN = 10;
constexpr int DIJI_LCD_DC_PIN = 11;
constexpr int DIJI_LCD_RST_PIN = 12;
constexpr int DIJI_LCD_BL_PIN = -1;
constexpr int DIJI_LCD_SCLK_PIN = 14;
constexpr int DIJI_LCD_MOSI_PIN = 13;
constexpr int DIJI_LCD_MISO_PIN = -1;
constexpr int DIJI_LCD_PANEL_WIDTH = 240;
constexpr int DIJI_LCD_PANEL_HEIGHT = 320;
constexpr int DIJI_LCD_FREQ_WRITE = 80000000;
constexpr int DIJI_LCD_FREQ_READ = 6000000;
constexpr bool DIJI_LCD_SPI_3WIRE = true;
constexpr bool DIJI_LCD_INVERT = true;
constexpr bool DIJI_LCD_RGB_ORDER = false;
constexpr uint8_t DIJI_TFT_ROTATION = 3;

// Original SPI MicroSD wiring.
constexpr int DIJI_SD_CS_PIN = 42;
constexpr int DIJI_SD_SCLK_PIN = 40;
constexpr int DIJI_SD_MISO_PIN = 39;
constexpr int DIJI_SD_MOSI_PIN = 41;
constexpr uint32_t DIJI_SD_FREQ = 10000000;

// Original MAX98357A I2S wiring.
constexpr int DIJI_AMP_ENABLE_PIN = -1;
constexpr int DIJI_AMP_ENABLE_LEVEL = 0;
constexpr bool DIJI_AUDIO_CODEC_ES8311 = false;
constexpr int DIJI_AUDIO_CODEC_I2C_SDA_PIN = -1;
constexpr int DIJI_AUDIO_CODEC_I2C_SCL_PIN = -1;
constexpr uint8_t DIJI_AUDIO_CODEC_ES8311_ADDR = 0x18;
constexpr bool DIJI_TOUCH_FT6336 = false;
constexpr int DIJI_TOUCH_I2C_SDA_PIN = -1;
constexpr int DIJI_TOUCH_I2C_SCL_PIN = -1;
constexpr int DIJI_TOUCH_INT_PIN = -1;
constexpr int DIJI_TOUCH_RST_PIN = -1;
constexpr int DIJI_I2S_MCLK_PIN = -1;
constexpr int DIJI_I2S_BCLK_PIN = 5;
constexpr int DIJI_I2S_LRCLK_PIN = 4;
constexpr int DIJI_I2S_DATA_PIN = 6;
constexpr int DIJI_I2S_INPUT_PIN = -1;

// Original eight-button wiring.
constexpr int DIJI_A_BUTTON_PIN = 48;
constexpr int DIJI_B_BUTTON_PIN = 47;
constexpr int DIJI_LEFT_BUTTON_PIN = 8;
constexpr int DIJI_RIGHT_BUTTON_PIN = 18;
constexpr int DIJI_UP_BUTTON_PIN = 17;
constexpr int DIJI_DOWN_BUTTON_PIN = 3;
constexpr int DIJI_START_BUTTON_PIN = 15;
constexpr int DIJI_SELECT_BUTTON_PIN = 16;

#endif
