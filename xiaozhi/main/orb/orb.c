/**
 * orb.c —— 呼吸圆球渲染核心（纯整数渲染路径）
 *
 * 球体的构成（由外到内）：
 *   外发光   3 条同心环带，alpha 由外向内递增
 *   边缘光   1 条细亮环
 *   球体     4 条同心环带做径向暗角 + 逐行垂直渐变（等效连续渐变）
 *   高光     3 层嵌套小椭圆
 *
 * 关键实现：
 *   - 轮廓 32 边形，环带 = 外多边形扫描线 − 内多边形扫描线，总填充量 ≈ 一个圆盘
 *   - 边表预计算 x 增量（Q16），逐行只做乘加，无除法
 *   - 颜色逐行由 orb_grad 直接算（约 30 次浮点运算/行），省掉查表内存
 *   - sin 走 256 项 Q12 查表
 */
#include "orb.h"
#include <math.h>
#include <string.h>

/* ==================== sin 查表 (Q12) ==================== */
#define ORB_LUT_N 256
static int16_t g_sin[ORB_LUT_N + 1];
static int     g_sin_ready = 0;

static void orb_trig_init(void) {
    if (g_sin_ready) return;
    for (int i = 0; i <= ORB_LUT_N; i++)
        g_sin[i] = (int16_t)(sinf((float)i / ORB_LUT_N * 6.28318530718f) * 4096.0f);
    g_sin_ready = 1;
}

/** 查表 sin，输入任意弧度 */
static inline float orb_sinf(float x) {
#if ORB_USE_TRIG_LUT
    const float INV_T = 0.15915494309189535f;   /* 1 / 2π */
    float u = x * INV_T;
    u -= floorf(u);
    uint32_t a = (uint32_t)(u * 65536.0f) & 0xFFFFu;
    uint32_t i = a >> 8, f = a & 255u;
    int32_t s0 = g_sin[i], s1 = g_sin[i + 1];
    return (float)(s0 + (((s1 - s0) * (int32_t)f) >> 8)) * (1.0f / 4096.0f);
#else
    return sinf(x);
#endif
}

/** 查表 cos / sin，输入 0..65535 代表 0..2π，返回 Q12 */
static inline int32_t orb_lcos(uint32_t a) { return g_sin[((a + 16384u) & 0xFFFFu) >> 8]; }
static inline int32_t orb_lsin(uint32_t a) { return g_sin[(a & 0xFFFFu) >> 8]; }

/* ==================== 默认状态参数 ==================== */
static const orb_cfg_t g_dflt[ORB_STATE_MAX] = {
    /* 2026-11-02 手感调整：用户反馈「不够圆」+「说话时扩张像一泡尿」。
       招式就是把形变(P_WOB)压到原来的 1/3 左右、音频驱动(P_REACT)降到 0.9，
       让球体保持球形，靠颜色和亮度变化表达情绪，而不是靠"胀大/扭曲"。
       2026-11-02 再改：用户要「正圆」—— P_WOB 全部归零，轮廓就是数学圆；
       情绪与声波改由颜色、亮度、以及 orb_lvgl 里的外扩声波环表达。 */
    /* IDLE */      {{0.035f, 0.55f, 0.000f, 0.70f, 0.85f, 0.55f, 0.15f},
                     {{ 99,102,241},{129,140,248},{ 34,211,238}}},
    /* LISTENING */ {{0.050f, 1.15f, 0.000f, 1.70f, 1.15f, 0.85f, 0.90f},
                     {{ 34,211,238},{ 59,130,246},{129,140,248}}},
    /* SPEAKING */  {{0.045f, 2.10f, 0.000f, 2.40f, 1.55f, 1.15f, 0.90f},
                     {{168, 85,247},{236, 72,153},{251,146, 60}}},
    /* THINKING */  {{0.038f, 0.85f, 0.000f, 1.10f, 1.00f, 0.70f, 0.35f},
                     {{ 59,130,246},{ 99,102,241},{168, 85,247}}},
};
static orb_cfg_t g_cfg[ORB_STATE_MAX];

void orb_override_state(orb_state_t s, const orb_cfg_t *cfg) {
    if (s < 0 || s >= ORB_STATE_MAX) return;
    g_cfg[s] = cfg ? *cfg : g_dflt[s];
}

/* ==================== 初始化 ==================== */
void orb_init(orb_t *o, int cx, int cy, int r_base) {
    orb_trig_init();
    if (!g_cfg[0].p[0]) {
        for (int i = 0; i < ORB_STATE_MAX; i++) g_cfg[i] = g_dflt[i];
    }
    memset(o, 0, sizeof(*o));
    o->cx = (int16_t)cx; o->cy = (int16_t)cy; o->r_base = (int16_t)r_base;
    o->clip_x0 = -32768; o->clip_y0 = -32768; o->clip_x1 = 32767; o->clip_y1 = 32767;
    o->state = ORB_IDLE;
    o->auto_audio = 1;
    for (int i = 0; i < P_N; i++) o->p[i] = g_cfg[0].p[i];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) o->pal[i][j] = (float)g_cfg[0].pal[i][j];
    o->dmax_q8 = 256;
    /* 首帧之前给个保守的外接半径，避免 r_out=0 */
    o->r_out = (int16_t)(r_base * (ORB_GLOW_SCALE + 2) / 10);
}

void orb_set_backend(orb_t *o, orb_span_cb span, void *user) { o->span = span; o->user = user; }
void orb_set_clip(orb_t *o, int x0, int y0, int x1, int y1) {
    o->clip_x0 = (int16_t)x0; o->clip_y0 = (int16_t)y0;
    o->clip_x1 = (int16_t)x1; o->clip_y1 = (int16_t)y1;
}
void orb_set_state(orb_t *o, orb_state_t s) { if (s >= 0 && s < ORB_STATE_MAX) o->state = (int)s; }
void orb_set_auto_audio(orb_t *o, int enable) { o->auto_audio = enable; }

void orb_feed_audio(orb_t *o, float level, const float band[4]) {
    o->auto_audio = 0;
    o->level = level < 0 ? 0 : (level > 1 ? 1 : level);
    for (int i = 0; i < 4; i++) {
        float v = band[i] < 0 ? 0 : (band[i] > 1 ? 1 : band[i]);
        o->band[i] += (v - o->band[i]) * 0.35f;
    }
}

/* ==================== 合成包络（无麦克风时） ==================== */
static void orb_synth(orb_t *o, float t) {
    float lv;
    switch (o->state) {
    case ORB_SPEAKING: {
        /* 2026-11-02：burst 从 0.42 降到 0.20、抖动减半 ——
           原来那个大脉冲正是"猛地扩张一下"的来源，现在改成小幅起伏。 */
        float a = orb_sinf(t * 6.7f) * 0.5f + 0.5f;
        float b = orb_sinf(t * 2.9f + 1.7f) * 0.5f + 0.5f;
        float burst = orb_sinf(t * 2.15f + 0.4f); if (burst < 0) burst = 0;
        burst *= burst;
        float j = orb_sinf(t * 23.3f) * 0.03f + orb_sinf(t * 11.1f) * 0.02f;
        lv = 0.30f + 0.32f * a + 0.18f * b + 0.20f * burst + j;
        break;
    }
    case ORB_LISTENING: {
        float a = orb_sinf(t * 2.3f) * 0.5f + 0.5f;
        float b = orb_sinf(t * 5.1f + 2.1f) * 0.5f + 0.5f;
        lv = 0.10f + 0.16f * a + 0.10f * b;
        break;
    }
    case ORB_THINKING:
        lv = 0.10f + 0.07f * (orb_sinf(t * 1.7f) * 0.5f + 0.5f); break;
    default:
        lv = 0.04f; break;
    }
    if (lv > 1) lv = 1;
    o->level += (lv - o->level) * 0.25f;

    float base = (o->state == ORB_SPEAKING || o->state == ORB_LISTENING) ? 1.0f : 0.35f;
    for (int i = 0; i < 4; i++) {
        float v = orb_sinf(t * (2.1f + i * 1.37f) + i * 2.2f) * 0.5f + 0.5f;
        float w = orb_sinf(t * (7.3f + i * 0.9f) + i) * 0.5f + 0.5f;
        float tgt = base * (0.25f + 0.45f * v + 0.30f * w) * (4.0f - i * 0.8f) / 4.0f;
        o->band[i] += (tgt - o->band[i]) * 0.18f;
    }
}

/* ==================== 参数插值 ==================== */
void orb_update(orb_t *o, uint32_t dt_ms) {
    if (dt_ms > 200) dt_ms = 200;
    o->t_ms = (o->t_ms + dt_ms) % 3600000u;

    float k = (float)dt_ms / ((float)dt_ms + 180.0f);
    const orb_cfg_t *c = &g_cfg[o->state];
    for (int i = 0; i < P_N; i++) o->p[i] += (c->p[i] - o->p[i]) * k;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        o->pal[i][j] += ((float)c->pal[i][j] - o->pal[i][j]) * k;

    if (o->auto_audio) orb_synth(o, o->t_ms * 0.001f);
    if (o->state == ORB_THINKING)
        o->spin += dt_ms * 0.0016f * o->p[P_SPEED];
}

/* ==================== 轮廓 ==================== */
static void orb_build_unit(orb_t *o) {
    const float T = 6.28318530718f;
    float t  = o->t_ms * 0.001f;
    float ws = o->p[P_WOBS];
    float wob = o->p[P_WOB] * (1.0f + o->level * 0.55f * o->p[P_REACT]);
    float drive = 0.35f + 0.90f * o->p[P_REACT] * (0.4f + o->level);
    int32_t dmax = 4096;                 /* 形变峰值 Q12，下限 1.0 */

    for (int i = 0; i < ORB_SEG; i++) {
        uint32_t ang = (uint32_t)(((int32_t)i * 65536) / ORB_SEG);
        float a = (float)i / ORB_SEG * T;
        float d = 1.0f;
        d += wob * (0.55f * orb_sinf(3.0f * a + t * ws)
                  + 0.30f * orb_sinf(5.0f * a - t * ws * 1.27f)
                  + 0.18f * orb_sinf(2.0f * a + t * ws * 0.63f));
        /* 频段引起的轮廓起伏：同样挂在 P_WOB 上。
           2026-11-02：P_WOB=0 时这一项必须也消失，否则哪怕 wob 归零，
           说话/聆听时仍会被 audio band 拉成不规则的形状（用户要"正圆"）。 */
        if (wob > 0.0f) {
            for (int b = 0; b < 4; b++)
                d += drive * o->band[b] * 0.085f *
                     orb_sinf((b + 2) * a + t * ws * (0.8f + b * 0.35f) + b * 1.3f);
        }

        /* 形变上限：4 个频段同时拉满时 d 会冲到 1.9，
           既让形状失控，也把脏框顶到屏幕外。钳到 1.45。 */
        if (d < 0.30f) d = 0.30f;
        else if (d > 1.45f) d = 1.45f;
        int32_t dd = (int32_t)(d * 4096.0f);
        if (dd > dmax) dmax = dd;
        int32_t cc = orb_lcos(ang), ss = orb_lsin(ang);
        o->ux[i] = (int16_t)((cc * dd) >> 12);
        o->uy[i] = (int16_t)((ss * dd) >> 12);
    }
    o->dmax_q8 = (uint16_t)((dmax + 8) >> 4);   /* Q12 -> Q8 */
}

/* ==================== 扫描线 ==================== */
typedef struct { int16_t y0, y1; int32_t x0, xinc; } orb_edge_t;

static int orb_edges(const int16_t *X, const int16_t *Y, int n, orb_edge_t *e) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1 == n) ? 0 : i + 1;
        int ya = Y[i], yb = Y[j];
        if (ya == yb) continue;
        int y0, y1, xa, xb;
        if (ya < yb) { y0 = ya; y1 = yb; xa = X[i]; xb = X[j]; }
        else         { y0 = yb; y1 = ya; xa = X[j]; xb = X[i]; }
        e[m].y0 = (int16_t)y0;
        e[m].y1 = (int16_t)y1;
        e[m].x0 = (int32_t)xa << 16;
        e[m].xinc = (int32_t)(((int32_t)(xb - xa)) << 16) / (y1 - y0);
        m++;
    }
    return m;
}

static inline void orb_row(const orb_edge_t *e, int m, int y, int *px0, int *px1) {
    int x0 = 0x7FFFFFFF, x1 = -0x7FFFFFFF;
    for (int k = 0; k < m; k++) {
        if (y >= e[k].y0 && y < e[k].y1) {
            int x = (int32_t)(e[k].x0 + (int32_t)(y - e[k].y0) * e[k].xinc) >> 16;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
        }
    }
    *px0 = x0; *px1 = x1;
}

/* ==================== 调色 ==================== */
/** k: 0(亮锚点)→1(暗锚点)，bright: 亮度系数 */
static inline uint16_t orb_grad(const float pal[3][3], float k, float bright) {
    float r, g, b;
    if (k < 0.5f) {
        float u = k * 2.0f;
        r = pal[0][0] + (pal[1][0] - pal[0][0]) * u;
        g = pal[0][1] + (pal[1][1] - pal[0][1]) * u;
        b = pal[0][2] + (pal[1][2] - pal[0][2]) * u;
    } else {
        float u = (k - 0.5f) * 2.0f;
        r = pal[1][0] + (pal[2][0] - pal[1][0]) * u;
        g = pal[1][1] + (pal[2][1] - pal[1][1]) * u;
        b = pal[1][2] + (pal[2][2] - pal[1][2]) * u;
    }
    r *= bright; g *= bright; b *= bright;
    uint8_t R = r > 255 ? 255 : (uint8_t)r;
    uint8_t G = g > 255 ? 255 : (uint8_t)g;
    uint8_t B = b > 255 ? 255 : (uint8_t)b;
    return orb_rgb565(R, G, B);
}

/* ==================== 带裁剪的 span 输出 ==================== */
static inline void orb_out(orb_t *o, int y, int x0, int x1, uint16_t c, uint8_t a) {
    if (x1 <= x0) return;
    if (y < o->clip_y0 || y > o->clip_y1) return;
    if (x0 < o->clip_x0) x0 = o->clip_x0;
    if (x1 > o->clip_x1 + 1) x1 = o->clip_x1 + 1;
    if (x1 <= x0) return;
    o->stat_px += (uint32_t)(x1 - x0);
    if (a < 32) o->stat_blend += (uint32_t)(x1 - x0);
    if (o->span) o->span(o, y, x0, x1, c, a);
}

void orb_clear(orb_t *o, uint16_t bg) {
    for (int y = o->dy0; y <= o->dy1; y++) orb_out(o, y, o->dx0, o->dx1 + 1, bg, 32);
}

/* ==================== 环带填充 ==================== */
/**
 * 填充 [Ro, Ri) 之间的环带（Ri<=0 时为整盘）。
 * use_grad=1 时逐行取调色渐变（亮度 bright），否则用 solid。
 * yf0/yf1 为渐变的参考行范围（通常 = 整球的上下边界）。
 */
static void orb_band(orb_t *o, int Ro, int Ri, uint8_t alpha,
                     int use_grad, float bright, uint16_t solid, int yf0, int yf1)
{
    static orb_edge_t eo[ORB_SEG], ei[ORB_SEG];
    static int16_t xo[ORB_SEG], yo[ORB_SEG];
    int mo, mi = 0;

    for (int i = 0; i < ORB_SEG; i++) {
        xo[i] = (int16_t)(o->cx + ((o->ux[i] * Ro) >> 12));
        yo[i] = (int16_t)(o->cy + ((o->uy[i] * Ro) >> 12));
    }
    mo = orb_edges(xo, yo, ORB_SEG, eo);
    if (Ri > 0) {
        for (int i = 0; i < ORB_SEG; i++) {
            o->lx[i] = (int16_t)(o->cx + ((o->ux[i] * Ri) >> 12));
            o->ly[i] = (int16_t)(o->cy + ((o->uy[i] * Ri) >> 12));
        }
        mi = orb_edges(o->lx, o->ly, ORB_SEG, ei);
    }

    int ys = yo[0], ye = yo[0];
    for (int i = 1; i < ORB_SEG; i++) { if (yo[i] < ys) ys = yo[i]; if (yo[i] > ye) ye = yo[i]; }
    if (ys < o->clip_y0) ys = o->clip_y0;
    if (ye > o->clip_y1) ye = o->clip_y1;
    if (ys > ye) return;

    int span = yf1 - yf0 + 1;
    if (span <= 0) return;

    for (int y = ys; y <= ye; y++) {
        int a0, a1, b0, b1;
        orb_row(eo, mo, y, &a0, &a1);
        if (a1 < a0) continue;
        if (mi) orb_row(ei, mi, y, &b0, &b1);
        else    { b0 = 0; b1 = -1; }

        uint16_t c = solid;
        if (use_grad) {
            float kk = (float)(y - yf0) / (float)span;
            if (kk < 0) kk = 0;
            if (kk > 1) kk = 1;
            c = orb_grad(o->pal, kk, bright);
        }
        if (b1 < b0) {
            orb_out(o, y, a0, a1 + 1, c, alpha);
        } else {
            if (b0 > a0) orb_out(o, y, a0, b0, c, alpha);
            if (a1 > b1) orb_out(o, y, b1 + 1, a1 + 1, c, alpha);
        }
    }
}

/* ==================== 主绘制 ==================== */
void orb_draw(orb_t *o) {
    if (!o->span) return;
    o->stat_px = 0;
    o->stat_blend = 0;

    float t = o->t_ms * 0.001f;
    float breath = orb_sinf(t * o->p[P_SPEED] * 1.35f) * 0.6f
                 + orb_sinf(t * o->p[P_SPEED] * 0.53f + 1.2f) * 0.4f;
    /* 音频驱动的半径膨胀：0.16 → 0.07。
       原来 level 满格能胀 16%*P_REACT（说话约 +20%），一鼓一鼓地撑大，
       视觉上就是用户说的「一泡尿」。降到 7% 后只在原地轻轻起伏。 */
    float Rf = (float)o->r_base * (1.0f + o->p[P_BREATH] * breath + o->level * 0.07f * o->p[P_REACT]);
    int R = (int)(Rf + 0.5f);
    if (R < 4) R = 4;

    int yf0 = o->cy - R, yf1 = o->cy + R;      /* 渐变参考范围 */

    /* ---- 先建轮廓，拿到本帧形变峰值，脏框才算得准 ---- */
    orb_build_unit(o);

    /* ---- 脏矩形：按「实际最大外接半径」算，不是名义半径 ---- */
    int Rg = R + (int)(R * 0.08f) + 2;         /* 无发光时的保守半径 */
#if ORB_ENABLE_GLOW
    if (o->p[P_GLOW] > 0.02f) Rg = (R * ORB_GLOW_SCALE) / 10;
#endif
    if (Rg < R) Rg = R;
    /* 轮廓抖动会把外缘推到 Rg * dmax 之外 */
    int ext = (int)(((int32_t)Rg * (int32_t)o->dmax_q8 + 255) >> 8);
    /* 思考弧线画在 1.42R~1.76R 上，再加点半径 */
    if (o->state == ORB_THINKING && o->p[P_SPEED] > 0.01f) {
        int ae = R + (int)(R * 0.80f) + 4;
        if (ae > ext) ext = ae;
    }
    ext += 2;
    o->r_out = (int16_t)ext;
    {
        int x0 = o->cx - ext, x1 = o->cx + ext;
        int y0 = o->cy - ext, y1 = o->cy + ext;
        if (x0 < o->clip_x0) x0 = o->clip_x0;
        if (x1 > o->clip_x1) x1 = o->clip_x1;
        if (y0 < o->clip_y0) y0 = o->clip_y0;
        if (y1 > o->clip_y1) y1 = o->clip_y1;
        o->dx0 = (int16_t)x0; o->dy0 = (int16_t)y0;
        o->dx1 = (int16_t)x1; o->dy1 = (int16_t)y1;
    }

    /* ---- 外发光：三条同心带，贴着球最亮 ---- */
#if ORB_ENABLE_GLOW
    if (o->p[P_GLOW] > 0.02f) {
        float gA = o->p[P_GLOW] * 12.0f;
        int g2 = R + (Rg - R) * 2 / 3;
        int g3 = R + (Rg - R) / 3;
        uint16_t gc = orb_grad(o->pal, 0.15f, 1.0f);
        orb_band(o, Rg, g2, (uint8_t)(gA * 0.28f + 0.5f), 0, 0, gc, 0, 0);
        orb_band(o, g2, g3, (uint8_t)(gA * 0.60f + 0.5f), 0, 0, gc, 0, 0);
        orb_band(o, g3, R,  (uint8_t)(gA * 1.00f + 0.5f), 0, 0, gc, 0, 0);
    }
#endif

    /* ---- 边缘光：细亮环 ---- */
    orb_band(o, R + R / 18 + 1, R + R / 40, 13, 1, 1.30f, 0, yf0, yf1);

    /* ---- 球体：径向暗角（同心带，亮度由外向内递增）---- */
    {
        int r1 = R - (R * 7)  / 100;
        int r2 = R - (R * 14) / 100;
        int r3 = R - (R * 22) / 100;
#if ORB_LAYERS >= 3
        orb_band(o, R,  r1, 32, 1, 0.80f, 0, yf0, yf1);
        orb_band(o, r1, r2, 32, 1, 0.87f, 0, yf0, yf1);
        orb_band(o, r2, r3, 32, 1, 0.94f, 0, yf0, yf1);
        orb_band(o, r3, 0,  32, 1, 1.00f, 0, yf0, yf1);
#else
        orb_band(o, R,  r2, 32, 1, 0.84f, 0, yf0, yf1);
        orb_band(o, r2, 0,  32, 1, 1.00f, 0, yf0, yf1);
#endif
    }

    /* ---- 高光：三层嵌套椭圆 ---- */
    {
        const float hx = -0.38f, hy = -0.46f;
        const float rr[3] = { 0.50f, 0.36f, 0.22f };
        const uint8_t aa[3] = { 3, 5, 8 };
        uint16_t white = orb_rgb565(255, 255, 255);
        for (int k = 0; k < 3; k++) {
            float rx = R * rr[k], ry = R * rr[k] * 0.80f;
            int ex = (int)(o->cx + R * hx), ey = (int)(o->cy + R * hy);
            int y0e = (int)(ey - ry), y1e = (int)(ey + ry);
            if (y0e < o->dy0) y0e = o->dy0;
            if (y1e > o->dy1) y1e = o->dy1;
            for (int y = y0e; y <= y1e; y++) {
                float dy = (float)(y - ey) / ry;
                float q = 1.0f - dy * dy;
                if (q <= 0) continue;
                int hw = (int)(rx * sqrtf(q));
                orb_out(o, y, ex - hw, ex + hw + 1, white, aa[k]);
            }
        }
    }

    /* ---- 思考态旋转弧（密排圆点连成弧线）---- */
    if (o->state == ORB_THINKING && o->p[P_SPEED] > 0.01f) {
        uint16_t ac = orb_grad(o->pal, 0.0f, 1.25f);
        int dot = R / 24; if (dot < 1) dot = 1;
        float base = o->spin;
        for (int s = 0; s < 3; s++) {
            float a0 = base * (1.0f + s * 0.28f) + s * 2.1f;
            int ar = (int)(R * (1.42f + s * 0.17f));
            for (int k = 0; k < 26; k++) {
                float a = a0 + 1.05f * k / 25.0f;
                int x = (int)(o->cx + cosf(a) * ar);
                int y = (int)(o->cy + sinf(a) * ar);
                orb_out(o, y, x - dot, x + dot + 1, ac, 18);
            }
        }
    }
}

void orb_dirty(const orb_t *o, int *x0, int *y0, int *x1, int *y1) {
    if (x0) *x0 = o->dx0;
    if (y0) *y0 = o->dy0;
    if (x1) *x1 = o->dx1;
    if (y1) *y1 = o->dy1;
}
