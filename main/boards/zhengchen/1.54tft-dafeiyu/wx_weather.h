/*
 * wx_weather.h — 和风天气 v1 数据层（后台任务）
 *
 * ⚠️ 2026-11-16 与原型 weather.c 的差异（原型两个 bug 已修）：
 *   1. 原型用 `https://host%s&key=` 硬拼，而 AQI 的 path 没有问号，
 *      拼出来 key 变成路径的一部分 → AQI 必失败。这里统一按 ? / & 判断。
 *   2. 原型要求响应里有 `"code":"200"` 字段，那是 v7 老接口的约定；
 *      v1 新接口没有 code 字段 → 原型一个接口都解析不出来。
 *      这里改成看 HTTP 200 + 关键字段是否存在。
 */
#pragma once
#include "wx_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 启动数据任务（只生效一次）。lat/lon 用十进制度，经度在前拼进 URL。
   首次拉取延时 6 秒等 WiFi/DNS 就绪；失败 30 秒重试，成功 10 分钟轮询。 */
void weather_init_cfg(const char *host, const char *key, double lat, double lon);

/* 立刻拉一次（手动刷新用） */
void weather_force_refresh(void);

/* 是否已拿到过有效数据 */
bool weather_is_valid(void);

#ifdef __cplusplus
}
#endif
