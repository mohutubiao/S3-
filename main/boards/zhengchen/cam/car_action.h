/**
 * 2026-11-11 小车动作调度（移植 qebabe-xiaoche-c3 的 self.car.act / PlaySequence 语义）
 *
 * 背景：C3 从机固件只收单步 {steer, throttle, ms}，多步动作必须在主机（本板）拆段下发
 *       —— 见 C:\Users\mohut\xiaoche-c3-slave\src\main.cpp 头部注释：
 *       "删减：动作序列 …（序列调度放在主机端拆步下发，从机只收单步指令，保持简单）"。
 *
 * ⚠️ 为什么必须有独立任务：MCP 工具回调跑在 Application 主循环里，
 *    在里面 vTaskDelay 等 "跳舞 5 秒" 会把音频采集/唤醒词/屏幕全冻住
 *    （xiaoche-c3 的 compact_wifi_board.cc 踩过这个坑）。
 */
#pragma once

#include <cstdint>

namespace car_action {

/** 跑一个预设动作（名字见 car_action.cc 的 kPresets 表）。
    duration_ms > 0 时覆盖默认时长（"跳舞 5 秒" 靠这个）。
    立即返回，动作在后台任务里执行。
    返回 false = 不认识这个名字（调用方应回一句"没听懂"）。 */
bool Run(const char* name, int duration_ms);

/** 跑自定义序列：steps = "steer,throttle,ms;steer,throttle,ms;..."
    steer/throttle -100..100，ms 100..5000，最多 64 段。立即返回。 */
bool RunSequence(const char* steps);

/** 装饰动作（照 xiaoche-c3 的 QueueMotorAction 那套）：
 *  kind = "wake"(唤醒点头平衡) / "speak_start"(开始说话前冲) /
 *         "speak_end"(说完回撤) / 情绪名(happy/sad/angry/surprised/thinking/confused)
 *  ⚠️ 只在不忙时执行，绝不打断用户正在跑的移动/动作序列（对应 C3 的忙窗口）。
 *     也带节流，避免服务端连发 emotion 时车抖个不停。 */
void Decor(const char* kind);

/** 立即停：取消当前序列 + 发停止包。 */
void Stop();

/** 是否有动作正在跑（用于避免两段动作互相踩）。 */
bool Busy();

}  // namespace car_action
