/**
 * 2026-11-11 小车模式表情（第二版）：改用交付的 xiaozhi_face_tft 引擎
 *
 * 来源：C:\Users\mohut\WorkBuddy\2026-11-06-14-12-36\outputs\xiaozhi_face_tft
 *   · xiaozhi_face_tft.h      —— 21 情绪引擎（极光渐变 / 220ms 变形 / 自动眨眼 / 说话嘴型）
 *   · 表情套装TFT接入说明.md  —— 接法（canvas + PSRAM 缓冲，内部 SRAM 零新增）
 * 上一版 RoboEyes 造型（8 情绪、LVGL 对象画）已整体替换。
 *
 * 屏幕：小车模式改成横屏 320x240（与引擎的坐标空间一致），
 *       竖屏 240x320 装不下 320 宽的脸。
 *
 * 待机时钟：空闲时把脸藏起来、显示大号时钟 —— 用户要求「不然不知道什么情况下唤醒」。
 *       一进入聆听/说话/动作，立刻切回脸。
 */
#pragma once

#include <lvgl.h>
#include <cstdint>

#include "xiaozhi_face_tft.h"

class RoboFace {
public:
    RoboFace(lv_obj_t* parent, int screen_w, int screen_h);
    ~RoboFace();

    void SetActive(bool on);
    lv_obj_t* Root() const { return root_; }

    /* 情绪：idx 用 xfacetft::XF_*（0..20）；名字走 xiaozhi 协议那 21 个 */
    void SetEmotion(int idx);
    void SetEmotionByName(const char* str);

    /* 说话：只控制嘴型 + 轻微点头，不动情绪 */
    void SetSpeaking(bool on);

    /* 行驶装饰动作：dir 0 停 / 1 前 / 2 后 / 3 左 / 4 右 / 5 跳舞
       hold_ms 到期自动回到 idle 情绪 */
    void SetDriveAction(int dir, uint32_t hold_ms);
    void ClearDriveAction();
    bool DriveActive() const;

    /** 待机时钟：true = 显示时钟、藏起脸；false = 显示脸 */
    void ShowClock(bool on);
    bool ClockShown() const { return clock_shown_; }

private:
    static void TimerCb(lv_timer_t* t);
    void Tick();
    void ApplyDriveEmotion();
    void UpdateClockText();
    /** 待机时钟下方的 IP / WiFi / 电量（2026-11-12 用户要求）。
        不走每秒刷新：电池要读 ADC、WiFi 要查 RSSI，5 秒一次足够。 */
    void UpdateNetText();

    lv_obj_t* root_ = nullptr;       /* 全屏黑底容器 */
    lv_obj_t* canvas_ = nullptr;     /* 脸：320x240 RGB565，缓冲在 PSRAM */
    uint16_t* buf_ = nullptr;
    xfacetft::Face face_;
    bool active_ = false;

    /* 时钟 */
    lv_obj_t* clock_box_ = nullptr;
    lv_obj_t* clock_time_ = nullptr;
    lv_obj_t* clock_date_ = nullptr;
    lv_obj_t* clock_ip_ = nullptr;    /* 2026-11-12 待机时钟下方：IP */

    /* 2026-11-12：WiFi 三格 + 电池外框/填充（搬自横屏 DafeiyuDisplay 的 sys_bar_）。
       原来用字体符号 LV_SYMBOL_CHARGE 表示充电，用户反馈「闪电看不出电量」，
       所以改成按百分比画填充条，充电只是填充变蓝。 */
    lv_obj_t* net_row_ = nullptr;
    lv_obj_t* battery_outline_ = nullptr;
    lv_obj_t* battery_fill_ = nullptr;
    lv_obj_t* wifi_signal_ = nullptr;
    lv_obj_t* wifi_bars_[3] = {nullptr, nullptr, nullptr};
    /* 2026-11-12：从机连接状态 + 实时距离（不知道距离就没法调阈值） */
    lv_obj_t* clock_car_ = nullptr;
    int net_div_ = 0;                 /* 5 秒节流计数 */
    bool clock_shown_ = false;
    int64_t last_clock_sec_ = -1;

    /* 情绪 / 行驶 overlay */
    int emotion_idx_ = xfacetft::XF_NEUTRAL;
    int idle_emotion_ = xfacetft::XF_NEUTRAL;   /* 动作结束后回到这个 */
    int drive_dir_ = 0;
    uint32_t drive_until_ms_ = 0;

    lv_timer_t* timer_ = nullptr;
    uint32_t t_ms_ = 0;
};
