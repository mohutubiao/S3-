// ══════════════════════════════════════════════════════════════════════════
// xiaozhi_face_tft.h —— 原创 TFT 彩屏机器人表情（320x240 ST7789）· 21 情绪完整版
//
// 与 OLED 版（xiaozhi_face.h，128x64 单色）同一套 21 情绪、同一套动画
// （220ms 平滑过渡 / 自动眨眼 / 呼吸 / 说话嘴型 / 弹跳 / 抖动 / 泪滴 / 游移），
// 区别只在渲染层：
//   1. 坐标从 128x64 逻辑空间 ×2.5 映射到 320x240（脸占 320x160，垂直居中）
//   2. 眼睛 = 「方形圆角」轮廓 (1-t⁴)^0.25（与 OLED 版一致，侧边平直四角收圆）
//   3. 极光配色：横向渐变 青#26D0CE → 紫#7C5CFF → 粉#FF5FA2，眼睛/眉毛/嘴
//      逐列取渐变色；泪滴青、腮红粉、爱心眼/飞吻粉红、墨镜深紫、舌头浅粉
//
// 后端依赖：一个「水平矩形填充 + RGB565 颜色」原语：
//   rect(user, x, y, w, h, color)。LVGL 接法见配套《表情套装TFT接入说明.md》§3
//   （lv_canvas + PSRAM 缓冲 + 行 memset，约 100KB PSRAM，内部 SRAM 零新增）。
//
// 用法：
//   xfacetft::Backend be{ this, &my_rect_fn };
//   xfacetft::Face face;  face.begin(be);  face.setEmotion(XF_HAPPY);
//   // 每帧：外部先清屏（黑底），然后
//   face.setSpeaking(speaking);  face.update(now_ms);
//
// 情绪 idx（与 OLED 版一致，见 kEmotionName）：
//   0 neutral    1 happy      2 laughing   3 funny      4 sad
//   5 angry      6 crying     7 loving     8 embarrassed 9 surprised
//   10 shocked   11 thinking  12 winking   13 cool      14 relaxed
//   15 delicious 16 kissy     17 confident 18 sleepy    19 silly
//   20 confused
//
// 内存：渐变表 640B + Face 对象 ~300B，无动态分配。
// CPU：每帧 2 眼 × ~90 列 + 眉 + 嘴 ≈ 250 次行填充，30fps 占用可忽略。
// ══════════════════════════════════════════════════════════════════════════
#pragma once
#include <cstdint>
#include <cmath>

namespace xfacetft {

// ── 绘制后端：唯一依赖 ──
// rect(user, x, y, w, h, color)：填充水平矩形，RGB565 颜色。
// x/y 可能为负或越界，后端自行裁剪。
struct Backend {
    void* user = nullptr;
    void (*rect)(void* user, int x, int y, int w, int h, uint16_t color) = nullptr;
};

// ── RGB565 调色 ──
inline uint16_t Rgb565(int r, int g, int b) {
    if (r < 0) { r = 0; }
    if (r > 255) { r = 255; }
    if (g < 0) { g = 0; }
    if (g > 255) { g = 255; }
    if (b < 0) { b = 0; }
    if (b > 255) { b = 255; }
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// 情绪 idx 常量
enum : int {
    XF_NEUTRAL = 0,  XF_HAPPY = 1,    XF_LAUGHING = 2, XF_FUNNY = 3,     XF_SAD = 4,
    XF_ANGRY = 5,    XF_CRYING = 6,   XF_LOVING = 7,   XF_EMBARRASSED = 8, XF_SURPRISED = 9,
    XF_SHOCKED = 10, XF_THINKING = 11, XF_WINKING = 12, XF_COOL = 13,    XF_RELAXED = 14,
    XF_DELICIOUS = 15, XF_KISSY = 16, XF_CONFIDENT = 17, XF_SLEEPY = 18, XF_SILLY = 19,
    XF_CONFUSED = 20,
};

// 上位机情绪字符串 → idx
inline int EmotionFromName(const char* s) {
    static const char* kNames[] = {
        "neutral", "happy", "laughing", "funny", "sad", "angry", "crying",
        "loving", "embarrassed", "surprised", "shocked", "thinking", "winking",
        "cool", "relaxed", "delicious", "kissy", "confident", "sleepy",
        "silly", "confused",
    };
    for (int i = 0; i < 21; i++) {
        const char* a = kNames[i];
        const char* b = s;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == *b) return i;
    }
    return XF_NEUTRAL;
}

inline float xf_lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float xf_smooth(float t) { t = t < 0 ? 0 : (t > 1 ? 1 : t); return t * t * (3 - 2 * t); }

// 一只眼睛 + 配套嘴的全部参数（逻辑空间数值，与 OLED 版完全相同 —— 128x64 调好的
// 比例原封不动，渲染层统一 ×2.5。改表情参数请参考 OLED 版文档 §4 的字段说明）
struct Look {
    float w = 28, h = 26;
    float gx = 0, gy = 0;
    float topO = 0, topI = 0;
    float botO = 0, botI = 0;
    float bow = 0;
    float browO = -100, browI = -100;
    float mw = 11, mh = 9, mdy = 0;
    float mdx = 0;
    float mr = 3;
    float blinkMin = 2800, blinkMax = 5200;
    float shakeX = 0;
    float breathe = 0;
    bool  tear = false;
    bool  bounce = false;
    bool  heart = false;
    bool  closed = false;
    bool  shades = false;
    bool  blush = false;
    bool  tongue = false;
    bool  kiss = false;
};

// 21 组情绪 × 左右眼（与 OLED 版 kLooks 逐字节相同）
inline const Look* kLooks(int idx, bool left) {
    static const Look L[21][2] = {
        // 0 neutral 平静
        {{28,26, 0,0, 0,0,0,0,0, -100,-100, 11,9,0,0,3, 2800,5200, 0,0.04, 0,0,0,0,0,0},
         {28,26, 0,0, 0,0,0,0,0, -100,-100, 11,9,0,0,3, 2800,5200, 0,0.04, 0,0,0,0,0,0}},
        // 1 happy 开心
        {{28,23, 0,0, 0,0,0,0,9, -100,-100, 14,8,0,0,4, 4200,6400, 0,0, 0,1,0,0,0,0},
         {28,23, 0,0, 0,0,0,0,9, -100,-100, 14,8,0,0,4, 4200,6400, 0,0, 0,1,0,0,0,0}},
        // 2 laughing 大笑
        {{26,21, 0,0, 1,2,0,0,11, -100,-100, 17,11,0,0,4, 3200,5200, 0,0, 0,1,0,0,0,0},
         {26,21, 0,0, 1,2,0,0,11, -100,-100, 17,11,0,0,4, 3200,5200, 0,0, 0,1,0,0,0,0}},
        // 3 funny 调皮
        {{26,22, 0,0, 0,0,0,0,8, -100,-100, 15,9,0,0,4, 3600,6000, 0,0, 0,1,0,0,1,0},
         {28,24, 0,-1, 0,0,0,0,0, -1,1, 15,9,0,0,4, 3600,6000, 0,0, 0,1,0,0,0,0}},
        // 4 sad 难过
        {{26,24, 0,3, 6,1,0,0,0, -100,-100, 9,7,2,0,3, 3600,6200, 0,0, 1,0,0,0,0,0},
         {26,24, 0,3, 6,1,0,0,0, -100,-100, 9,7,2,0,3, 3600,6200, 0,0, 1,0,0,0,0,0}},
        // 5 angry 生气
        {{26,22, 0,0, 1,8,0,0,0, -2,4, 12,5,0,0,2, 4000,7000, 1,0, 0,0,0,0,0,0},
         {26,22, 0,0, 1,8,0,0,0, -2,4, 12,5,0,0,2, 4000,7000, 1,0, 0,0,0,0,0,0}},
        // 6 crying 哭泣
        {{24,20, 0,2, 5,3,0,0,0, -2,3, 10,11,1,0,4, 5000,8000, 0,0, 1,0,0,0,0,0},
         {24,20, 0,2, 5,3,0,0,0, -2,3, 10,11,1,0,4, 5000,8000, 0,0, 1,0,0,0,0,0}},
        // 7 loving 喜欢心动
        {{28,26, 0,0, 0,0,0,0,0, -100,-100, 10,8,0,0,4, 4500,7000, 0,0, 0,0,1,0,0,0},
         {28,26, 0,0, 0,0,0,0,0, -100,-100, 10,8,0,0,4, 4500,7000, 0,0, 0,0,1,0,0,0}},
        // 8 embarrassed 害羞尴尬
        {{26,22, 0,2, 3,1,0,0,0, -100,-100, 9,7,0,0,3, 3600,6200, 0,0, 0,0,0,1,0,0},
         {26,22, 0,2, 3,1,0,0,0, -100,-100, 9,7,0,0,3, 3600,6200, 0,0, 0,0,0,1,0,0}},
        // 9 surprised 惊讶
        {{32,28, 0,0, 0,0,0,0,0, -1,1, 8,10,0,0,4, 7000,11000, 0,0, 0,0,0,0,0,0},
         {32,28, 0,0, 0,0,0,0,0, -1,1, 8,10,0,0,4, 7000,11000, 0,0, 0,0,0,0,0,0}},
        // 10 shocked 震惊
        {{34,30, 0,0, 0,0,0,0,0, -3,3, 7,13,0,0,4, 8000,13000, 0,0, 0,0,0,0,0,0},
         {34,30, 0,0, 0,0,0,0,0, -3,3, 7,13,0,0,4, 8000,13000, 0,0, 0,0,0,0,0,0}},
        // 11 thinking 思考
        {{28,26, 2,-1, 0,0,0,0,0, -100,-100, 10,8,0,0,3, 3000,5200, 0,0, 0,0,0,0,0,0},
         {26,22, 2,-1, 0,5,0,0,0, -3,0, 10,8,0,0,3, 3000,5200, 0,0, 0,0,0,0,0,0}},
        // 12 winking 眨眼俏皮
        {{28,26, 0,0, 0,0,0,0,0, -100,-100, 12,7,0,2,3, 3600,6000, 0,0, 0,0,0,0,0,0},
         {28,26, 0,0, 0,0,0,0,0, -100,-100, 12,7,0,2,3, 3600,6000, 0,0, 0,1,0,0,0,0}},
        // 13 cool 酷酷
        {{28,20, 0,0, 0,0,0,0,0, -100,-100, 11,6,0,-2,3, 4200,6400, 0,0, 0,0,0,0,0,0},
         {28,20, 0,0, 0,0,0,0,0, -100,-100, 11,6,0,-2,3, 4200,6400, 0,0, 0,0,0,0,0,0}},
        // 14 relaxed 放松
        {{28,24, 0,0, 0,0,0,0,4, -100,-100, 12,7,0,0,4, 9000,14000, 0,0.06, 0,0,0,1,0,0},
         {28,24, 0,0, 0,0,0,0,4, -100,-100, 12,7,0,0,4, 9000,14000, 0,0.06, 0,0,0,1,0,0}},
        // 15 delicious 馋好吃
        {{28,23, 0,0, 0,0,0,0,8, -100,-100, 14,9,0,0,4, 4200,6400, 0,0, 0,0,0,0,1,0},
         {28,23, 0,0, 0,0,0,0,8, -100,-100, 14,9,0,0,4, 4200,6400, 0,0, 0,0,0,0,0,0}},
        // 16 kissy 亲亲
        {{28,24, 0,0, 0,0,0,0,5, -100,-100, 7,8,0,0,3, 3600,6000, 0,0, 0,0,0,0,0,1},
         {28,24, 0,0, 0,0,0,0,0, -100,-100, 7,8,0,0,3, 3600,6000, 0,0, 0,1,0,0,0,0}},
        // 17 confident 自信
        {{28,26, 0,0, 0,0,0,0,0, -3,-1, 12,6,0,3,3, 3400,5800, 0,0, 0,0,0,0,0,0},
         {28,26, 0,0, 0,0,0,0,0, -100,-100, 12,6,0,3,3, 3400,5800, 0,0, 0,0,0,0,0,0}},
        // 18 sleepy 犯困
        {{28,13, 0,2, 0,0,3,3,0, -100,-100, 9,8,1,0,3, 6000,9500, 0,0.02, 0,0,0,0,0,0},
         {28,13, 0,2, 0,0,3,3,0, -100,-100, 9,8,1,0,3, 6000,9500, 0,0.02, 0,0,0,0,0,0}},
        // 19 silly 傻萌
        {{26,24, 5,0, 0,0,0,0,0, -100,-100, 12,9,0,-2,3, 3000,5600, 0,0, 0,0,0,0,1,0},
         {26,24, -5,0, 0,0,0,0,0, -100,-100, 12,9,0,-2,3, 3000,5600, 0,0, 0,0,0,0,0,0}},
        // 20 confused 疑惑懵
        {{28,26, 2,0, 0,0,0,0,0, -3,0, 9,6,0,-2,3, 2800,5200, 0,0, 0,0,0,0,0,0},
         {26,24, -2,0, 0,0,0,0,0, 0,3, 9,6,0,-2,3, 2800,5200, 0,0, 0,0,0,0,0,0}},
    };
    if (idx < 0 || idx > 20) idx = 0;
    return &L[idx][left ? 0 : 1];
}

class Face {
public:
    void begin(const Backend& be, uint32_t now_ms = 0) {
        be_ = be;
        // 极光渐变表：x=0..319 → 青(38,208,206) → 紫(124,92,255) → 粉(255,95,162)
        for (int x = 0; x < 320; x++) {
            float t = x / 319.0f;
            int r, g, b;
            if (t < 0.5f) {
                float u = t / 0.5f;
                r = (int)(38 + (124 - 38) * u);
                g = (int)(208 + (92 - 208) * u);
                b = (int)(206 + (255 - 206) * u);
            } else {
                float u = (t - 0.5f) / 0.5f;
                r = (int)(124 + (255 - 124) * u);
                g = (int)(92 + (95 - 92) * u);
                b = (int)(255 + (162 - 255) * u);
            }
            grad_[x] = Rgb565(r, g, b);
        }
        cur_idx_ = -1;
        setEmotion(XF_NEUTRAL, now_ms);
        next_blink_ = now_ms + 1800 + (now_ms % 1400);
    }

    void setEmotion(int idx, uint32_t now_ms) {
        if (idx < 0 || idx > 20) idx = 0;
        if (idx == cur_idx_) return;
        from_ = (cur_idx_ < 0) ? *kLooks(idx, true) : cur_;
        fromR_ = (cur_idx_ < 0) ? *kLooks(idx, false) : curR_;
        cur_idx_ = idx;
        trans_t0_ = now_ms;
        entry_t0_ = now_ms;
    }

    void setSpeaking(bool on) { speaking_ = on; }

    // 每帧调用一次。外部负责清屏（黑底）和推屏，这里只画脸。
    void update(uint32_t now_ms) {
        float k = xf_smooth((now_ms - trans_t0_) / 220.0f);
        const Look* tl = kLooks(cur_idx_, true);
        const Look* tr = kLooks(cur_idx_, false);
        cur_  = mix(from_,  *tl, k);
        curR_ = mix(fromR_, *tr, k);

        // 说话：眼睛随嘴型轻微点头（gy/mw/mh 均为逻辑空间值）
        bool talking = speaking_;
        if (talking) {
            static const uint8_t kTH[3] = {6, 9, 11};
            static const uint8_t kTW[3] = {13, 11, 10};
            int ph = (now_ms / 110) % 3;
            mouth_h_ = kTH[ph]; mouth_w_ = kTW[ph];
            if (mouth_h_ >= 10) { cur_.gy -= 1; curR_.gy -= 1; }
        } else {
            mouth_w_ = (int)cur_.mw; mouth_h_ = (int)cur_.mh;
        }

        // confused：视线缓慢左右游移（逻辑 px）
        if (cur_idx_ == XF_CONFUSED) {
            float gxw = 1.5f * sinf(now_ms * (6.2831853f / 2300.0f));
            cur_.gx += gxw; curR_.gx += gxw;
        }
        // 笑：入场 900ms 弹跳（逻辑 px）
        if (tl->bounce) {
            uint32_t dt = now_ms - entry_t0_;
            if (dt < 900) {
                float amp = (cur_idx_ == XF_LAUGHING ? 3.5f : 2.5f) * (1.0f - dt / 900.0f);
                float b = amp * fabsf(sinf(dt * (6.2831853f / 300.0f)));
                cur_.gy -= b; curR_.gy -= b;
            }
        }
        // 生气：横向抖（设备 px）
        int shake = 0;
        if ((int)cur_.shakeX) shake = ((now_ms / 80) & 1) ? 3 : -3;

        // ── 呼吸 ──
        float brL = 1.0f + cur_.breathe * sinf(now_ms * (6.2831853f / 1600.0f));
        float brR = 1.0f + curR_.breathe * sinf(now_ms * (6.2831853f / 1600.0f));

        // ── 眨眼 ──
        float blink = 1.0f;
        if (blink_t0_ >= 0) {
            float bp = (now_ms - blink_t0_) / 140.0f;
            if (bp >= 1.0f) { blink_t0_ = -1; next_blink_ = now_ms + rand_range(cur_.blinkMin, cur_.blinkMax); }
            else blink = fabsf(cosf(3.14159265f * bp));
        } else if ((int32_t)(now_ms - next_blink_) >= 0) {
            blink_t0_ = now_ms;
        }

        // ── 画眼睛 + 眉毛（设备坐标：逻辑 ×2.5，脸区 y0=40）──
        drawEye(112, 88, cur_,  true,  blink, brL, shake, now_ms);
        drawEye(208, 88, curR_, false, blink, brR, shake, now_ms);
        // cool：镜片中间的鼻梁架
        if (cur_.shades || curR_.shades) {
            rect(145, 83 + (int)(cur_.gy * 2.5f + 0.5f), 35, 7, grad_[160]);
        }

        // ── 画嘴 ──
        drawMouth(160 + (int)(cur_.mdx * 2.5f), 158 + (int)(cur_.mdy * 2.5f), now_ms);
    }

private:
    uint16_t grad(int x) const {
        if (x < 0) { x = 0; }
        if (x > 319) { x = 319; }
        return grad_[x];
    }

    // 逐列画一只眼（设备空间）。left=true 时 t 的 +1 侧是内侧（靠鼻子）。
    void drawEye(int cx, int cy, const Look& e, bool left, float blink, float breathe, int shake, uint32_t now_ms) {
        // 逻辑 → 设备：视线偏移
        cx += (int)(e.gx * 2.5f + 0.5f);
        cy += (int)(e.gy * 2.5f + 0.5f);
        int hw = (int)(e.w * 1.25f + 0.5f);
        float hh = e.h * 1.25f * breathe;

        // ── 特形：墨镜 ──
        if (e.shades) {
            uint16_t lens = Rgb565(88, 78, 140);     // 深紫镜片（黑底上要够亮才读得出）
            rect(cx - 35 + shake, cy - 15, 70, 25, lens);
            rect(cx - 30 + shake, cy - 20, 60, 35, lens);
            return;
        }
        // ── 特形：闭眼（一条线；bow>0 时 ∩ 弧）──
        if (e.closed) {
            uint16_t col = grad(cx);
            for (int dx = -hw; dx <= hw; dx++) {
                float t = hw ? (float)dx / hw : 0;
                int y = cy - 2;
                if (e.bow > 0.5f) y -= (int)(e.bow * 2.5f * (1.0f - t * t) + 0.5f);
                rect(cx + dx + shake, y, 2, 5, col);
            }
            return;
        }
        // ── 特形：爱心眼 ──
        if (e.heart) {
            uint16_t hcol = Rgb565(255, 77, 126);
            for (int dx = -hw; dx <= hw; dx++) {
                float u = hw ? (float)dx / hw : 0;
                float bb = 1.0f - u * u; if (bb < 0) bb = 0;
                float topArc = powf(bb, 0.6f) * hh;
                float dip = hh * 0.5f * expf(-u * u * 7.0f);
                int yTop = (int)(cy - topArc + dip + 0.5f);
                int yBot = (int)(cy + hh * 1.15f * powf(bb, 0.55f) + 0.5f);
                if (yBot - yTop < 1) continue;
                rect(cx + dx + shake, yTop, 1, yBot - yTop, hcol);
            }
            return;
        }

        for (int dx = -hw; dx <= hw; dx++) {
            float t = hw ? (float)dx / hw : 0;
            float base = 1.0f - t * t;
            if (base < 0) base = 0;
            // 方形圆角轮廓：四次方超椭圆（与 OLED 版一致）
            float at = t < 0 ? -t : t;
            float corner = 1.0f - at * at * at * at;
            if (corner < 0) corner = 0;
            float half = hh * powf(corner, 0.25f) * blink;
            float tin = left ? t : -t;
            float kk = (tin + 1) * 0.5f;
            float cutT = xf_lerp(e.topO, e.topI, kk) * 2.5f;
            float cutB = (xf_lerp(e.botO, e.botI, kk) + e.bow * base) * 2.5f;
            int yTop = (int)(cy - half + cutT + 0.5f);
            int yBot = (int)(cy + half - cutB + 0.5f);
            int x = cx + dx + shake;
            uint16_t col = grad(x);
            if (yBot - yTop < 4 && blink < 0.15f) {    // 眨眼闭合 → 一条 5px 线
                rect(x, cy - 2, 1, 5, col);
                continue;
            }
            if (yBot - yTop < 1) continue;
            rect(x, yTop, 1, yBot - yTop, col);
        }
        // 眉毛
        if (e.browO > -50) {
            int eyeTop = (int)(cy - e.h * 1.25f * breathe);
            float yO = eyeTop - 10 + e.browO * 2.5f, yI = eyeTop - 10 + e.browI * 2.5f;
            for (int dx = -27; dx <= 27; dx++) {
                float t = (dx + 27) / 54.0f;
                int y = (int)(xf_lerp(yO, yI, left ? t : 1 - t) + 0.5f);
                rect(cx + dx + shake, y, 2, 6, grad(cx + dx + shake));
            }
        }
        // 泪滴（外侧眼角往下淌）
        if (e.tear) {
            int period = 1900;
            float p = (now_ms % period) / (float)period;
            if (p < 0.62f) {
                int dy = (int)(p / 0.62f * 33);
                int tx = cx + (left ? -(hw - 3) : (hw - 3)) + shake;
                int ty = (int)(cy + e.h * 1.25f) - 5 + dy;
                uint16_t tcol = Rgb565(41, 216, 232);
                rect(tx, ty, 5, 7, tcol);
                rect(tx + 1, ty + 7, 3, 1, tcol);
            }
        }
        // 腮红（眼外下方 3 条 45° 斜线）
        if (e.blush) {
            uint16_t bcol = Rgb565(255, 110, 158);
            for (int k = 0; k < 3; k++) {
                int bx = cx + (left ? -(hw + 5 + k * 10) : (hw + 5 + k * 10)) + shake;
                for (int i = 0; i < 10; i++) rect(bx + i, cy + 18 + i, 2, 2, bcol);
            }
        }
    }

    // 圆角矩形嘴：逐列圆角裁剪；tongue / kiss 特形挂这里（设备空间）
    void drawMouth(int cx, int cy, uint32_t now_ms) {
        int w = (int)(mouth_w_ * 2.5f + 0.5f);
        int h = (int)(mouth_h_ * 2.5f + 0.5f);
        int r = (int)(cur_.mr * 2.5f + 0.5f);
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;
        int hw = w / 2, hh = h / 2;
        for (int dx = -hw; dx <= hw; dx++) {
            int ax = hw - (dx < 0 ? -dx : dx);
            int trim = (ax < r) ? (int)(r - sqrtf((float)(r * r - (r - ax) * (r - ax)))) : 0;
            rect(cx + dx, cy - hh + trim, 1, h - 2 * trim, grad(cx + dx));
        }
        // 吐舌（微微上下点）
        if (cur_.tongue) {
            int bob = ((now_ms / 400) & 1) ? 3 : 0;
            int ty = cy + hh + 2 + bob;
            rect(cx - 8, ty, 16, 10, Rgb565(255, 127, 168));
            rect(cx - 5, ty + 10, 10, 3, Rgb565(255, 127, 168));
        }
        // 飞吻小爱心（嘴角上方飘，上下浮动）
        if (cur_.kiss) {
            int hx = cx + hw + 10;
            int hy = cy - hh - 15 - ((now_ms / 350) & 1 ? 3 : 0);
            uint16_t kcol = Rgb565(255, 143, 192);
            rect(hx, hy, 3, 3, kcol);
            rect(hx + 8, hy, 3, 3, kcol);
            rect(hx, hy + 3, 11, 2, kcol);
            rect(hx + 3, hy + 5, 6, 2, kcol);
            rect(hx + 5, hy + 7, 3, 2, kcol);
        }
    }

    Look mix(const Look& a, const Look& b, float t) {
        Look o;
        o.w = xf_lerp(a.w, b.w, t);   o.h = xf_lerp(a.h, b.h, t);
        o.gx = xf_lerp(a.gx, b.gx, t); o.gy = xf_lerp(a.gy, b.gy, t);
        o.topO = xf_lerp(a.topO, b.topO, t); o.topI = xf_lerp(a.topI, b.topI, t);
        o.botO = xf_lerp(a.botO, b.botO, t); o.botI = xf_lerp(a.botI, b.botI, t);
        o.bow = xf_lerp(a.bow, b.bow, t);
        o.browO = xf_lerp(a.browO, b.browO, t); o.browI = xf_lerp(a.browI, b.browI, t);
        o.mw = xf_lerp(a.mw, b.mw, t); o.mh = xf_lerp(a.mh, b.mh, t);
        o.mdy = xf_lerp(a.mdy, b.mdy, t); o.mdx = xf_lerp(a.mdx, b.mdx, t);
        o.mr = xf_lerp(a.mr, b.mr, t);
        o.blinkMin = xf_lerp(a.blinkMin, b.blinkMin, t);
        o.blinkMax = xf_lerp(a.blinkMax, b.blinkMax, t);
        o.shakeX = xf_lerp(a.shakeX, b.shakeX, t);
        o.breathe = xf_lerp(a.breathe, b.breathe, t);
        o.tear     = (t > 0.5f) ? b.tear     : a.tear;
        o.bounce   = (t > 0.5f) ? b.bounce   : a.bounce;
        o.heart    = (t > 0.5f) ? b.heart    : a.heart;
        o.closed   = (t > 0.5f) ? b.closed   : a.closed;
        o.shades   = (t > 0.5f) ? b.shades   : a.shades;
        o.blush    = (t > 0.5f) ? b.blush    : a.blush;
        o.tongue   = (t > 0.5f) ? b.tongue   : a.tongue;
        o.kiss     = (t > 0.5f) ? b.kiss     : a.kiss;
        return o;
    }

    float rand_range(float a, float b) {
        if (b <= a) return a;
        static uint32_t s = 0x9E3779B9u;
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return a + (b - a) * ((s >> 8) / 16777216.0f);
    }

    void rect(int x, int y, int w, int h, uint16_t c) {
        if (be_.rect) be_.rect(be_.user, x, y, w, h, c);
    }

    Backend be_;
    uint16_t grad_[320];         // 极光渐变表（begin 建好，640B）
    int  cur_idx_ = 0;
    Look cur_, curR_, from_, fromR_;
    uint32_t trans_t0_ = 0, entry_t0_ = 0;
    int32_t  blink_t0_ = -1;
    uint32_t next_blink_ = 0;
    bool speaking_ = false;
    int  mouth_w_ = 11, mouth_h_ = 9;   // 逻辑空间值
};

} // namespace xfacetft
