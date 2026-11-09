/**
 * orb_audio.c —— Goertzel 频段能量 + RMS 音量
 * 4 个频点：200Hz(低频轰鸣) / 700Hz(基频) / 2000Hz(齿音) / 5000Hz(气息)
 * 快起慢落的包络跟踪，避免球体抖动。
 */
#include "orb_audio.h"
#include <math.h>
#include <string.h>
#if defined(ESP_PLATFORM)
#include "esp_log.h"
#endif

static const float BAND_HZ[4] = { 200.0f, 700.0f, 2000.0f, 5000.0f };
/* 各频段的归一化系数：高频天然能量低，需要更大增益 */
static const float BAND_K[4] = { 5.0f, 7.0f, 16.0f, 40.0f };

void orb_audio_init(orb_audio_t *a, int sample_rate) {
    memset(a, 0, sizeof(*a));
    a->fs = sample_rate > 0 ? (float)sample_rate : 16000.0f;
    a->gain = 1.0f;
    a->noise_gate = 0.02f;
}

/** Goertzel 单点 DFT 幅值 */
static float orb_goertzel(const int16_t *x, int n, float hz, float fs) {
    float w = 6.28318530718f * hz / fs;
    float coeff = 2.0f * cosf(w);
    float s1 = 0.0f, s2 = 0.0f;
    for (int i = 0; i < n; i++) {
        float s0 = (float)x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    float p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    if (p < 0) p = 0;
    return sqrtf(p) / (float)n;
}

static inline float orb_smooth(float cur, float tgt) {
    /* 快起慢落 */
    return tgt > cur ? cur + (tgt - cur) * 0.55f
                     : cur + (tgt - cur) * 0.12f;
}

/* ================= 全局 PCM 电平（跨任务安全） =================
   为什么不在音频任务里直接调 orb_feed_audio：
   orb_t 的字段由 LVGL 任务在渲染时读，跨任务写浮点可能读到撕裂的值。
   这里只把结果量化成 int（Xtensa 上 32 位读写是原子的），
   由显示层在自己的 tick 里取走，完全不用加锁、不阻塞音频线程。 */
static orb_audio_t g_sink;
static int          g_sink_ready = 0;
static volatile int g_sink_level = 0;                 /* 0..1000 */
static volatile int g_sink_band[4] = {0, 0, 0, 0};
static volatile int g_sink_seq = 0;                   /* 有新数据就 +1 */

void orb_feed_pcm_global(const int16_t *pcm, int n) {
    if (!pcm || n < 16) return;
    if (!g_sink_ready) {
        orb_audio_init(&g_sink, 16000);
        g_sink.gain = 1.8f;          /* 小智的 PCM 偏轻，给点增益才看得出起伏 */
        g_sink.noise_gate = 0.015f;
        g_sink_ready = 1;
    }
    orb_audio_feed(&g_sink, pcm, n);

    /* 只打一次：确认音频链路真的挂上了（某些板子可能绕过 OutputData） */
    static int logged = 0;
    if (!logged) {
        logged = 1;
#if defined(ESP_PLATFORM)
        ESP_LOGI("orb_audio", "PCM hook active: n=%d level=%d/1000", n,
                 (int)(g_sink.level * 1000.0f));
#endif
    }

    int lv = (int)(g_sink.level * 1000.0f + 0.5f);
    if (lv > 1000) lv = 1000;
    if (lv < 0) lv = 0;
    /* 回落软化：麦克风和播放两条链路会交替上报，若其中一路是静音，
       直接覆盖会让电平被 0 打断、声波闪烁。下降时只走 1/4 步。 */
    if (lv < g_sink_level) {
        lv = g_sink_level - (g_sink_level - lv + 3) / 4;
    }
    g_sink_level = lv;
    for (int i = 0; i < 4; i++) {
        int v = (int)(g_sink.band[i] * 1000.0f + 0.5f);
        if (v > 1000) v = 1000;
        if (v < 0) v = 0;
        g_sink_band[i] = v;
    }
    g_sink_seq++;
}

int orb_take_pcm_global(float *level, float band[4]) {
    static int last_seq = 0;
    const int s = g_sink_seq;
    if (s == last_seq) return 0;               /* 没有新数据 */
    last_seq = s;
    if (level) *level = (float)g_sink_level / 1000.0f;
    if (band) {
        for (int i = 0; i < 4; i++) band[i] = (float)g_sink_band[i] / 1000.0f;
    }
    return 1;
}

/* 2026-11-05 非消耗型读取：频谱条和彩灯律动都要电平，
   take 的 seq 是全局单份，两个消费者会互相抢数据，这里只读不推进。 */
int orb_peek_audio_global(float *level, float band[4]) {
    if (!g_sink_ready) return 0;
    if (level) *level = (float)g_sink_level / 1000.0f;
    if (band) {
        for (int i = 0; i < 4; i++) band[i] = (float)g_sink_band[i] / 1000.0f;
    }
    return 1;
}

void orb_audio_feed(orb_audio_t *a, const int16_t *pcm, int n) {
    if (!pcm || n < 16) return;

    /* 直流偏置跟踪（去掉麦克风零漂） */
    float sum = 0, sq = 0;
    for (int i = 0; i < n; i++) sum += (float)pcm[i];
    float mean = sum / (float)n;
    a->dc += (mean - a->dc) * 0.05f;

    for (int i = 0; i < n; i++) {
        float v = (float)pcm[i] - a->dc;
        sq += v * v;
    }
    float rms = sqrtf(sq / (float)n);

    /* 归一化：16bit 下正常说话 RMS 约 300~3000 */
    float lv = rms / 2200.0f * a->gain;
    if (lv > 1.0f) lv = 1.0f;
    if (lv < a->noise_gate) lv = 0.0f;
    a->level = orb_smooth(a->level, lv);

    for (int b = 0; b < 4; b++) {
        float m = orb_goertzel(pcm, n, BAND_HZ[b], a->fs);
        float v = m * BAND_K[b] * a->gain;
        if (v > 1.0f) v = 1.0f;
        a->raw[b] = v;
        a->band[b] = orb_smooth(a->band[b], v);
    }
}
