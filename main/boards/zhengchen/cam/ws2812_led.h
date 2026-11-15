#ifndef WS2812_LED_H
#define WS2812_LED_H

#include "led/led.h"

#include <driver/gpio.h>
#include <led_strip.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <mutex>
#include <string>

/** 板载 WS2812 彩灯（语音 self.led.set / 网页 /api/led 都走这里）。

    设计取舍：OnStateChanged() 故意留空 —— 原来的 SingleLed 会跟着设备
    状态闪（连接中/聆听/说话），但用户要的是「语音网页都能控制」，
    彩灯完全交给用户，不然刚设的颜色一下就被状态灯覆盖了。

    线程模型：Set() 可能从 httpd 任务（网页）或主循环（MCP 工具）调，
    效果由一个常驻低优先级任务算，两者都用 mutex_ 保护灯带句柄。 */
class Ws2812Led : public Led {
public:
    Ws2812Led(gpio_num_t gpio, int count);
    ~Ws2812Led() override;

    void OnStateChanged() override {}

    /** 设置颜色 / 亮度 / 效果。
        color: "#RRGGBB"、"RRGGBB"、英文名（red/green/...）、中文名（红/绿/...），
               传空串表示保持当前颜色不变。
        level: 0-100，传 -1 表示保持当前亮度。
        effect: on / off / breathe / blink / rainbow（开/关/呼吸/闪烁/彩虹）。
        返回 false = 颜色或效果名不认识（调用方负责提示）。 */
    bool Set(const std::string& color, int level, const std::string& effect);

    /** 当前状态（JSON 字符串），给网页 /api/led 与 /api/state 用 */
    std::string GetStateJson();

    /* 2026-11-05 彩灯律动开关（网页控制页）：
       on  = 记住当前效果，切到 music（亮度/色相随声音跳）；
       off = 恢复之前的效果。重复调用幂等。 */
    void SetDanceMode(bool on);
    bool DanceModeOn() const { return dance_mode_; }

private:
    led_strip_handle_t strip_ = nullptr;
    int count_;
    std::mutex mutex_;
    TaskHandle_t task_ = nullptr;

    uint8_t r_ = 0x3D, g_ = 0x5A, b_ = 0xFE;   // 默认蓝紫色
    int level_ = 50;                            // 亮度 0-100
    char effect_[16] = "off";
    bool dance_mode_ = false;                   // 律动开关（网页控制页）
    char dance_prev_effect_[16] = "on";         // 开律动前的效果，关掉时恢复

    /** 套上亮度（gamma 2 感知校正）把颜色发到灯带。调用方需已持有 mutex_ */
    void ShowScaled_(uint8_t r, uint8_t g, uint8_t b, float factor);

    static void EffectTask_(void* arg);
};

/** 板子构造时注册实例；网页 / 语音侧跨文件取用（不认识 Board 类） */
void SetWs2812Instance(Ws2812Led* led);
Ws2812Led* Ws2812Instance();

#endif  // WS2812_LED_H
