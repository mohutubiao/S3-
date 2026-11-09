# S3 成品板（征辰 AI camera / zhengchen-cam）

工程目录：`C:\Users\mohut\xiaozhi-s3-board`　板型 `zhengchen-cam`　芯片 ESP32-S3（16MB flash + Octal PSRAM）

## 接手第一件事

**先读本目录的 `S3成品板-工程说明.md`** —— 引脚表、编译/烧录命令、板子现状全在里面。

> 本工程是 `C:\Users\mohut\xiaozhi-s3-robot`（二驴子）的副本，只在它的基础上换了目标板型。
> `参考-二驴子交接说明.md` 与 `main/boards/s3-robot/` 是沿用的原始内容，**不要顺手改**。

## 三条硬约束

- 引脚几乎排满：摄像头 4~9/11/15/16/17/18/21，音频 I2S 12/13/14/38/45 + I2C 1/2，
  屏 SPI 39/40/41/42，按键 0/3/46，LED 48，充电检测 47，电池 ADC=IO10
- 禁区：`26~32`（flash）、`33~37`（Octal PSRAM）、`19/20`（原生 USB CDC，本工程的下载与日志通道）
- **唯一真空闲**：IO43 / IO44（默认为 UART0 console，需要时须把 console 挪到 USB CDC）

## 屏幕是「全屏角色动画」

- 显示层是 `main/boards/zhengchen/cam/dafeiyu_display.{h,cc}`（`DafeiyuDisplay : SpiLcdDisplay`），
  **不是**原生的 `SpiLcdDisplay`：状态栏/字幕全隐藏，整屏一张 GIF
- 13 套动画在 `assets/dafeiyu/dafeiyu_*.gif`（idle/wait/think/speak 循环 + wake/success/error/happy/yawn/eat + dance/hum/pet 一次性），
  由 `_assets_src/make_all_shenshen.py` 整表生成（源：`_assets_src/shenshen/videos/`，106 段 WebM，黑底抠透明），
  经 `scripts/build_default_assets.py --emoji_collection dafeiyu` 打进 assets 分区（7.09MB/8MB）
- 旧素材包 `dsh-dafeiyu-main`（16 段）+ `make_all.py` 保留，可回退
- 三层调度：状态层（SetStatus）→ 情绪层（SetEmotion 映射）→ 待机层（25~45s 随机小动作）
- ⚠️ 素材授权：`assets/pet/` 是 MIT 可用；`legacy/dafeiyu/` 是 restricted，**不要用**

## 协作约定

1. 改代码前先讲清改什么 / 为什么 / 代价，给 A / B / C 选项，**同意后才动手**
2. 一次只改一处
3. 用户用裸铜丝手工焊接 —— 报硬件故障**先怀疑虚焊**
4. 原生 USB CDC **无法用 RTS 复位**，抓完整开机日志要手动拉 EN/RST

## 与 GameBox 的双固件关系（2026-11-05）

- 同一台机器 16MB flash：本工程 @ota_0(0x20000)，GameBox @ota_1(0x620000)，**共用 nvs(0x9000)**；烧 GameBox 不碰本工程
- 本机面板实际是 **BGR**：`main/boards/zhengchen/cam/zhengchen_cam_board.cc` 的
  `kDefaultLcdRgbSwap = 1`（2026-11-05 真机定案，日志 `MADCTL 下发: 0x60 (BGR=0)` 就是反色铁证）
- 该文件里加了 `color_ver` 一次性 NVS 迁移（2026-11-06 升 v3）；GameBox 侧开机也会写同一组值
  （`syncLcdColorPrefsForXiaozhi()`），两边只认 `lcd_display: invert=1 rgb_swap=0 color_ver=3`。
  ⚠️ 11-06 用户网页实测定案：这块 ST7789 要 **RGB 顺序（swap=0）**，11-05 记的
  swap=1 是误判。改颜色默认值必须两边同时改 + 升 color_ver。

## 音乐可视化（2026-11-05 烧录，11-06 修正）

- 11-06 频谱终版（按 WorkBuddy 交接文档 `音乐频谱改造方案.md` 执行，已烧录）：
  `main/orb/spec_fft.c/.h` 真 FFT——512 点 + 对数 16 段（60Hz~10kHz + 高频补偿），
  音频任务只 memcpy 进环形缓冲，FFT 在 LVGL tick 里做（40ms 节流，上次音乐卡顿
  的教训：别把计算放音频路径）。显示：16 根极光渐变柱（青→紫→粉，纵向
  35%→100% 亮度）+ 白峰帽 + 自动增益（峰值阻尼归一化）。挂钩
  `audio_codec.cc OutputData`（**只输出不输入**，麦克风喂入会让待机频谱乱跳）。
  触发走**协议层**（照搬 C3 小车 application.cc：sentence_start 含 `% play_music`
  → SetMusicPlaying(true)，tts stop → false，其后第一条 sentence_start 自动抓
  歌名），**不依赖模型调 MCP 工具**。⚠️ spec_fft.h 必须 `extern "C"` 守卫，
  否则 .cc 引用炸链接。⚠️ FFT 归一化必须除 32768（int16 PCM 换满刻度）——
  交接文档按 ±1.0 浮点设计的，首版没除导致所有柱钳顶格「不律动」（11-06 已修）。
  ⚠️ spec_fft_feed_pcm 的音乐流块可超 512 样本，环形缓冲写入必须按剩余空间
  分块——首版两条路径 memcpy 在 n>512 时越界冲 .bss，放音乐必崩（已修）
- 11-06 翻页时钟模式（菜单第 7 项「时钟」）：全屏 320×240 canvas 像素直绘
  （PSRAM 150KB，`fclock_*` 成员），aurora 皮肤 4 卡 54×72 + 冒号 1Hz 闪 +
  日期行（主题全量字库）；分钟变化时变化卡两段式翻页 320ms、级联 110ms
  （个位先动）；BOOT 单击/长按退出。浮层只隐藏不删除；菜单 8 项行距 24→21。
  参考 `WorkBuddy/2026-11-06-14-12-36/outputs/翻页时钟固件实现方案.md`
- 11-06 起播卡顿优化（用户反馈「刚刚播放时候还是卡顿」）：① FFT 工作区
  懒分配 + 256 次 cosf 建表 + 画布 35KB 铺底全部挪到 `BuildSpectrum()`（开机）；
  ② 点歌后 600ms 内先不出频谱（只有音频任务动）；③ `music_playing_` 时
  LVGL 刷新降到 50ms/20fps（`ApplyRefrPeriod`，由 SetMusicPlaying/SetSongTitle 触发）。
  ⚠️ 优先级不是瓶颈：LVGL 任务 prio 1，opus 解码 prio 2、输出 prio 4（能抢占显示）
- 11-06 续修：① 歌名条放歌中不倒计时（用户反馈「名字显示一会就没了」，
  OnTick 判 `!music_playing_` 才收）；② 歌词恢复——放音乐时 assistant 的
  sentence_start 唱词显示屏底（`SetChatMessage` 里判 `music_playing_`），
  字体用主题全量字库，说话时依旧无文字；歌词/歌名随 `SetMusicPlaying(false)`
  和待机清理一起收
- 初版功能：歌名（屏顶 20 秒，用 assets 全量字库）、彩灯 `music` 律动、
  控制页开关（spec/dance，复用 `/api/config`，**只收 POST**）、菜单「WEB」
  弹 IP 二维码（BOOT 单击只关浮层）
- ⚠️ 11-06 TTS 字幕已撤（用户不想要；且 basic 字库仅 890 字、服务端文本大量
  缺字成方块）。歌名已换全量字库：**assets 分区里本来就带**
  `font_noto_sans_common_20_4.bin`（6845 字 cbin，index.json 已声明 text_font，
  `Assets::Apply` 开机挂进主题），11-06 发现这点后歌名标签直接改用
  `GetTheme()->text_font()`。C3 小车同机制（xiaozhi-car main/assets.cc:241）
- assets 分区格式：**不是 SPIFFS**，是 esp_mmap_assets 自定义表格式：
  头 12B=文件数u32+checksumu32(字节和&0xFFFF)+总长u32，表项 44B
  （name[32]+size+off+w16+h16，CONFIG_MMAP_FILE_NAME_LENGTH=32），每文件
  前缀 'ZZ'。备份在 `C:\Users\mohut\assets_backup.bin`（4.5MB）
- 11-06 修正（用户反馈）：① 菜单项不能叫「控制页」——basic 子集字库没这三个字
  （查 `78__xiaozhi-fonts/charsets/basic.json`），改名「WEB」；② 频谱**只在放音乐**
  （MCP `self.music.now` 隐式置 `music_playing_`，`self.music.playing(false)` 收）
  居中顶替唤醒球；**对话 TTS 永远用圆圈**（用户明确否决了"说话时出频谱"）；
  回待机（SetWakeIndicator(0)）强制清频谱/歌名/字幕浮层；待机彻底
  隐藏——待机时麦克风底噪会让屏底 8 柱常驻（用户看到"很多色块"）；③ 冷启红蓝
  反色保险：MADCTL 开机 3 秒后补发一次（怀疑 ST7789 冷启上电时序慢丢命令）
- 二维码**不用** LVGL 自带 qrcode（`CONFIG_LV_USE_QRCODE` 与 esp_emote_gfx 的
  qrcodegen.c 符号冲突），复用后者 API 画进 LVGL canvas；头文件路径在
  main/CMakeLists 的 INCLUDE_DIRS 里追加（必须在 idf_component_register **之前**）
- 设备 IP 通常 `192.168.1.139`；网页接口在 free sram <20KB 时连不上（已知）
- 构建环境：`$env:IDF_PATH='C:\Espressif\frameworks\esp-idf-v5.5.4'` +
  ESP_ROM_ELF_DIR + xtensa 工具链后 `cmake --build build`；IDF 框架的
  micro-ecc 子模块 HEAD 曾损坏（浅克隆丢对象），已改指到 refs/remotes/origin/master
- 它的网页接口（`/api/config?rgb_swap=1`）在 SRAM <20KB 时连不上，别依赖；硬件入口：
  开机按住「音量-」切游戏机
