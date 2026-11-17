/*
 * gfx.h — 轻量 RGB565 像素绘制层
 * 直接操作帧缓冲，不依赖 LVGL 绘制 API（LVGL 只负责最终刷屏和文字 label）
 */
#pragma once
#include <stdint.h>

typedef uint16_t color_t;   // RGB565

/* 颜色构造 */
color_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b);   // 8bit→RGB565
color_t gfx_hex(uint32_t rgb888);                    // 0xRRGGBB→RGB565
color_t gfx_mix(color_t a, color_t b, int t);        // t:0..255 混合
color_t gfx_alpha(color_t c, int a);                 // a:0..255 混黑色(压暗)

/* 基础图元（作用于全局帧缓冲） */
void gfx_set_buffer(color_t *buf, int w, int h);
void gfx_clear(color_t c);
void gfx_pixel(int x, int y, color_t c);
void gfx_fill_rect(int x, int y, int w, int h, color_t c);
void gfx_fill_rect_alpha(int x,int y,int w,int h,color_t c,int a);
void gfx_vgradient(int x, int y, int w, int h, color_t top, color_t bot);
void gfx_vgradient3(color_t c0, color_t c1, color_t c2); // 全屏三段渐变
void gfx_fill_circle(int cx, int cy, int r, color_t c);
void gfx_ring(int cx, int cy, int r, color_t c, int a);  // 半透明圆环
void gfx_line(int x0,int y0,int x1,int y1,color_t c);
void gfx_hline(int x0,int x1,int y,color_t c,int a);

/* 径向辉光（叠加，additive-ish） */
void gfx_radial_glow(int cx, int cy, int radius, color_t c, int strength);

/* 用每列顶边高度填充到指定底边（画山峦），heights 长度=屏宽 */
void gfx_fill_terrain(const int *top_y, int bottom_y, color_t c);
/* 山间雾带：水平渐变透明条 */
void gfx_mist_band(int y0, int y1, color_t c, int strength);
