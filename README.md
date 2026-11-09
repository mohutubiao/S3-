# 大肥鱼 S3 — 小智 AI + GameBox 游戏盒子 双固件方案

ESP32-S3（16MB flash）单板双固件：**小智 AI 语音助手** + **GameBox NES 游戏盒子**，配 **ESP-NOW 无线手柄**。

## 出处鸣谢 / Credits

| 模块 | 出处 |
|---|---|
| **小智 AI 固件** | 抖音「**创客久久**」 |
| **游戏盒子固件（GameBox NES）** | 修改自 抖音「**半岛铁盒子**」的开源项目（GPLv3）|
| **ESP-NOW 无线手柄固件** | 修改自 抖音「**半岛铁盒子**」的开源项目（GPLv3）|

感谢原作者的开源精神。GameBox/手柄按 GPLv3 继续开源。

## 目录结构

```
S3-/
├── xiaozhi/    小智 AI 固件源码（ESP-IDF，板型 zhengchen-cam）
├── gamebox/    GameBox NES 模拟器 + 无线手柄固件源码（PlatformIO）
│   ├── src/espnow_gamepad.cpp   无线手柄固件（环境 esp32c3-gamepad）
│   └── data/rom/                内置游戏放这里（文件名必须纯 ASCII）
├── release/    打包好的固件 bin（3 个，见烧录说明）
└── DAFEIYU_HARDWARE.md   主板+手柄硬件接线图
```

## 固件烧录（详见 release/烧录说明.txt）

| 包 | 文件 | 烧录地址 |
|---|---|---|
| 小智单独 | release/xiaozhi.bin | 0x20000 |
| **小智+游戏盒子合并版**（推荐）| release/xiaozhi_gamebox_merged.bin | **0x0** 一键全刷 |
| 无线手柄 | release/gamepad.bin | C3 USB 直刷 |

## 本机改动的关键点

- 双固件分区布局：小智 0x20000(6MB) / GameBox 0x620000(2MB) / romfs 游戏库 0x820000(3MB)
- 屏幕复位/功放走 PCA9557(0x19)；SWRESET 时序有讲究（见代码注释）
- 显示链路三层转换（CPU 字节序→MADCTL BGR→面板）已全部定案，游戏帧推送前逐像素字节交换
- 手柄：X/Y 连发、双采样消抖、启动引脚自诊断、上键挪 GPIO7（避开 strapping 脚 GPIO9）

## 许可证

- GameBox/手柄部分遵循原项目 **GPLv3**
- 小智部分遵循其原项目许可证
