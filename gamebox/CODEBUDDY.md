# GameBox-NES（ESP32-S3 游戏机 + ESP32-C3 手柄）

工程目录：`C:\Users\mohut\GameBox-NES-OpenSource`
相关工程：`C:\Users\mohut\xiaozhi-s3-board`（小智固件，同一台机器双固件共存）

## 双固件布局（partitions/v2/16m.csv，共用同一块 16MB flash）

| 分区 | 偏移 | 内容 |
|---|---|---|
| ota_0 | 0x20000 | 小智固件（xiaozhi-s3-board 工程） |
| ota_1 | 0x620000 | GameBox（本工程） |
| romfs | 0x820000 | NES 游戏 |
| nvs | 0x9000 | 两套固件共用 |

- 烧 GameBox：`esptool.py --chip esp32s3 -p COM21 write_flash 0x620000 .pio\build\xiaozhi-daifeiyu\firmware.bin`（不动小智）
- 手柄固件：`platformio run -e esp32c3-gamepad -t upload --upload-port COM24`
- 切固件：开机 5 秒内按住「音量-」(GPIO46) ≥0.6 秒 → 游戏机；游戏机设置里「返回小智」

## 屏幕（正辰/大飞鱼 320x240 ST7789）

- 面板实际要 **BGR 顺序**：`src/board_config.h` 的 `DIJI_LCD_RGB_ORDER = false`（2026-11-05 真机确认）
- 调色板已是标准 NES 2C02 RGB565（`src/ppu.cpp`），不要再改
- 小智侧同一结论：`zhengchen_cam_board.cc` 的 `kDefaultLcdRgbSwap = 1`（源码已改，未重烧小智分区）
- GameBox 开机会执行 `syncLcdColorPrefsForXiaozhi()` 把 `lcd_display: invert=1 rgb_swap=1 color_ver=2` 写进共用 NVS，小智读到即正色（不覆盖小智分区）

## 音量（2026-11-05 定案）

- 唯一真音量 = ES8311 芯片音量（0%=真静音），入口在暂停菜单第 4 项「音量」：A 进入，音量+/- 调，A 完成
- 游戏内长按音量+/- = 出暂停菜单（原手势），APU 增益链路已还原，不要再加第二套音量

## 手柄硬件现状（2026-11-05，诊断已完成）

- 引脚：A=0 B=1 X=3 Y=2 SELECT=8(已停用) START=4 上=9 下=21 左=10 右=20
- GPIO8 是板上蓝灯脚，线上限流电阻对地，拆灯无效 → SELECT 已 `-1` 停用（假按键 0x04 已消除）
- GPIO3(X 连发) 同样会持续拉低 → 已 `-1` 停用；Y(GPIO2)=B 连发正常
- 掩码位：A=0x01 B=0x02 SELECT=0x04 START=0x08 上=0x10 下=0x20 左=0x40 右=0x80
- GameBox 里的「选择」由主机端「上+下同按」合成（`src/board_gestures.cpp`），手柄 SELECT 本来就不需要
- strapping 脚：GPIO0(A)/GPIO2/8/9(上)，上电瞬间别按住
- 深睡：空闲 5 分钟；C3 只有 GPIO0-5 能唤醒（A/B/X/Y/START），SELECT/方向键唤不醒是硬件限制

## 待办（2026-11-06）

1. 挪线恢复按键：SELECT → GPIO5，X → GPIO6（都是普通 IO），然后改回
   `-DDIJI_GAMEPAD_SELECT_PIN=5`、`-DDIJI_GAMEPAD_X_PIN=6` 并烧 COM24
2. 或拆掉 GPIO8 上的限流电阻（灯已拆，电阻还在）
3. 验证：空闲掩码 0x00、每个键只点亮对应一位、X/Y 连发正常
