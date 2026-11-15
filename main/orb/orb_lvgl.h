/**
 * orb_lvgl.h —— LVGL 适配层
 *
 * 思路：球体画在一块「只比球稍大」的 canvas 上。
 *   - 背景/文字等 UI 由 LVGL 正常绘制，不参与每帧重绘
 *   - 每帧只 invalidate 这块 canvas，SPI 只推这块区域
 *   - 240x320 屏、R=62、开 glow 时 canvas 约 186x186，
 *     传输量是全屏的 45%，帧率从 ~14fps 提到 ~28fps
 */
#ifndef ORB_LVGL_H
#define ORB_LVGL_H

#include "orb.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 1=只刷新脏矩形，0=整块 canvas 都刷。
 * 这是整个优化里最值钱的一项：开 1 时 SPI 只推约 35KB/帧，
 * 开 0 时整块 canvas（约 110KB）全推，帧率直接掉一半以上。
 * 坐标系已按 lv_obj_invalidate_area 的语义修正（绝对坐标）。
 */
#ifndef ORB_LV_PARTIAL_REFRESH
#define ORB_LV_PARTIAL_REFRESH   1
#endif

/** 1=canvas 缓冲放 PSRAM（省内部 RAM，S3 通常有 8MB PSRAM） */
#ifndef ORB_LV_USE_SPIRAM
#define ORB_LV_USE_SPIRAM        1
#endif

/**
 * canvas 半径 = r_base * ORB_LV_CANVAS_MUL。
 * 必须能容纳「最大呼吸 + 最大形变 + 外发光」，否则球会被裁掉。
 * 实测最大脏框边长（说话态被音频顶到最大时）：
 *   开 glow：≈ 5.3 × r_base   → 需 MUL ≥ 2.65
 *   关 glow：≈ 4.0 × r_base   → 需 MUL ≥ 2.00
 * 320×240 屏上 r_base 取 40~44 时，canvas 约 214~235，正好塞得下。
 */
#ifndef ORB_LV_CANVAS_MUL
#if ORB_ENABLE_GLOW
#define ORB_LV_CANVAS_MUL        2.65f
#else
#define ORB_LV_CANVAS_MUL        2.05f
#endif
#endif

typedef struct orb_lv_s {
    orb_t      core;
    lv_obj_t  *canvas;
    uint16_t  *buf;        /* canvas 像素缓冲，RGB565，w*h */
    int16_t    w, h;       /* canvas 尺寸 */
    int16_t    ox, oy;     /* canvas 左上角在屏幕上的位置 */
    lv_timer_t *timer;
    uint32_t   last_ms;
    uint16_t   bg;
    int16_t    pdx0, pdy0, pdx1, pdy1;   /* 上一帧脏框，用于求并集 */
} orb_lv_t;

/**
 * 创建。parent 一般为 lv_scr_act()；cx,cy 为球体中心（屏幕坐标）；r 为半径。
 * 返回 NULL 表示内存不足。
 */
orb_lv_t *orb_lv_create(lv_obj_t *parent, int cx, int cy, int r);

void orb_lv_destroy(orb_lv_t *L);

/** 设置背景色（RGB565），canvas 每帧会用它清底 */
void orb_lv_set_bg(orb_lv_t *L, uint16_t rgb565);

/** 启动自动刷新定时器。fps=20 推荐；传 0 则关闭定时器，由你手动调 orb_lv_render */
void orb_lv_set_fps(orb_lv_t *L, uint8_t fps);

/** 手动渲染一帧（dt_ms 为距上次调用的毫秒数） */
void orb_lv_render(orb_lv_t *L, uint32_t dt_ms);

void orb_lv_set_state(orb_lv_t *L, orb_state_t s);
void orb_lv_feed_audio(orb_lv_t *L, float level, const float band[4]);
orb_t *orb_lv_core(orb_lv_t *L);

#ifdef __cplusplus
}
#endif
#endif
