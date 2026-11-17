/*
 * wx_scene.h — 三层场景合成引擎（移植自原型 scene.h）
 * 背景画在 RGB565 帧缓冲里，文字由 LVGL label 层叠加。
 *
 * ⚠️ 2026-11-16 移植改动：加 extern "C"。场景引擎是 C 文件，但被 .cc（显示层）
 *    调用；不加包裹会按 C++ 名字修饰去找符号，链接期直接报 undefined。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "wx_gfx.h"
#include "wx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化场景引擎（绑定帧缓冲，幂等）。w/h 必须与 LVGL canvas 一致（本板 240x240） */
void scene_init(color_t *buf, int w, int h);

/* 渲染一帧背景（不含文字），tick_ms 用于粒子动画 */
void scene_render_bg(uint32_t tick_ms);

/* 供文字层组装信息 */
typedef struct {
    char time_hm[8];      // "19:30"
    char date[24];        // "11月16日 周一"
    char temp[12];        // "15°"
    char feels[16];       // "体感13°"
    char desc[24];        // "晴"
    char range[24];       // "9° ~ 20°"
    char aqi[16];         // "AQI 优"
    const char *season;   // 春/夏/秋/冬
    bool dark_text;       // 文字用深色还是浅色（依背景明暗+昼夜）
} scene_text_t;

void scene_build_text(scene_text_t *out);

#ifdef __cplusplus
}
#endif
