/**
 * orb_audio.h —— 麦克风特征提取（Goertzel，零堆分配、无 FFT）
 *
 * 为什么不用 FFT：
 *   512 点实 FFT 在 S3 上约 2~4ms；而 4 个 Goertzel 只要 4×N 次乘加，
 *   N=256 时不到 0.05ms。对"驱动球体形变"这个用途，4 个频段完全够。
 */
#ifndef ORB_AUDIO_H
#define ORB_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 输出 */
    float level;        /* 0..1 总体音量（已平滑） */
    float band[4];      /* 0..1 四个频段能量，由低到高 */

    /* 配置 */
    float fs;           /* 采样率，默认 16000 */
    float gain;         /* 灵敏度，默认 1.0，声音小就调大 */
    float noise_gate;   /* 噪声门限（level 低于此值归零），默认 0.02 */

    /* 内部 */
    float dc;           /* 直流偏置跟踪 */
    float raw[4];       /* 未平滑的频段值 */
} orb_audio_t;

/** 初始化。sample_rate 一般 16000 */
void orb_audio_init(orb_audio_t *a, int sample_rate);

/**
 * 喂入单声道 int16 PCM。建议每次 128~512 样本（16kHz 下即 8~32ms）。
 * 内部会用「快起慢落」平滑，避免抖动。
 */
void orb_audio_feed(orb_audio_t *a, const int16_t *pcm, int n);

/* ================= 全局 PCM 电平（跨任务） =================
   音频播放/录音任务调用 orb_feed_pcm_global 丢入 PCM，
   显示任务每帧调用 orb_take_pcm_global 取走最新电平（无需加锁）。
   返回 1 表示这一帧有新数据，0 表示没有（调用方可自行衰减）。 */
void orb_feed_pcm_global(const int16_t *pcm, int n);
int  orb_take_pcm_global(float *level, float band[4]);

/** 2026-11-05 非消耗型读取：只看最新值，不推进 seq。
    take 接口一个 seq 只能被一个消费者拿走（频谱条和彩灯律动都要用电平，
    谁先调谁拿走，另一个就饿死），所以多消费者场景用这个。
    没有喂过数据时返回 0（调用方自行处理初始态）。 */
int orb_peek_audio_global(float *level, float band[4]);

#ifdef __cplusplus
}
#endif
#endif
