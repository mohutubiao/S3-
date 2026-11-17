/*
 * wx_types.h — 天气数据层的对外契约（移植自原型 weather.h，只留类型）
 * 场景渲染只读这个结构体，不关心 HTTP/JSON 怎么来的。
 * ⚠️ 2026-11-16 移植改动：
 *   1. 原型里 weather_init(void) 没参数，这里挪到 wx_weather.h 且带配置参数；
 *   2. 全部函数加 extern "C"，因为本文件同时被 .c（场景引擎）和 .cc（显示层）包含。
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 天气归类：8 类（和风 icon 码归并） */
typedef enum {
    WX_CLEAR = 0,   // 晴 100/150
    WX_CLOUDY,      // 云 101-104/151-153
    WX_RAIN,        // 雨 300-318/399（不含雷）
    WX_SNOW,        // 雪 400-410/499
    WX_THUNDER,     // 雷 302-304
    WX_FOG,         // 雾/霾 500-502/509-515
    WX_DUST,        // 沙尘 503-504/507-508
    WX_SPECIAL,     // 极端冷/热 900/901
    WX_COUNT
} wx_category_t;

/* 月相 8 档 */
typedef enum {
    MOON_NEW = 0, MOON_WAX_CRESCENT, MOON_FIRST_Q, MOON_WAX_GIBBOUS,
    MOON_FULL, MOON_WAN_GIBBOUS, MOON_LAST_Q, MOON_WAN_CRESCENT
} moon_phase_t;

/* 昼夜 4 态 */
typedef enum {
    PHASE_DAWN = 0, PHASE_DAY, PHASE_DUSK, PHASE_NIGHT
} day_phase_t;

/* 季节 4 态 */
typedef enum {
    SEASON_SPRING = 0, SEASON_SUMMER, SEASON_AUTUMN, SEASON_WINTER
} season_t;

/* 从 API 拉取的原始数据（单位已归一） */
typedef struct {
    float   temp;          // 当前温度 °C
    float   feels_like;    // 体感 °C
    int     icon_code;     // 和风原始 icon 码
    wx_category_t category;// 归类后的 8 类
    char    text[24];      // 天气现象文字（UTF-8）
    int     humidity;      // 相对湿度 0-100
    int     wind_scale;    // 蒲福风级
    float   vis_km;        // 能见度 km

    float   temp_max;      // 今日最高
    float   temp_min;      // 今日最低
    int32_t sunrise;       // 日出 epoch 秒（UTC）
    int32_t sunset;        // 日落 epoch 秒（UTC）
    int32_t solar_noon;    // 太阳正午 epoch 秒
    int     uv_index;      // 紫外线指数
    moon_phase_t moon;     // 月相

    int     pop;           // 当前小时降水概率 0-100
    int     aqi;           // 空气质量指数
    char    aqi_category[16]; // 优/良/轻度污染…（UTF-8）
    bool    has_alert;     // 是否有预警

    bool    valid;         // 数据是否有效（false=用缓存/默认值）
    int64_t updated_at;    // 更新时刻 epoch 秒
} weather_data_t;

/* 全局唯一数据实例，渲染层只读它 */
extern weather_data_t g_weather;

/* 派生量：设备端算，不用 API */
day_phase_t weather_day_phase(int64_t now_ts);   // 昼夜 4 态（黎明/昼/黄昏/夜）
season_t    weather_season(int month);            // 月份→季节
float       weather_sun_progress(int64_t now_ts); // 0..1 太阳弧线位置

/* 数据层线程安全读取快照（渲染层每帧调，内部是临界区拷贝） */
void weather_get(weather_data_t *out);

/* icon 码归类（渲染层也可能用到） */
wx_category_t weather_icon_to_category(int code);

#ifdef __cplusplus
}
#endif
