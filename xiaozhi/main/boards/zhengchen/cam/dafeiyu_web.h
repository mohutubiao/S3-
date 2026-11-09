#ifndef DAFEIYU_WEB_H
#define DAFEIYU_WEB_H

#include <cstdint>

namespace dafeiyu {

/** 界面布局 */
enum class Layout {
    kDesktop = 0,  // 桌面模式：角色全屏居中，时钟只是角落里的一张小卡片
    kInfo = 1,     // 信息模式：时钟居中当主角，角色缩到右下角让位
};

/** 界面配置（NVS 存档，网页可改） */
struct UiConfig {
    uint32_t bg_color = 0x191F2E;  // 屏幕底色
    bool clock_enabled = true;     // 待机时钟
    bool wallpaper_enabled = true; // 用上传的壁纸（没有壁纸时自动回落纯色）
    Layout layout = Layout::kDesktop;
    /* 唤醒动画风格 —— 只影响唤醒界面，不动待机角色：
       0 = 呼吸球（xiaozhi-orb，整数渲染，默认）
       1 = 大眼睛（2026-11-02 已废弃：用户反馈不好看，代码保留但不再创建）
       2 = 图元涟漪/声波条（旧兜底方案）
       注意：加字段会改变 NVS blob 长度，老配置会被判为无效并回默认值。 */
    uint8_t wake_style = 0;
    /* 2026-11-05 音乐可视化三开关（网页控制页可改）。
       加字段同样会破坏旧 blob 长度 —— 代价是老配置回默认值，可接受。 */
    bool spectrum_enabled = true;   // 屏底频谱条（放音时显示）
    bool subtitle_enabled = true;   // TTS 字幕（歌词替代）：模型说的话显示在屏底
    bool led_dance_enabled = false; // 彩灯律动：开 = 灯效强制 music（随声音跳动）
};

/** 配置版本号：任何一项被改动就 +1，显示层据此热更新 */
uint32_t ConfigVersion();
UiConfig GetConfig();
void SaveConfig(const UiConfig& config);

/** 起 HTTP 服务（局域网访问设备 IP 即可），失败返回 false。
    可重复调用：已在运行则直接返回 true。配网模式下 80 端口被官方配网页占着，
    此时返回 false，联网后由 board 定时重试。 */
bool StartWebServer();
/** 停服务、让出 80 端口（进配网模式前必须调，否则官方配网页起不来） */
void StopWebServer();
bool WebServerRunning();

}  // namespace dafeiyu

#endif  // DAFEIYU_WEB_H
