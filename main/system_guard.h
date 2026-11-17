#pragma once

/*
 * 2026-11-17 主任务 / LVGL 心跳护栏
 *
 * 背景：实机出现「屏幕不动 + BOOT 无反应 + 叫不醒」，但整机并没死
 * （HTTP 200、天气任务照跑、麦克风 RMS 正常）。根因是 Application 主任务
 * 卡在某个锁上，LVGL 任务跟着卡住；而 IDF 的 task WDT 只盯 idle 任务
 * （CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPUx），主任务卡死永远不报警，
 * 于是「看日志」只能看到它没动静，看不到卡在哪 —— 反复排查全在猜。
 *
 * 这里补两路心跳：
 *   · 主任务：Application::Run() 每轮循环都喂（事件等待改成有超时，空闲也喂）
 *   · LVGL  ：DafeiyuDisplay::OnTick() 每次 tick 都喂
 * 任一超过阈值没喂，就打印全部任务状态（谁在跑、谁被阻塞、栈余量），
 * 然后自动重启回待机 —— 不再需要用户拔电，且第一次就能留下证据。
 */

void SystemGuardInit();
void SystemGuardNotifyMain();
void SystemGuardNotifyLvgl();

/* 供网页 /api 侧查询最近一次看门狗事件（没触发过返回 "none"） */
const char* SystemGuardLastEvent();
