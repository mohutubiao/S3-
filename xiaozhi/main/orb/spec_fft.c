/**
 * spec_fft.c —— 音乐频谱：输出 PCM 环形缓冲 + 512 点 FFT + 对数分频
 *
 * 内存预算：环形缓冲 1KB(内部) + cos 表 1KB(内部) + FFT 交错工作区 4KB(PSRAM)
 */
#include "spec_fft.h"
#include <math.h>
#include <string.h>
#include <stdbool.h>
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

/* ── 环形缓冲（音频任务单写，LVGL 任务单读）── */
static int16_t s_ring[SPEC_FFT_N];
static volatile uint32_t s_total = 0;   /* 累计写入样本数（单调递增） */

void spec_fft_feed_pcm(const int16_t* pcm, int n) {
    if (pcm == NULL || n <= 0) return;
    /* 块拷贝代替逐样本取模（音频任务热路径）。
       ⚠️ n 可能大于环形缓冲（512）：音乐流块可达上千样本，
       必须按剩余空间分块拷贝 —— 2026-11-06 首版只写了
       「剩余一次拷完」两条路径，n>512 时第二段 memcpy 越界
       冲掉 .bss 相邻内存，播放音乐必崩（已修）。 */
    uint32_t pos = s_total % SPEC_FFT_N;
    int i = 0;
    while (i < n) {
        int blk = (int)SPEC_FFT_N - (int)pos;   /* 到缓冲尾的剩余空间 */
        if (blk > n - i) blk = n - i;
        memcpy(&s_ring[pos], pcm + i, (size_t)blk * sizeof(int16_t));
        i += blk;
        pos = (pos + (uint32_t)blk) % SPEC_FFT_N;
    }
    s_total += (uint32_t)n;
}

int spec_fft_read_latest(int16_t* dst, int n) {
    const uint32_t total = s_total;
    if (total < (uint32_t)n) return 0;
    const uint32_t start = total - (uint32_t)n;
    for (int i = 0; i < n; i++) {
        dst[i] = s_ring[(start + (uint32_t)i) % SPEC_FFT_N];
    }
    return 1;
}

/* ── FFT 工作区（懒分配）── */
static float* s_fft_buf = NULL;   /* 交错 re/im，N*2 floats，PSRAM */
static float* s_cos_tab = NULL;   /* cos 表 N/2 floats，内部 SRAM（热路径） */

static int spec_fft_workspace_init() {
    if (s_fft_buf != NULL) return 1;
#if defined(ESP_PLATFORM)
    s_fft_buf = (float*)heap_caps_calloc(SPEC_FFT_N * 2, sizeof(float),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_cos_tab = (float*)heap_caps_calloc(SPEC_FFT_N / 2, sizeof(float),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    s_fft_buf = (float*)calloc(SPEC_FFT_N * 2, sizeof(float));
    s_cos_tab = (float*)calloc(SPEC_FFT_N / 2, sizeof(float));
#endif
    if (s_fft_buf == NULL || s_cos_tab == NULL) return 0;
    for (int k = 0; k < SPEC_FFT_N / 2; k++) {
        s_cos_tab[k] = cosf(6.28318530718f * (float)k / (float)SPEC_FFT_N);
    }
    return 1;
}

/* 就地基-2 DIT FFT（n 必须是 2 的幂），buf 交错 [re0,im0,re1,im1,...] */
static void spec_fft_run(float* buf, int n) {
    /* 位反转重排 */
    for (uint32_t i = 1, j = 0; i < (uint32_t)n; i++) {
        uint32_t bit = (uint32_t)n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            const uint32_t a = i * 2, b = j * 2;
            float t = buf[a]; buf[a] = buf[b]; buf[b] = t;
            t = buf[a + 1]; buf[a + 1] = buf[b + 1]; buf[b + 1] = t;
        }
    }
    /* 蝶形：sin 用 cos 表移相 n/4 得到，省一张表 */
    const int half = n / 2;
    for (int len = 2; len <= n; len <<= 1) {
        const int step = n / len;
        const int half_len = len / 2;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half_len; k++) {
                const int ti = k * step;
                float* pa = buf + (i + k) * 2;
                float* pb = buf + (i + k + half_len) * 2;
                const float wr = s_cos_tab[ti];
                const float wi = -s_cos_tab[(ti + half) % half];
                const float xr = pb[0] * wr - pb[1] * wi;
                const float xi = pb[0] * wi + pb[1] * wr;
                pb[0] = pa[0] - xr;
                pb[1] = pa[1] - xi;
                pa[0] += xr;
                pa[1] += xi;
            }
        }
    }
}

/* ── 对数分频：16 段，60Hz ~ 10kHz；高频段给递增补偿 ── */
static float s_bin_lo[SPEC_BARS], s_bin_hi[SPEC_BARS], s_band_gain[SPEC_BARS];
static int s_edges_ready = 0;

static void spec_fft_init_edges() {
    if (s_edges_ready) return;
    const float f_lo = 60.0f, f_hi = 10000.0f;
    const float ratio = f_hi / f_lo;
    const float bin_hz = SPEC_FFT_FS / (float)SPEC_FFT_N;   /* 46.875 Hz */
    for (int i = 0; i <= SPEC_BARS; i++) {
        const float f = f_lo * powf(ratio, (float)i / (float)SPEC_BARS);
        const float b = f / bin_hz;
        if (i < SPEC_BARS) s_bin_lo[i] = b;
        if (i > 0) s_bin_hi[i - 1] = (b > s_bin_lo[i - 1] + 1.0f) ? b : s_bin_lo[i - 1] + 1.0f;
    }
    for (int i = 0; i < SPEC_BARS; i++) {
        const float f = (float)i / (float)(SPEC_BARS - 1);
        s_band_gain[i] = 1.0f + f * f * 3.5f + f * 0.5f;
    }
    s_edges_ready = 1;
}

void spec_fft_compute_bars(const int16_t* frame, float* bars) {
    if (!spec_fft_workspace_init()) return;
    spec_fft_init_edges();

    /* 转 float + Hann 加窗（窗口系数现算，省 2KB 表） */
    float sq = 0.0f;
    for (int i = 0; i < SPEC_FFT_N; i++) {
        const float w = 0.5f * (1.0f - cosf(6.28318530718f * (float)i / (float)(SPEC_FFT_N - 1)));
        const float v = (float)frame[i];
        sq += v * v;
        s_fft_buf[i * 2] = v * w;
        s_fft_buf[i * 2 + 1] = 0.0f;
    }
    const float rms = sqrtf(sq / (float)SPEC_FFT_N);

    spec_fft_run(s_fft_buf, SPEC_FFT_N);

    /* 底噪门限：正常音乐 RMS 上百，低于 30 视为静音，柱值清零由平滑自然落下 */
    const bool silent = rms < 30.0f;

    /* 每段取 bin 幅值最大值（比平均更跟拍）；幅值 ≈ A*N/4（含 Hann 增益） */
    for (int b = 0; b < SPEC_BARS; b++) {
        const int lo = (int)s_bin_lo[b];
        int hi = (int)s_bin_hi[b];
        if (hi <= lo) hi = lo + 1;
        if (hi > SPEC_FFT_N / 2) hi = SPEC_FFT_N / 2;
        float mx = 0.0f;
        for (int k = lo; k < hi && k < SPEC_FFT_N / 2; k++) {
            const float re = s_fft_buf[k * 2];
            const float im = s_fft_buf[k * 2 + 1];
            const float mag = sqrtf(re * re + im * im);
            if (mag > mx) mx = mag;
        }
        /* ⚠️ 2026-11-06 修复（用户反馈频谱不律动）：交接文档的归一化按
           浮点 PCM（±1.0）设计，但链路喂的是 int16 PCM —— 幅值 A 达几百
           上千，全部柱值被钳到 1.5 顶格，16 根柱满高不动。
           除以 32768 换算回满刻度 1.0 后柱值才落在 0~1.5 正常区间。 */
        float v = mx / ((float)(SPEC_FFT_N / 4) * 32768.0f) * s_band_gain[b];
        if (v > 1.5f) v = 1.5f;
        bars[b] = silent ? 0.0f : v;
    }
}
