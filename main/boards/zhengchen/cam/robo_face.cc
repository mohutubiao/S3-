/**
 * 小车模式表情 v2：xiaozhi_face_tft 引擎 + 待机时钟（2026-11-11）
 * 接入方式照《表情套装TFT接入说明.md》§3：canvas + PSRAM 缓冲，内部 SRAM 零新增。
 */
#include "robo_face.h"

#include "board.h"           // 电量：Board::GetBatteryLevel()
#include "esp_now_link.h"    // 从机连接状态 + 实时距离
#include "wifi_manager.h"    // IP / RSSI / 是否已连

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>   // esp_wifi_sta_get_ap_info()：WiFi 格数按 RSSI 点

#include <cstdio>
#include <cstring>
#include <ctime>

#define TAG "RoboFace"

namespace {
constexpr int kFpsPeriodMs = 33;          /* ~30fps */
constexpr uint32_t kClockColor = 0xFFFFFF;
constexpr uint32_t kClockAccent = 0x26D0CE;   /* 极光青，跟脸的渐变同色系 */

/* 引擎按 320x240 横屏设计，本板小车模式是竖屏 240x320：
   后端做 0.75 缩放（320×0.75 = 240 正好铺满宽）+ 垂直居中。
   ⚠️ 不改交付的 xiaozhi_face_tft.h 本体 —— 它的坐标全写死在 320x240 空间里。 */
constexpr int kSrcW = 320;
constexpr int kSrcH = 240;
constexpr int kDstW = 240;
constexpr int kDstH = 320;
constexpr int kFaceH = kSrcH * 3 / 4;          /* 180 */
constexpr int kFaceY = (kDstH - kFaceH) / 2;   /* 70 */
}  // namespace

/* ── 引擎唯一后端：源坐标 ×0.75 + 垂直居中，写进 240x320 canvas ── */
static void FaceRect(void* user, int x, int y, int w, int h, uint16_t color) {
    auto* self = static_cast<uint16_t*>(user);
    if (self == nullptr || w <= 0 || h <= 0) return;

    int dx = x * 3 / 4;
    int dy = kFaceY + y * 3 / 4;
    int dw = (w * 3 + 3) / 4;   /* 向上取整：宁可多画一列，避免相邻矩形之间露缝 */
    int dh = (h * 3 + 3) / 4;

    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    if (dx + dw > kDstW) dw = kDstW - dx;
    if (dy + dh > kDstH) dh = kDstH - dy;
    if (dw <= 0 || dh <= 0) return;

    for (int yy = 0; yy < dh; ++yy) {
        uint16_t* row = self + (size_t)(dy + yy) * kDstW + dx;
        for (int xx = 0; xx < dw; ++xx) row[xx] = color;
    }
}

RoboFace::RoboFace(lv_obj_t* parent, int screen_w, int screen_h) {
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, screen_w, screen_h);
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);

    /* 脸：240x320 全屏 canvas，缓冲 150KB 放 PSRAM（内部 SRAM 现在只剩 3KB） */
    buf_ = (uint16_t*)heap_caps_calloc((size_t)kDstW * kDstH, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (buf_ != nullptr) {
        canvas_ = lv_canvas_create(root_);
        lv_canvas_set_buffer(canvas_, buf_, kDstW, kDstH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, 0, 0);
        xfacetft::Backend be{};
        be.user = buf_;
        be.rect = &FaceRect;
        face_.begin(be, 0);
    } else {
        ESP_LOGE(TAG, "PSRAM 分配脸缓冲失败（需要 150KB）");
    }

    /* 待机时钟：大号时间 + 日期 */
    clock_box_ = lv_obj_create(root_);
    lv_obj_remove_style_all(clock_box_);
    lv_obj_set_size(clock_box_, screen_w, screen_h);
    lv_obj_set_pos(clock_box_, 0, 0);
    lv_obj_set_style_bg_opa(clock_box_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(clock_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(clock_box_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(clock_box_, LV_OBJ_FLAG_HIDDEN);

    clock_time_ = lv_label_create(clock_box_);
    lv_label_set_text(clock_time_, "--:--");
    lv_obj_set_style_text_font(clock_time_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(clock_time_, lv_color_hex(kClockColor), 0);
    lv_obj_align(clock_time_, LV_ALIGN_CENTER, 0, -10);

    clock_date_ = lv_label_create(clock_box_);
    lv_label_set_text(clock_date_, "");
    lv_obj_set_style_text_color(clock_date_, lv_color_hex(kClockAccent), 0);
    lv_obj_align(clock_date_, LV_ALIGN_CENTER, 0, 45);

    /* 2026-11-12 用户要求：待机时钟上带 IP / WiFi / 电量。
       只放 ASCII —— 中文字库（puhui_14）体积 200KB+，这小屏不值得为几个字塞进去。 */
    clock_ip_ = lv_label_create(clock_box_);
    lv_label_set_text(clock_ip_, "--");
    lv_obj_set_style_text_font(clock_ip_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(clock_ip_, lv_color_hex(kClockColor), 0);
    lv_obj_align(clock_ip_, LV_ALIGN_CENTER, 0, 96);

    /* ── 2026-11-12：WiFi + 电量图标，整体搬自横屏 DafeiyuDisplay 的 sys_bar_ ──
       原来这里是一个 label 打 LV_SYMBOL_WIFI / LV_SYMBOL_CHARGE，充电时只剩一个
       闪电，用户看不出「还剩多少电」。横屏那套是电池空心框 + 按百分比填的实心条，
       信息量一眼可见，所以直接照搬几何与配色：
         电池 22x12、内部 18px 代表 100%，绿=正常 / 蓝=充电 / 红=≤20%
         WiFi  15x11，三根 3px 竖条（高 4/7/10），白=已连接、红=断开 */
    net_row_ = lv_obj_create(clock_box_);
    lv_obj_remove_style_all(net_row_);   /* 不要默认白底/边框，纯当布局容器 */
    lv_obj_set_size(net_row_, LV_SIZE_CONTENT, 12);
    lv_obj_set_flex_flow(net_row_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(net_row_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(net_row_, 8, 0);
    lv_obj_align(net_row_, LV_ALIGN_CENTER, 0, 118);
    lv_obj_clear_flag(net_row_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(net_row_, LV_OBJ_FLAG_CLICKABLE);

    /* 电池：先画外框，填充条是它的子对象 */
    battery_outline_ = lv_obj_create(net_row_);
    lv_obj_set_size(battery_outline_, 22, 12);
    lv_obj_set_style_bg_opa(battery_outline_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(battery_outline_, 1, 0);
    lv_obj_set_style_border_color(battery_outline_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(battery_outline_, 2, 0);
    lv_obj_set_style_pad_all(battery_outline_, 1, 0);
    lv_obj_clear_flag(battery_outline_, LV_OBJ_FLAG_SCROLLABLE);

    battery_fill_ = lv_obj_create(battery_outline_);
    lv_obj_set_height(battery_fill_, 6);
    lv_obj_align(battery_fill_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_width(battery_fill_, 2);
    lv_obj_set_style_bg_color(battery_fill_, lv_color_hex(0x6BE36B), 0);
    lv_obj_set_style_border_width(battery_fill_, 0, 0);
    lv_obj_set_style_radius(battery_fill_, 1, 0);

    /* WiFi：三根由低到高的竖条（手机信号格式），零素材、零字库 */
    wifi_signal_ = lv_obj_create(net_row_);
    lv_obj_set_size(wifi_signal_, 15, 11);
    lv_obj_set_flex_flow(wifi_signal_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_signal_, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_all(wifi_signal_, 0, 0);
    lv_obj_set_style_pad_gap(wifi_signal_, 1, 0);
    lv_obj_set_style_border_width(wifi_signal_, 0, 0);
    lv_obj_set_style_bg_opa(wifi_signal_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(wifi_signal_, LV_OBJ_FLAG_SCROLLABLE);

    static const uint8_t kBarHeights[3] = {4, 7, 10};
    for (int i = 0; i < 3; ++i) {
        wifi_bars_[i] = lv_obj_create(wifi_signal_);
        lv_obj_set_size(wifi_bars_[i], 3, kBarHeights[i]);
        lv_obj_set_style_bg_color(wifi_bars_[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(wifi_bars_[i], LV_OPA_20, 0);   /* 未点亮：暗 */
        lv_obj_set_style_border_width(wifi_bars_[i], 0, 0);
        lv_obj_set_style_radius(wifi_bars_[i], 1, 0);
    }

    /* 从机连接状态 + 实时距离：不知道当前距离就没法判断阈值该设多少 */
    clock_car_ = lv_label_create(clock_box_);
    lv_label_set_text(clock_car_, "CAR --");
    lv_obj_set_style_text_font(clock_car_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(clock_car_, lv_color_hex(kClockAccent), 0);
    lv_obj_align(clock_car_, LV_ALIGN_CENTER, 0, 142);

    timer_ = lv_timer_create(TimerCb, kFpsPeriodMs, this);
    lv_timer_pause(timer_);
}

RoboFace::~RoboFace() {
    if (timer_ != nullptr) lv_timer_delete(timer_);
    if (root_ != nullptr) lv_obj_delete(root_);
    if (buf_ != nullptr) heap_caps_free(buf_);
}

void RoboFace::SetActive(bool on) {
    if (root_ == nullptr) return;
    active_ = on;
    if (on) {
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(root_);
        if (timer_ != nullptr) lv_timer_resume(timer_);
    } else {
        lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
        if (timer_ != nullptr) lv_timer_pause(timer_);
    }
    ESP_LOGI(TAG, "SetActive(%d)", (int)on);
}

void RoboFace::SetEmotion(int idx) {
    if (idx < 0 || idx > 20) idx = xfacetft::XF_NEUTRAL;
    idle_emotion_ = idx;
    emotion_idx_ = idx;
    if (!DriveActive()) face_.setEmotion(idx, t_ms_);
}

void RoboFace::SetEmotionByName(const char* str) {
    SetEmotion(xfacetft::EmotionFromName(str));
}

void RoboFace::SetSpeaking(bool on) { face_.setSpeaking(on); }

/* 行驶装饰动作 → 最接近的情绪（引擎没有"看方向"的 API，用情绪表达动作） */
void RoboFace::ApplyDriveEmotion() {
    int idx = idle_emotion_;
    switch (drive_dir_) {
        case 1: idx = xfacetft::XF_HAPPY; break;       /* 前进：开心 */
        case 2: idx = xfacetft::XF_SURPRISED; break;   /* 后退：惊讶 */
        case 3: idx = xfacetft::XF_THINKING; break;    /* 左转：思考（眼神偏） */
        case 4: idx = xfacetft::XF_CONFIDENT; break;   /* 右转：自信 */
        case 5: idx = xfacetft::XF_LAUGHING; break;    /* 跳舞：大笑 */
        default: break;
    }
    emotion_idx_ = idx;
    face_.setEmotion(idx, t_ms_);
}

void RoboFace::SetDriveAction(int dir, uint32_t hold_ms) {
    if (dir < 0 || dir > 5) dir = 0;
    drive_dir_ = dir;
    drive_until_ms_ = (dir == 0) ? 0 : t_ms_ + hold_ms;
    ApplyDriveEmotion();
}

void RoboFace::ClearDriveAction() {
    drive_dir_ = 0;
    drive_until_ms_ = 0;
    ApplyDriveEmotion();
}

bool RoboFace::DriveActive() const {
    return drive_dir_ != 0 && (int32_t)(drive_until_ms_ - t_ms_) > 0;
}

void RoboFace::ShowClock(bool on) {
    if (clock_box_ == nullptr) return;
    if (clock_shown_ == on) return;
    clock_shown_ = on;
    if (on) {
        lv_obj_add_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(clock_box_, LV_OBJ_FLAG_HIDDEN);
        last_clock_sec_ = -1;
        net_div_ = 0;
        UpdateClockText();
        UpdateNetText();   /* 刚切到时钟就把 IP/电量填好，不留 "--" */
    } else {
        lv_obj_add_flag(clock_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_HIDDEN);
    }
}

void RoboFace::UpdateClockText() {
    if (clock_time_ == nullptr) return;
    const time_t now = time(nullptr);
    struct tm tm_now {};
    localtime_r(&now, &tm_now);

    /* 没同步 NTP 时年份会是 1970 —— 直接摆 "--:--"，别显示假时间 */
    if (tm_now.tm_year < 124) {
        lv_label_set_text(clock_time_, "--:--");
        lv_label_set_text(clock_date_, "waiting for NTP");
        return;
    }
    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    lv_label_set_text(clock_time_, tbuf);

    char dbuf[32];
    snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d", tm_now.tm_year + 1900,
             tm_now.tm_mon + 1, tm_now.tm_mday);
    lv_label_set_text(clock_date_, dbuf);
}

void RoboFace::UpdateNetText() {
    if (clock_ip_ == nullptr || net_row_ == nullptr) return;

    auto& wifi = WifiManager::GetInstance();
    const bool linked = wifi.IsConnected();
    const std::string ip = linked ? wifi.GetIpAddress() : std::string();
    lv_label_set_text(clock_ip_, ip.empty() ? "--" : ip.c_str());

    int level = 0;
    bool charging = false;
    bool discharging = false;
    const bool has_bat = Board::GetInstance().GetBatteryLevel(level, charging, discharging);

    /* 电池：按百分比画实心填充条（搬自横屏状态栏）。
       不再用 LV_SYMBOL_CHARGE —— 充电时只剩一个闪电，看不出还剩多少电。 */
    if (battery_fill_ != nullptr && has_bat) {
        if (level < 0) level = 0;
        if (level > 100) level = 100;
        // 外框 22px、边框 1px、pad 1px → 内部可用宽度 18px 表示 100%
        int w = 18 * level / 100;
        if (w < 2) w = 2;
        lv_obj_set_width(battery_fill_, w);

        uint32_t col = 0x6BE36B;            // 正常：绿
        if (charging) {
            col = 0x5AA9FF;                 // 充电：蓝
        } else if (level <= 20) {
            col = 0xFF6B6B;                 // 低电量：红
        }
        lv_obj_set_style_bg_color(battery_fill_, lv_color_hex(col), 0);
    }

    /* WiFi：按 RSSI 点亮格数，阈值与横屏栏完全一致
       （≥-55 三格 / ≥-70 两格 / ≥-85 一格 / 更弱或无连接 0 格）。
       未连接时整组变红，跟「连着但信号弱」区分开。 */
    int lit = 0;
    bool connected = false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        connected = true;
        if (ap.rssi >= -55) {
            lit = 3;
        } else if (ap.rssi >= -70) {
            lit = 2;
        } else if (ap.rssi >= -85) {
            lit = 1;
        }
    }
    for (int i = 0; i < 3; ++i) {
        if (wifi_bars_[i] == nullptr) continue;
        lv_obj_set_style_bg_opa(wifi_bars_[i], i < lit ? LV_OPA_COVER : LV_OPA_20, 0);
        lv_obj_set_style_bg_color(wifi_bars_[i],
                                  lv_color_hex(connected ? 0xFFFFFF : 0xFF6B6B), 0);
    }

    /* 从机连接状态 + 实时距离：CAR 12cm OK / CAR 8cm STOP / CAR OFFLINE */
    if (clock_car_ != nullptr) {
        LinkStatus st;
        const bool car_linked = EspNowLink::Get().Status(&st);
        char car[48];
        if (car_linked) {
            const bool exploring = (st.flags & TLM_FLAG_EXPLORE) != 0;
            const bool blocked = (st.state == 3 || st.state == 4);   /* obstacle/fall */
            /* 2026-11-13 用户要求：屏幕上不再显示实时距离（网页里能看到）。
               这里只保留状态字，省得小数点一直在跳影响观感。 */
            snprintf(car, sizeof(car), "CAR %s",
                     exploring ? "EXP" : (blocked ? "STOP" : "OK"));
            lv_obj_set_style_text_color(clock_car_, lv_color_hex(0x6BE36B), 0);
        } else {
            snprintf(car, sizeof(car), "CAR OFFLINE");
            lv_obj_set_style_text_color(clock_car_, lv_color_hex(0xFF6B6B), 0);
        }
        lv_label_set_text(clock_car_, car);
    }
}

void RoboFace::TimerCb(lv_timer_t* t) {
    static_cast<RoboFace*>(lv_timer_get_user_data(t))->Tick();
}

void RoboFace::Tick() {
    if (!active_) return;
    t_ms_ += kFpsPeriodMs;

    if (clock_shown_) {
        const int64_t sec = esp_timer_get_time() / 1000000;
        if (sec != last_clock_sec_) {
            last_clock_sec_ = sec;
            UpdateClockText();
            /* IP/WiFi/电量 5 秒刷一次：电池要读 ADC、RSSI 要查驱动，每秒刷没必要 */
            if (++net_div_ >= 5) {
                net_div_ = 0;
                UpdateNetText();
            }
        }
        return;   /* 时钟模式下不画脸，省 CPU/带宽 */
    }

    /* 动作到期 → 回到 idle 情绪 */
    if (drive_dir_ != 0 && (int32_t)(drive_until_ms_ - t_ms_) <= 0) {
        drive_dir_ = 0;
        drive_until_ms_ = 0;
        ApplyDriveEmotion();
    }

    if (buf_ == nullptr || canvas_ == nullptr) return;

    /* 引擎画的是不透明像素，换情绪后眼睛变小会有残留 → 每帧清底（150KB ≈ 0.2ms） */
    memset(buf_, 0, (size_t)kDstW * kDstH * sizeof(uint16_t));
    face_.update(t_ms_);
    lv_obj_invalidate(canvas_);
}
