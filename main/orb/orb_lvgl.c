/**
 * orb_lvgl.c —— LVGL canvas 后端
 *
 * span 回调直接写 RGB565 缓冲，不经过 lv_draw 的图元管线，
 * 这是相比「用 lv_draw_arc / lv_draw_polygon 拼球」最大的性能来源。
 *
 * 每帧流程：
 *   1) 整块 canvas 填背景（纯 CPU，不推屏）
 *   2) orb_draw 只写球体脏矩形
 *   3) invalidate 上一帧与这一帧脏矩形的并集 —— SPI 只推这一块
 */
#include "orb_lvgl.h"
#include <string.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#ifndef LVGL_VERSION_MAJOR
#define LVGL_VERSION_MAJOR 8
#endif

/* ---------------- RGB565 alpha 混合（alpha: 0..32） ---------------- */
static inline uint16_t orb_blend565(uint16_t dst, uint16_t src, uint8_t a) {
    uint32_t d = (dst | ((uint32_t)dst << 16)) & 0x07E0F81Fu;
    uint32_t s = (src | ((uint32_t)src << 16)) & 0x07E0F81Fu;
    uint32_t r = ((((s - d) * (uint32_t)a) >> 5) + d) & 0x07E0F81Fu;
    return (uint16_t)(r | (r >> 16));
}

/* ---------------- span 回调 ---------------- */
static void orb_lv_span(orb_t *o, int y, int x0, int x1, uint16_t color, uint8_t alpha) {
    orb_lv_t *L = (orb_lv_t *)o->user;
    int yy = y - L->oy;
    if ((unsigned)yy >= (unsigned)L->h) return;
    int xa = x0 - L->ox, xb = x1 - L->ox;
    if (xa < 0) xa = 0;
    if (xb > L->w) xb = L->w;
    if (xb <= xa) return;

    uint16_t *p = L->buf + (size_t)yy * (size_t)L->w + (size_t)xa;
    int n = xb - xa;
    if (alpha >= 32) {
        while (n--) *p++ = color;
    } else {
        while (n--) { *p = orb_blend565(*p, color, alpha); p++; }
    }
}

/* ---------------- 内存 ---------------- */
static void *orb_alloc(size_t n) {
#if defined(ESP_PLATFORM) && ORB_LV_USE_SPIRAM
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) return p;
#endif
    return malloc(n);
}

/* ---------------- 创建 ---------------- */
orb_lv_t *orb_lv_create(lv_obj_t *parent, int cx, int cy, int r) {
    orb_lv_t *L = (orb_lv_t *)orb_alloc(sizeof(orb_lv_t));
    if (!L) return NULL;
    memset(L, 0, sizeof(*L));

    int rad = (int)(r * ORB_LV_CANVAS_MUL) + 3;
    int w = rad * 2 + 2, h = rad * 2 + 2;

    /* 限制在屏幕内 */
    int sw = lv_obj_get_width(lv_scr_act());
    int sh = lv_obj_get_height(lv_scr_act());
    if (sw <= 0) sw = 320;
    if (sh <= 0) sh = 240;
    if (w > sw) w = sw;
    if (h > sh) h = sh;

    L->w = (int16_t)w; L->h = (int16_t)h;
    L->ox = (int16_t)(cx - w / 2);
    L->oy = (int16_t)(cy - h / 2);
    if (L->ox < 0) L->ox = 0;
    if (L->oy < 0) L->oy = 0;
    if (L->ox + w > sw) L->ox = (int16_t)(sw - w);
    if (L->oy + h > sh) L->oy = (int16_t)(sh - h);

    L->buf = (uint16_t *)orb_alloc((size_t)w * (size_t)h * 2);
    if (!L->buf) { free(L); return NULL; }

    L->canvas = lv_canvas_create(parent);
    if (!L->canvas) { free(L->buf); free(L); return NULL; }
    lv_obj_set_pos(L->canvas, L->ox, L->oy);
    lv_obj_set_size(L->canvas, w, h);
    lv_obj_clear_flag(L->canvas, LV_OBJ_FLAG_SCROLLABLE);
    /* 不吃触摸事件，避免遮挡下层 UI */
    lv_obj_clear_flag(L->canvas, LV_OBJ_FLAG_CLICKABLE);
#if LVGL_VERSION_MAJOR >= 9
    lv_canvas_set_buffer(L->canvas, L->buf, w, h, LV_COLOR_FORMAT_RGB565);
#else
    lv_canvas_set_buffer(L->canvas, L->buf, w, h, LV_IMG_CF_TRUE_COLOR);
#endif

    L->bg = 0x0000;
    L->last_ms = lv_tick_get();

    orb_init(&L->core, cx, cy, r);
    orb_set_backend(&L->core, orb_lv_span, L);
    orb_set_clip(&L->core, L->ox, L->oy, L->ox + w - 1, L->oy + h - 1);

    /* 初始脏框设为整块 canvas */
    L->pdx0 = L->ox; L->pdy0 = L->oy;
    L->pdx1 = L->ox + w - 1; L->pdy1 = L->oy + h - 1;

    /* 首帧先清一次，避免出现未初始化的花屏 */
    for (int i = 0; i < w * h; i++) L->buf[i] = L->bg;
    lv_obj_invalidate(L->canvas);

    return L;
}

void orb_lv_destroy(orb_lv_t *L) {
    if (!L) return;
    if (L->timer) {
#if LVGL_VERSION_MAJOR >= 9
        lv_timer_delete(L->timer);
#else
        lv_timer_del(L->timer);
#endif
        L->timer = NULL;
    }
    if (L->canvas) {
#if LVGL_VERSION_MAJOR >= 9
        lv_obj_delete(L->canvas);
#else
        lv_obj_del(L->canvas);
#endif
    }
    if (L->buf) free(L->buf);
    free(L);
}

void orb_lv_set_bg(orb_lv_t *L, uint16_t rgb565) { if (L) L->bg = rgb565; }
orb_t *orb_lv_core(orb_lv_t *L) { return L ? &L->core : NULL; }
void orb_lv_set_state(orb_lv_t *L, orb_state_t s) { if (L) orb_set_state(&L->core, s); }

void orb_lv_feed_audio(orb_lv_t *L, float level, const float band[4]) {
    if (L) orb_feed_audio(&L->core, level, band);
}

/* ================= 说话态：球体外扩的声波环 =================
   直接往 RGB565 canvas 写，不经过 LVGL 图元管线。
   每帧 3 圈 × 约 100 点 × 2px ≈ 600 次混合，开销 0.1ms 级。 */
static inline uint16_t orb_lv_pal565(const float c[3]) {
    int r = (int)(c[0] + 0.5f), g = (int)(c[1] + 0.5f), b = (int)(c[2] + 0.5f);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return orb_rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static inline void orb_lv_put(orb_lv_t *L, int x, int y, uint16_t c, uint8_t a) {
    int xx = x - L->ox, yy = y - L->oy;
    if ((unsigned)xx >= (unsigned)L->w || (unsigned)yy >= (unsigned)L->h) return;
    uint16_t *p = L->buf + (size_t)yy * (size_t)L->w + (size_t)xx;
    *p = (a >= 32) ? c : orb_blend565(*p, c, a);
}

/* 中点画圆：像素落点精确，圆度是数学意义上的正圆（不是多边形近似） */
static void orb_lv_circle(orb_lv_t *L, int cx, int cy, int r, uint16_t c, uint8_t a) {
    if (r <= 0) return;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        orb_lv_put(L, cx + x, cy + y, c, a);
        orb_lv_put(L, cx + y, cy + x, c, a);
        orb_lv_put(L, cx - y, cy + x, c, a);
        orb_lv_put(L, cx - x, cy + y, c, a);
        orb_lv_put(L, cx - x, cy - y, c, a);
        orb_lv_put(L, cx - y, cy - x, c, a);
        orb_lv_put(L, cx + y, cy - x, c, a);
        orb_lv_put(L, cx + x, cy - y, c, a);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

/** 画扩散环，返回本帧画到的最大半径（0 = 没画），供脏框并集用。
    说话 = 3 圈由内向外扩散（放声）；
    聆听 = 2 圈由外向内收拢（收声，视觉上一眼就分得清）。 */
static int orb_lv_rings(orb_lv_t *L) {
    const int speaking = (L->core.state == ORB_SPEAKING);
    if (!speaking && L->core.state != ORB_LISTENING) return 0;

    const int cx = L->core.cx, cy = L->core.cy;
    const int r0 = L->core.r_base + 3;
    const int half = (L->w < L->h ? L->w : L->h) / 2;
    const int rmax = half - 3;
    if (rmax <= r0 + 6) return 0;

    const float t   = L->core.t_ms * 0.001f;
    const int   n   = speaking ? 3 : 2;
    const float spd = speaking ? 0.80f : 0.55f;
    const float ba  = speaking ? 26.0f : 20.0f;
    /* 音量决定亮度：安静时几乎看不见，说得响环就亮 —— 这就是"跟着声波走"。
       聆听时麦克风电平通常比播放低，给一个更高的底数，否则看着像没动。 */
    const float lv = speaking ? (0.18f + 0.82f * L->core.level)
                              : (0.40f + 0.60f * L->core.level);
    const uint16_t col = orb_lv_pal565(L->core.pal[1]);

    for (int k = 0; k < n; k++) {
        float ph = t * spd + (float)k / (float)n;
        ph -= (float)(int)ph;                       /* 相位 0..1 */
        const float pos = speaking ? ph : (1.0f - ph);
        const int rr = r0 + (int)((float)(rmax - r0) * pos);
        const float fade = speaking ? (1.0f - ph) : ph;
        int a = (int)(fade * ba * lv);
        if (a <= 0) continue;
        if (a > 30) a = 30;
        orb_lv_circle(L, cx, cy, rr, col, (uint8_t)a);
        orb_lv_circle(L, cx, cy, rr + 1, col, (uint8_t)(a / 2));   /* 2px 粗 */
    }
    return rmax;
}

/* ---------------- 渲染 ---------------- */
void orb_lv_render(orb_lv_t *L, uint32_t dt_ms) {
    if (!L) return;

    orb_update(&L->core, dt_ms);

    /* 整块填背景（纯 CPU 开销，约 0.3ms @168x168） */
    int n = (int)L->w * (int)L->h;
    uint16_t *p = L->buf;
    uint16_t bg = L->bg;
    if ((bg & 0xFF) == (bg >> 8)) {
        memset(p, bg & 0xFF, (size_t)n * 2);
    } else {
        for (int i = 0; i < n; i++) p[i] = bg;
    }

    orb_draw(&L->core);

    /* 说话/聆听都要有波纹：说话向外扩、聆听向内收（2026-11-02 补上聆听，
       之前只在 SPEAKING 画，所以用户觉得"聆听时完全不动"）。 */
    int ring_r = 0;
    if (L->core.state != ORB_IDLE) {
        ring_r = orb_lv_rings(L);
    }

    /* 本帧脏框 = 球体脏框 ∪ 声波环范围 */
    int nx0 = L->core.dx0, ny0 = L->core.dy0;
    int nx1 = L->core.dx1, ny1 = L->core.dy1;
    if (ring_r > 0) {
        const int ax = L->core.cx - ring_r, ay = L->core.cy - ring_r;
        const int bx = L->core.cx + ring_r, by = L->core.cy + ring_r;
        if (ax < nx0) nx0 = ax;
        if (ay < ny0) ny0 = ay;
        if (bx > nx1) nx1 = bx;
        if (by > ny1) ny1 = by;
    }

    /* 本帧与上一帧脏框取并集后再刷新，避免残留。
       注意 pdx 存的是「含声波环」的范围：这样说话刚停下的那一帧
       会把环扫过的区域一起刷掉，不会留下残影。 */
    int x0 = nx0, y0 = ny0, x1 = nx1, y1 = ny1;
    if (L->pdx0 < x0) x0 = L->pdx0;
    if (L->pdy0 < y0) y0 = L->pdy0;
    if (L->pdx1 > x1) x1 = L->pdx1;
    if (L->pdy1 > y1) y1 = L->pdy1;
    L->pdx0 = (int16_t)nx0; L->pdy0 = (int16_t)ny0;
    L->pdx1 = (int16_t)nx1; L->pdy1 = (int16_t)ny1;

    /* 夹到 canvas 范围 */
    int cx0 = L->ox, cy0 = L->oy, cx1 = L->ox + L->w - 1, cy1 = L->oy + L->h - 1;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x1 < x0 || y1 < y0) return;

#if ORB_LV_PARTIAL_REFRESH
    /* lv_obj_invalidate_area 收的是「屏幕绝对坐标」（lv_obj_invalidate 内部
       就是拿 lv_obj_get_coords 的结果直接调它），所以这里不能再减 ox/oy。
       x0..y1 已经在上面夹到 canvas 范围，直接传即可。 */
    lv_area_t ar;
    ar.x1 = (lv_coord_t)x0;
    ar.y1 = (lv_coord_t)y0;
    ar.x2 = (lv_coord_t)x1;
    ar.y2 = (lv_coord_t)y1;
    lv_obj_invalidate_area(L->canvas, &ar);
#else
    (void)x0; (void)y0; (void)x1; (void)y1;
    lv_obj_invalidate(L->canvas);
#endif
}

/* ---------------- 定时器 ---------------- */
static void orb_lv_timer(lv_timer_t *t) {
    orb_lv_t *L = (orb_lv_t *)lv_timer_get_user_data(t);
    uint32_t now = lv_tick_get();
    uint32_t dt = now - L->last_ms;
    L->last_ms = now;
    if (dt > 200) dt = 200;
    orb_lv_render(L, dt);
}

void orb_lv_set_fps(orb_lv_t *L, uint8_t fps) {
    if (!L) return;
    if (L->timer) {
#if LVGL_VERSION_MAJOR >= 9
        lv_timer_delete(L->timer);
#else
        lv_timer_del(L->timer);
#endif
        L->timer = NULL;
    }
    if (fps == 0) return;
    if (fps > 40) fps = 40;
    L->last_ms = lv_tick_get();
    L->timer = lv_timer_create(orb_lv_timer, 1000u / fps, L);
}
