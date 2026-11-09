#include "ws2812_led.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "orb_audio.h"   // orb_peek_audio_global：律动效果从这取实时电平

#define TAG "Ws2812Led"

static Ws2812Led* s_instance = nullptr;
void SetWs2812Instance(Ws2812Led* led) { s_instance = led; }
Ws2812Led* Ws2812Instance() { return s_instance; }

namespace {

struct NamedColor {
    const char* name;
    uint8_t r, g, b;
};
/* 英文名给大模型用（它更倾向回英文），中文名给语音留余地 */
const NamedColor kColors[] = {
    {"red",    255,   0,   0}, {"green",   0, 255,   0}, {"blue",    0,  80, 255},
    {"white",  255, 255, 255}, {"yellow", 255, 200,   0}, {"purple", 128,   0, 255},
    {"pink",   255,  80, 190}, {"orange", 255, 120,   0}, {"cyan",    0, 220, 255},
    {"gold",   255, 190,  60},
    {"红",     255,   0,   0}, {"绿",       0, 255,   0}, {"蓝",      0,  80, 255},
    {"白",     255, 255, 255}, {"黄",     255, 200,   0}, {"紫",    128,   0, 255},
    {"粉",     255,  80, 190}, {"橙",     255, 120,   0},
};

bool ParseColor(std::string c, uint8_t* r, uint8_t* g, uint8_t* b) {
    if (c.empty()) {
        return false;
    }
    /* 统一小写再比较（模型可能回 "Red"） */
    for (auto& ch : c) {
        ch = (char)tolower((unsigned char)ch);
    }
    for (const auto& nc : kColors) {
        if (c == nc.name) {
            *r = nc.r; *g = nc.g; *b = nc.b;
            return true;
        }
    }
    if (c[0] == '#') {
        c = c.substr(1);
    }
    if (c.size() == 6 && strspn(c.c_str(), "0123456789abcdef") == 6) {
        unsigned v = 0;
        sscanf(c.c_str(), "%x", &v);
        *r = (uint8_t)(v >> 16);
        *g = (uint8_t)(v >> 8);
        *b = (uint8_t)v;
        return true;
    }
    return false;
}

/** HSV(h: 0-359, s/v: 0-1) -> RGB，彩虹效果用 */
void HsvToRgb(int h, float s, float v, uint8_t* r, uint8_t* g, uint8_t* b) {
    float f = (h % 60) / 60.0f;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);
    float rf = 0, gf = 0, bf = 0;
    switch ((h / 60) % 6) {
        case 0: rf = v; gf = t; bf = p; break;
        case 1: rf = q; gf = v; bf = p; break;
        case 2: rf = p; gf = v; bf = t; break;
        case 3: rf = p; gf = q; bf = v; break;
        case 4: rf = t; gf = p; bf = v; break;
        case 5: rf = v; gf = p; bf = q; break;
    }
    *r = (uint8_t)(rf * 255);
    *g = (uint8_t)(gf * 255);
    *b = (uint8_t)(bf * 255);
}

}  // namespace

Ws2812Led::Ws2812Led(gpio_num_t gpio, int count) : count_(count) {
    /* 配置抄自 main/led/circular_strip.cc —— 同一颗 WS2812，
       GRB 分量序 + 10MHz RMT，这套参数是验证过能点亮的。 */
    led_strip_config_t cfg = {};
    cfg.strip_gpio_num = gpio;
    cfg.max_leds = count_;
    cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    cfg.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt = {};
    rmt.resolution_hz = 10 * 1000 * 1000;

    if (led_strip_new_rmt_device(&cfg, &rmt, &strip_) != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 初始化失败 (gpio=%d)", (int)gpio);
        strip_ = nullptr;
        return;
    }
    led_strip_clear(strip_);
    ESP_LOGI(TAG, "WS2812 就绪: gpio=%d, %d 颗", (int)gpio, count_);
}

Ws2812Led::~Ws2812Led() {
    if (task_ != nullptr) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (strip_ != nullptr) {
        led_strip_del(strip_);
    }
}

void Ws2812Led::ShowScaled_(uint8_t r, uint8_t g, uint8_t b, float factor) {
    if (strip_ == nullptr) {
        return;
    }
    /* 亮度用平方（gamma≈2）：人眼对低亮度变化更敏感，
       线性缩放在 10% 以下几乎看不出差别。 */
    int lvl = (int)(level_ * factor);
    if (lvl > 100) lvl = 100;
    if (lvl < 0) lvl = 0;
    auto scale = [lvl](uint8_t c) -> uint32_t {
        uint32_t v = (uint32_t)c * lvl / 100;
        return (v * v) / 255;
    };
    for (int i = 0; i < count_; i++) {
        led_strip_set_pixel(strip_, i, scale(r), scale(g), scale(b));
    }
    led_strip_refresh(strip_);
}

bool Ws2812Led::Set(const std::string& color, int level, const std::string& effect) {
    /* 1. 颜色：空串 = 保持当前色（网页可以只调亮度） */
    if (!color.empty()) {
        uint8_t r, g, b;
        if (!ParseColor(color, &r, &g, &b)) {
            ESP_LOGW(TAG, "不认识的颜色: %s", color.c_str());
            return false;
        }
        r_ = r; g_ = g; b_ = b;
    }

    /* 2. 亮度 */
    if (level >= 0 && level <= 100) {
        level_ = level;
    }

    /* 3. 效果（中英文都认） */
    std::string e = effect;
    for (auto& ch : e) {
        ch = (char)tolower((unsigned char)ch);
    }
    if (e == "开") e = "on";
    else if (e == "关" || e == "close") e = "off";
    else if (e == "呼吸") e = "breathe";
    else if (e == "闪烁") e = "blink";
    else if (e == "彩虹") e = "rainbow";
    else if (e == "律动" || e == "音乐") e = "music";
    if (e != "on" && e != "off" && e != "breathe" && e != "blink" &&
        e != "rainbow" && e != "music") {
        ESP_LOGW(TAG, "不认识的效果: %s", effect.c_str());
        return false;
    }
    strncpy(effect_, e.c_str(), sizeof(effect_) - 1);
    effect_[sizeof(effect_) - 1] = '\0';

    /* 4. 懒启动效果任务：一个常驻低优先级任务按当前 effect 刷灯，
       之后每次 Set 只改参数就行，不用杀任务重建（重建有 join 竞态）。 */
    if (task_ == nullptr && strip_ != nullptr) {
        if (xTaskCreate(EffectTask_, "ws2812_fx", 3072, this, 3, &task_) != pdPASS) {
            ESP_LOGE(TAG, "效果任务创建失败");
            task_ = nullptr;
        }
    }
    /* 静态效果立即刷一次，别等任务的第一个 tick（30~200ms 的延迟
       在语音场景听感上就是"没反应"） */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (strcmp(effect_, "on") == 0) {
            ShowScaled_(r_, g_, b_, 1.0f);
        } else if (strcmp(effect_, "off") == 0) {
            ShowScaled_(0, 0, 0, 0);
        }
    }
    ESP_LOGI(TAG, "彩灯: #%02X%02X%02X 亮度%d 效果%s", r_, g_, b_, level_, effect_);
    return true;
}

std::string Ws2812Led::GetStateJson() {
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"color\":\"#%02X%02X%02X\",\"brightness\":%d,\"effect\":\"%s\"}",
             r_, g_, b_, level_, effect_);
    return std::string(buf);
}

/* 2026-11-05 律动开关：开 = 记住当前效果并切到 music；关 = 恢复。
   幂等：重复开/关不会把 prev 覆盖成 music 自身。 */
void Ws2812Led::SetDanceMode(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (on == dance_mode_) {
        return;
    }
    if (on) {
        strncpy(dance_prev_effect_, effect_, sizeof(dance_prev_effect_) - 1);
        dance_prev_effect_[sizeof(dance_prev_effect_) - 1] = '\0';
        strncpy(effect_, "music", sizeof(effect_) - 1);
        effect_[sizeof(effect_) - 1] = '\0';
    } else {
        strncpy(effect_, dance_prev_effect_, sizeof(effect_) - 1);
        effect_[sizeof(effect_) - 1] = '\0';
        /* 恢复静态效果立刻刷一次，别等任务 tick */
        if (strcmp(effect_, "on") == 0) {
            ShowScaled_(r_, g_, b_, 1.0f);
        } else if (strcmp(effect_, "off") == 0) {
            ShowScaled_(0, 0, 0, 0);
        }
    }
    dance_mode_ = on;
    ESP_LOGI(TAG, "律动模式 -> %d (恢复效果 %s)", on ? 1 : 0, dance_prev_effect_);
}

void Ws2812Led::EffectTask_(void* arg) {
    auto* self = static_cast<Ws2812Led*>(arg);
    const int64_t t0 = esp_timer_get_time();
    while (true) {
        const int64_t now = esp_timer_get_time() - t0;   // us
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            const char* e = self->effect_;
            if (strcmp(e, "on") == 0) {
                self->ShowScaled_(self->r_, self->g_, self->b_, 1.0f);
            } else if (strcmp(e, "off") == 0) {
                self->ShowScaled_(0, 0, 0, 0);
            } else if (strcmp(e, "breathe") == 0) {
                /* 2.8 秒一个呼吸周期，最低留 8% 别全黑（全黑像关了） */
                float f = 0.08f + 0.92f * (0.5f - 0.5f * cosf(now * 2.0f * (float)M_PI / 2800000.0f));
                self->ShowScaled_(self->r_, self->g_, self->b_, f);
            } else if (strcmp(e, "blink") == 0) {
                float f = ((now / 500000) % 2 == 0) ? 1.0f : 0.0f;
                self->ShowScaled_(self->r_, self->g_, self->b_, f);
            } else if (strcmp(e, "rainbow") == 0) {
                uint8_t r, g, b;
                HsvToRgb((int)(now / 17000) % 360, 1.0f, 1.0f, &r, &g, &b);
                self->ShowScaled_(r, g, b, 1.0f);
            } else if (strcmp(e, "music") == 0) {
                /* 2026-11-05 律动：亮度随总音量、色相随主频（低频偏暖、高频偏冷）。
                   orb_peek 是非消耗读（take 的 seq 全局单份，会被频谱条抢走）。 */
                static float s_lvl = 0.0f;
                static float s_band[4] = {0, 0, 0, 0};
                static int64_t s_last_feed_us = 0;
                float lvl = 0.0f;
                float band[4] = {0, 0, 0, 0};
                if (orb_peek_audio_global(&lvl, &band[0])) {
                    s_lvl = lvl;
                    for (int i = 0; i < 4; i++) s_band[i] = band[i];
                    s_last_feed_us = now;
                } else if (now - s_last_feed_us > 300000) {
                    /* 300ms 没有新音频数据（没人说话/没放歌）：慢慢熄灭 */
                    s_lvl *= 0.82f;
                    for (int i = 0; i < 4; i++) s_band[i] *= 0.82f;
                }
                /* 亮度：12% 底亮 + 音量曲线（平方让跳动更明显） */
                float factor = 0.12f + 0.88f * s_lvl * s_lvl;
                /* 色相：低频能量占比越大越偏暖（40=橙），高频占比大越偏冷（240=蓝） */
                float bass = s_band[0] + s_band[1];
                float treb = s_band[2] + s_band[3];
                float warm = (bass + treb > 0.01f) ? bass / (bass + treb) : 0.5f;
                int hue = 40 + (int)((1.0f - warm) * 200.0f);
                hue = (hue + (int)((now / 90000) % 40)) % 360;   // 缓慢转色相，别死板
                uint8_t r, g, b;
                HsvToRgb(hue, 0.9f, 1.0f, &r, &g, &b);
                self->ShowScaled_(r, g, b, factor);
            }
        }
        /* 呼吸/彩虹/律动 30ms 一帧才顺滑；静态效果降到 200ms 省刷新 */
        const bool animated = (strcmp(self->effect_, "breathe") == 0) ||
                              (strcmp(self->effect_, "rainbow") == 0) ||
                              (strcmp(self->effect_, "music") == 0) ||
                              (strcmp(self->effect_, "blink") == 0);
        vTaskDelay(pdMS_TO_TICKS(animated ? 30 : 200));
    }
}
