/*
 * wx_weather.c — 和风天气 v1 数据层实现
 *
 * 三个请求：
 *   /weather/v1/current/{lat}/{lon}      实况（温度/体感/现象/风/能见度/UV）
 *   /weather/v1/daily/{lat}/{lon}?days=1 今日高低温 + astro（日出日落/月相）
 *   /airquality/v1/current/{lat}/{lon}   AQI
 * 全部带 localTime=true，返回时间就是当地墙钟时间（+08:00 形式）。
 *
 * 内存：响应缓冲 8KB 强制走 PSRAM（内部 SRAM 很紧），cJSON 用默认分配。
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include <cJSON.h>
/* ROM 里的 miniz（bootloader 解压镜像用的那份），只取 tinfl 单次解压接口。
   ⚠️ 和风天气**无条件**回 gzip（Content-Encoding: gzip，请求头带
   Accept-Encoding: identity 也没用），不解压的话 cJSON 只能看到二进制。 */
#include "miniz.h"

#include "wx_weather.h"

/* 不用 timegm/mktime：前者本工具链链接不到，后者按设备本地时区解释，
   而我们手里是自带 +08:00 偏移的字符串。自己算 UTC epoch 最准。 */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static const char *TAG = "wxweather";

/* 默认值：数据没拉到之前屏上也不至于是一堆 0 */
weather_data_t g_weather = {
    .temp = 0.0f,
    .feels_like = 0.0f,
    .icon_code = 100,
    .category = WX_CLEAR,
    .text = "--",
    .humidity = 50,
    .wind_scale = 0,
    .vis_km = 10.0f,
    .temp_max = 0.0f,
    .temp_min = 0.0f,
    .sunrise = 0,
    .sunset = 0,
    .solar_noon = 0,
    .uv_index = 0,
    .moon = MOON_FULL,
    .pop = 0,
    .aqi = 0,
    .aqi_category = "--",
    .has_alert = false,
    .valid = false,
    .updated_at = 0,
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static char s_host[64];
static char s_key[48];
static double s_lat = 0.0;
static double s_lon = 0.0;
static bool s_started = false;

#define WX_RESP_MAX 8192

static void wx_store(const weather_data_t *d) {
    taskENTER_CRITICAL(&s_lock);
    g_weather = *d;
    taskEXIT_CRITICAL(&s_lock);
}

void weather_get(weather_data_t *out) {
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = g_weather;
    taskEXIT_CRITICAL(&s_lock);
}

bool weather_is_valid(void) {
    bool v;
    taskENTER_CRITICAL(&s_lock);
    v = g_weather.valid;
    taskEXIT_CRITICAL(&s_lock);
    return v;
}

/* ────────────────────────── HTTP ────────────────────────── */

typedef struct {
    char *buf;
    int   len;
    bool  gzip;
} resp_ctx_t;

static esp_err_t http_evt(esp_http_client_event_t *e) {
    resp_ctx_t *c = (resp_ctx_t *)e->user_data;
    if (c == NULL) {
        return ESP_OK;
    }
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key != NULL &&
        e->header_value != NULL) {
        if (strcasecmp(e->header_key, "Content-Encoding") == 0 &&
            strncasecmp(e->header_value, "gzip", 4) == 0) {
            c->gzip = true;
        }
        return ESP_OK;
    }
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
        int room = WX_RESP_MAX - 1 - c->len;
        int n = (e->data_len < room) ? e->data_len : room;
        if (n > 0) {
            memcpy(c->buf + c->len, e->data, n);
            c->len += n;
            c->buf[c->len] = '\0';
        }
    }
    return ESP_OK;
}

/* gzip 解到 dst（尾部 8 字节是 CRC32 + ISIZE，要剥掉再交给 tinfl） */
static bool gunzip_to(const uint8_t *src, int src_len, char *dst, int dst_cap, int *out_len) {
    if (src == NULL || src_len < 18 || src[0] != 0x1f || src[1] != 0x8b) {
        return false;
    }
    const uint8_t flg = src[3];
    int off = 10;
    if (flg & 0x04) {                                   /* FEXTRA */
        if (off + 2 > src_len) return false;
        const int xlen = src[off] | (src[off + 1] << 8);
        off += 2 + xlen;
    }
    if (flg & 0x08) {                                   /* FNAME */
        while (off < src_len && src[off] != 0) off++;
        off++;
    }
    if (flg & 0x10) {                                   /* FCOMMENT */
        while (off < src_len && src[off] != 0) off++;
        off++;
    }
    if (flg & 0x02) off += 2;                           /* FHCRC */
    int in_len = src_len - off - 8;
    if (off < 0 || in_len <= 0) {
        return false;
    }
    size_t n = tinfl_decompress_mem_to_mem(dst, (size_t)(dst_cap - 1),
                                           src + off, (size_t)in_len,
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        return false;
    }
    dst[n] = '\0';
    *out_len = (int)n;
    return true;
}

/* path 里不要自带 query —— key 和 localTime 统一在这里拼，避免 '?&' 混用 */
static cJSON *wx_get(const char *path) {
    char url[192];
    snprintf(url, sizeof(url), "https://%s%s%ckey=%s&localTime=true",
             s_host, path, (strchr(path, '?') != NULL) ? '&' : '?', s_key);

    char *buf = (char *)heap_caps_malloc(WX_RESP_MAX, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        ESP_LOGE(TAG, "PSRAM 分配失败");
        return NULL;
    }
    buf[0] = '\0';
    resp_ctx_t ctx = {buf, 0, false};

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_evt,
        .user_data = &ctx,
        .timeout_ms = 12000,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (h == NULL) {
        free(buf);
        return NULL;
    }

    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);

    cJSON *root = NULL;
    /* v1 接口没有 "code" 字段，只看 HTTP 状态码 + 能不能解析出 JSON */
    if (err == ESP_OK && status == 200 && ctx.len > 0) {
        const char *json = buf;
        char *plain = NULL;
        if (ctx.gzip) {
            plain = (char *)heap_caps_malloc(WX_RESP_MAX, MALLOC_CAP_SPIRAM);
            int plain_len = 0;
            if (plain != NULL &&
                gunzip_to((const uint8_t *)buf, ctx.len, plain, WX_RESP_MAX, &plain_len)) {
                json = plain;
                ESP_LOGD(TAG, "%s gzip %d -> %d 字节", path, ctx.len, plain_len);
            } else {
                ESP_LOGW(TAG, "%s gzip 解压失败（原始 %d 字节）", path, ctx.len);
                json = NULL;
            }
        }
        if (json != NULL) {
            root = cJSON_Parse(json);
            if (root == NULL) {
                ESP_LOGW(TAG, "%s JSON 解析失败，前 80 字节: %.80s", path, json);
            }
        }
        if (plain != NULL) {
            free(plain);
        }
    } else {
        ESP_LOGW(TAG, "%s 请求失败 err=%d status=%d len=%d", path, (int)err, status, ctx.len);
    }
    free(buf);
    return root;
}

/* ────────────────────────── JSON 取值 ────────────────────────── */

static cJSON *jobj(cJSON *o, const char *k) {
    if (o == NULL) {
        return NULL;
    }
    return cJSON_GetObjectItem(o, k);
}

static bool jnum(cJSON *o, const char *k, double *out) {
    cJSON *v = jobj(o, k);
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    if (out != NULL) {
        *out = v->valuedouble;
    }
    return true;
}

static const char *jstr(cJSON *o, const char *k) {
    cJSON *v = jobj(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* "2026-11-16T06:29+08:00" → UTC epoch 秒 */
static int32_t iso_to_epoch(const char *s) {
    if (s == NULL) {
        return 0;
    }
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    int n = sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec);
    if (n < 5) {
        return 0;
    }
    int64_t utc = days_from_civil(Y, (unsigned)M, (unsigned)D) * 86400 +
                  (int64_t)h * 3600 + (int64_t)m * 60 +
                  (int64_t)((n >= 6) ? sec : 0);

    int off = 0;
    const char *t = strchr(s, 'T');
    if (t != NULL) {
        const char *q = t + 1;
        while (*q != '\0' && (*q == ':' || (*q >= '0' && *q <= '9'))) {
            q++;
        }
        if ((*q == '+' || *q == '-') && strlen(q) >= 6) {
            int oh = 0, om = 0;
            if (sscanf(q + 1, "%d:%d", &oh, &om) == 2) {
                off = (oh * 3600 + om * 60) * (*q == '-' ? -1 : 1);
            }
        }
    }
    return (int32_t)(utc - off);
}

wx_category_t weather_icon_to_category(int code) {
    if (code >= 302 && code <= 304) return WX_THUNDER;
    if (code >= 300 && code <= 318) return WX_RAIN;
    if (code == 399) return WX_RAIN;
    if (code >= 400 && code <= 410) return WX_SNOW;
    if (code == 499) return WX_SNOW;
    if ((code >= 500 && code <= 502) || (code >= 509 && code <= 515)) return WX_FOG;
    if (code >= 503 && code <= 508) return WX_DUST;
    if (code == 900 || code == 901) return WX_SPECIAL;
    if (code == 100 || code == 150) return WX_CLEAR;
    if ((code >= 101 && code <= 104) || (code >= 151 && code <= 153)) return WX_CLOUDY;
    return WX_CLEAR;
}

static moon_phase_t moon_from_str(const char *s) {
    if (s == NULL) return MOON_FULL;
    if (strcmp(s, "new-moon") == 0) return MOON_NEW;
    if (strcmp(s, "waxing-crescent") == 0) return MOON_WAX_CRESCENT;
    if (strcmp(s, "first-quarter") == 0) return MOON_FIRST_Q;
    if (strcmp(s, "waxing-gibbous") == 0) return MOON_WAX_GIBBOUS;
    if (strcmp(s, "full-moon") == 0) return MOON_FULL;
    if (strcmp(s, "waning-gibbous") == 0) return MOON_WAN_GIBBOUS;
    if (strcmp(s, "last-quarter") == 0) return MOON_LAST_Q;
    if (strcmp(s, "waning-crescent") == 0) return MOON_WAN_CRESCENT;
    return MOON_FULL;
}

/* ────────────────────────── 三个解析 ────────────────────────── */

static void parse_current(cJSON *root) {
    weather_data_t d;
    weather_get(&d);

    cJSON *cond = jobj(root, "condition");
    const char *text = jstr(cond, "text");
    const char *code = jstr(cond, "code");
    if (text != NULL) {
        snprintf(d.text, sizeof(d.text), "%s", text);
    }
    if (code != NULL) {
        d.icon_code = atoi(code);
    }
    d.category = weather_icon_to_category(d.icon_code);

    double v = 0;
    if (jnum(jobj(root, "temperature"), "value", &v)) d.temp = (float)v;
    if (jnum(jobj(root, "feelsLike"), "value", &v))   d.feels_like = (float)v;
    if (jnum(root, "humidity", &v))                   d.humidity = (int)lround(v * 100.0);
    if (jnum(jobj(root, "wind"), "scale", &v))        d.wind_scale = (int)lround(v);
    if (jnum(jobj(root, "visibility"), "value", &v))  d.vis_km = (float)(v / 1000.0);
    if (jnum(root, "uvIndex", &v))                    d.uv_index = (int)lround(v);

    d.valid = true;
    d.updated_at = (int64_t)time(NULL);
    wx_store(&d);
    ESP_LOGI(TAG, "实况 %.1f°C %s 湿度%d%% 风%d级 能见度%.1fkm",
             d.temp, d.text, d.humidity, d.wind_scale, d.vis_km);
}

static void parse_daily(cJSON *root) {
    cJSON *d0 = cJSON_GetArrayItem(jobj(root, "days"), 0);
    if (d0 == NULL) {
        return;
    }
    weather_data_t d;
    weather_get(&d);

    double v = 0;
    if (jnum(jobj(d0, "temperatureMax"), "value", &v)) d.temp_max = (float)v;
    if (jnum(jobj(d0, "temperatureMin"), "value", &v)) d.temp_min = (float)v;
    if (jnum(d0, "uvIndexMax", &v))                    d.uv_index = (int)lround(v);

    cJSON *astro = jobj(d0, "astro");
    if (astro != NULL) {
        const char *sr = jstr(astro, "sunrise");
        const char *ss = jstr(astro, "sunset");
        const char *sn = jstr(astro, "solarNoon");
        if (sr != NULL) d.sunrise = iso_to_epoch(sr);
        if (ss != NULL) d.sunset = iso_to_epoch(ss);
        if (sn != NULL) d.solar_noon = iso_to_epoch(sn);
        d.moon = moon_from_str(jstr(astro, "moonPhase"));
    }

    wx_store(&d);
    ESP_LOGI(TAG, "今日 %.0f~%.0f°C 日出%ld 日落%ld 月相%d",
             d.temp_min, d.temp_max, (long)d.sunrise, (long)d.sunset, (int)d.moon);
}

static void parse_air(cJSON *root) {
    cJSON *i0 = cJSON_GetArrayItem(jobj(root, "indexes"), 0);
    if (i0 == NULL) {
        return;
    }
    weather_data_t d;
    weather_get(&d);

    double v = 0;
    if (jnum(i0, "aqi", &v)) {
        d.aqi = (int)lround(v);
    }
    const char *cat = jstr(i0, "category");
    if (cat != NULL) {
        snprintf(d.aqi_category, sizeof(d.aqi_category), "%s", cat);
    }
    wx_store(&d);
    ESP_LOGI(TAG, "AQI %d %s", d.aqi, d.aqi_category);
}

/* ────────────────────────── 派生量 ────────────────────────── */

season_t weather_season(int month) {
    if (month >= 3 && month <= 5)  return SEASON_SPRING;
    if (month >= 6 && month <= 8)  return SEASON_SUMMER;
    if (month >= 9 && month <= 11) return SEASON_AUTUMN;
    return SEASON_WINTER;
}

day_phase_t weather_day_phase(int64_t now_ts) {
    weather_data_t d;
    weather_get(&d);

    /* 没拿到日出日落就按本地时间兜底（6~18 点算白天） */
    if (d.sunrise <= 0 || d.sunset <= d.sunrise) {
        time_t t = (time_t)now_ts;
        struct tm *ti = localtime(&t);
        int h = (ti != NULL) ? ti->tm_hour : 12;
        return (h >= 18 || h < 6) ? PHASE_NIGHT : PHASE_DAY;
    }

    const int64_t rise = d.sunrise;
    const int64_t set = d.sunset;
    if (now_ts < rise - 1800) return PHASE_NIGHT;   /* 日出前 30 分钟之前：夜 */
    if (now_ts < rise)        return PHASE_DAWN;    /* 黎明 */
    if (now_ts < set)         return PHASE_DAY;     /* 白天 */
    if (now_ts < set + 1800)  return PHASE_DUSK;    /* 黄昏 */
    return PHASE_NIGHT;
}

float weather_sun_progress(int64_t now_ts) {
    weather_data_t d;
    weather_get(&d);
    if (d.sunrise <= 0 || d.sunset <= d.sunrise) {
        return 0.5f;
    }
    float p = (float)(now_ts - d.sunrise) / (float)(d.sunset - d.sunrise);
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return p;
}

/* ────────────────────────── 任务 ────────────────────────── */

static bool fetch_all(void) {
    char path[96];
    bool any = false;

    snprintf(path, sizeof(path), "/weather/v1/current/%.2f/%.2f", s_lat, s_lon);
    cJSON *r = wx_get(path);
    if (r != NULL) {
        parse_current(r);
        cJSON_Delete(r);
        any = true;
    }

    snprintf(path, sizeof(path), "/weather/v1/daily/%.2f/%.2f", s_lat, s_lon);
    r = wx_get(path);
    if (r != NULL) {
        parse_daily(r);
        cJSON_Delete(r);
        any = true;
    }

    snprintf(path, sizeof(path), "/airquality/v1/current/%.2f/%.2f", s_lat, s_lon);
    r = wx_get(path);
    if (r != NULL) {
        parse_air(r);
        cJSON_Delete(r);
    }

    return any;
}

static void weather_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(6000));   /* 等 WiFi 连上 + DNS 就绪 */
    for (;;) {
        bool ok = fetch_all();
        /* 每次跑完看剩余栈：低于 1.5KB 就说明还得加，别等到又崩在 time() 里 */
        const unsigned left = (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
        if (!ok) {
            ESP_LOGW(TAG, "本轮全部失败，30 秒后重试（栈余量 %u）", left);
        } else {
            ESP_LOGI(TAG, "本轮完成（栈余量 %u）", left);
        }
        vTaskDelay(pdMS_TO_TICKS(ok ? 600000 : 30000));
    }
}

void weather_init_cfg(const char *host, const char *key, double lat, double lon) {
    if (s_started || host == NULL || key == NULL) {
        return;
    }
    snprintf(s_host, sizeof(s_host), "%s", host);
    snprintf(s_key, sizeof(s_key), "%s", key);
    s_lat = lat;
    s_lon = lon;
    s_started = true;

    /* ⚠️ 2026-11-16 崩溃教训：原来给 8192，实测栈溢出 ——
       表现不是"栈溢出"日志，而是 newlib 内部锁对象被踩坏，
       崩在 time() → _gettimeofday_r → lock_release → xQueueGenericSend
       → uxListRemove（EXCVADDR=0x8）。TLS 握手 + cJSON 递归解析
       实际要 10KB 上下，这里给 14336 留足余量。 */
    BaseType_t r = xTaskCreate(weather_task, "wx_weather", 14336, NULL, 3, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "创建天气任务失败");
        s_started = false;
        return;
    }
    ESP_LOGI(TAG, "天气任务已启动 host=%s loc=%.2f,%.2f", s_host, s_lat, s_lon);
}

void weather_force_refresh(void) {
    /* 任务本来 10 分钟一轮；这里只把"下一轮"提前 —— 简单做法是直接同步拉一次。
       注意：会在调用者线程阻塞最多 12 秒，别在 LVGL 线程调。 */
    fetch_all();
}
