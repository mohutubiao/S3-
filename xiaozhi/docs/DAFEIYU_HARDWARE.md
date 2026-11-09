# 大肥鱼（小智 S3 相机板）硬件资料与接线图

> 双固件方案：小智 AI + GameBox NES 模拟器共存于一块 16MB flash。
> 游戏盒子与无线手柄固件**修改自「半岛铁盒子」**（GameBox-NES，GPLv3），本板适配与手柄增补为二次开发。

---

## 一、主板：ESP32-S3（16MB flash）

### flash 分区布局（16MB）

| 偏移 | 分区 | 内容 | 大小 |
|---|---|---|---|
| 0x0000 | bootloader | — | — |
| 0x8000 | 分区表 | — | — |
| 0x20000 | **ota_0** | 小智 AI 固件（xiaozhi.bin）| 6MB |
| 0x620000 | **ota_1** | GameBox NES 固件（firmware.bin）| 2MB |
| 0x820000 | **romfs** | 游戏库（LittleFS，内置 .nes）| 3MB |
| — | assets | 唤醒词模型等（勿覆盖）| 其余 |

> ⚠️ 打包游戏库用 3MB 镜像烧 0x820000；仓库旧分区表写 8MB@0x810000 是错的，会覆盖 assets。

### 双固件切换

- 上电进 **GameBox 菜单**（GameBox 固件宿主），选择进小智或留在游戏
- 小智端按住音量键操作回菜单；otadata 记忆上次固件
- 屏幕复位/功放经 PCA9557（两固件共享），初始化序列见各固件源码注释

---

## 二、主板接线总表

### 1. LCD — ST7789V 240×320（SPI3，80MHz，横屏 320×240）

| LCD 引脚 | ESP32-S3 | 备注 |
|---|---|---|
| SCLK | **GPIO41** | |
| MOSI | **GPIO40** | |
| DC | **GPIO39** | |
| CS | — | 固定拉低，不接 GPIO |
| RST | **PCA9557 pin0** | 不接 ESP32！复位线挂 I2C 扩展器（高=复位）|
| BL（背光）| **GPIO42** | **低电平点亮** |

⚠️ 颜色参数（真出厂 POR 态校准）：`MADCTL BGR=1`；GameBox 游戏帧为 raw 直推，发送前需逐像素字节交换（`__builtin_bswap16`），否则红蓝对调 + 通道错位。

### 2. I2C 总线（共享：音频 codec + IO 扩展器）

| 信号 | ESP32-S3 |
|---|---|
| SDA | **GPIO1** |
| SCL | **GPIO2** |

挂载设备：
- **ES8311** 音频 codec @ 0x18
- **PCA9557** IO 扩展器 @ 0x19：
  - pin0 = LCD 复位（高=复位，低=运行）
  - pin1 = 功放使能（高=开）

### 3. 音频 — ES8311 + I2S

| 信号 | ESP32-S3 |
|---|---|
| MCLK | **GPIO38** |
| BCLK | **GPIO14** |
| LRCLK/WS | **GPIO13** |
| DOUT（→codec）| **GPIO45** |
| DIN（←codec 麦）| **GPIO12** |

### 4. 按键与指示

| 功能 | GPIO | 备注 |
|---|---|---|
| BOOT 键 | **GPIO0** | strapping，上电别按住 |
| 音量+ | **GPIO3** | strapping |
| 音量- | **GPIO46** | |
| 电池检测 | **GPIO47** | ADC |
| 板载 LED | **GPIO48** | WS2812 |

### 5. 摄像头 — OV 系列 DVP

| 信号 | GPIO | 信号 | GPIO |
|---|---|---|---|
| XCLK | 17 | D0 | 7 |
| SIOD | 1 | D1 | 4 |
| SIOC | 2 | D2 | 5 |
| VSYNC | 21 | D3 | 6 |
| HREF | 18 | D4 | 8 |
| PCLK | 16 | D5 | 9 |
| | | D6 | 11 |
| | | D7 | 15 |

⚠️ 摄像头与音频共享 I2C（SIOD/SIOC = SDA/SCL = GPIO1/2）。

---

## 三、无线手柄 — ESP32-C3 SuperMini

> 手柄固件修改自半岛铁盒子项目，增补：连发键、双采样消抖、启动引脚自诊断、60Hz/10Hz 自适应功耗模式。
> 通信：ESP-NOW 直连主板（不走路由器），延迟 ~5-10ms；SELECT+START 长按 = 配对。

### 按键接线（每键一脚接 GPIO、一脚共 GND，固件内部上拉）

| 功能 | GPIO | 备注 |
|---|---|---|
| A | **GPIO0** | strapping，上电瞬间别按住 |
| B | **GPIO1** | |
| Y（B 连发）| **GPIO2** | strapping |
| START | **GPIO4** | |
| SELECT | **GPIO5** | |
| X（A 连发）| **GPIO6** | |
| **上** | **GPIO7** | ⚠️ 2026-11-08 从 GPIO9 挪来：GPIO9 是 strapping，按键卡死会导致开机进下载模式 |
| 左 | **GPIO10** | |
| 右 | **GPIO20** | |
| 下 | **GPIO21** | |
| LED / 震动 / 电池 ADC | 未接 | 预留 |

### 排障速查

- 手柄串口统计每 2 秒输出一次（115200），含 `按键=0xXX RAW=0xXX`（GPIO0-7 位图，1=按下）与 `RAW2`（GPIO9/10/20/21 位图）
- 打开 COM 口必须 `DTR=False, RTS=False`，否则 C3 被拉进下载模式（表现为零输出）
- 按键读数异常（悬空读成按下/卡键）优先查：弹片老化 → 线碰 GND → strapping 上电态

---

## 四、烧录速查

| 目标 | 命令要点 |
|---|---|
| 小智 | `esptool write_flash 0x20000 xiaozhi.bin`（OTA 已禁用自动升级）|
| 游戏盒子 | `esptool write_flash 0x620000 firmware.bin`（须 < 2MB）|
| 游戏库 | `pio run -t buildfs` → `esptool write_flash 0x820000 littlefs.bin`（⚠️ 游戏文件名必须纯 ASCII，mklittlefs Windows 下不支持中文）|
| 手柄 | `pio run -e esp32c3-gamepad -t upload` |

---

## 五、开源声明 / 出处

| 模块 | 出处 |
|---|---|
| **小智 AI 固件** | 抖音「**创客久久**」 |
| **游戏盒子固件（GameBox NES）** | 修改自 抖音「**半岛铁盒子**」的开源项目（GPLv3）|
| **ESP-NOW 无线手柄固件** | 修改自 抖音「**半岛铁盒子**」的开源项目（GPLv3）|

- GameBox 与手柄的修改内容：大肥鱼板适配（双固件分区、PCA9557 屏复位链、ESP-NOW 手柄增补、显示管线字节序修正、连发与消抖）。
- GameBox/手柄遵循原项目 **GPLv3** 许可证，修改版同样以 GPLv3 开源。
- 源码仓库：https://github.com/mohutubiao/S3-
