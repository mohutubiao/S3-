/**
 * spec_fft.h —— 音乐频谱专用：输出 PCM 环形缓冲 + FFT 对数分频
 *
 * 音频任务只 memcpy 进环形缓冲；FFT 在显示任务 tick 里做（25fps 节流）。
 * 单写单读，volatile 序号同步不加锁；环绕撕裂对频谱可容忍。
 */
#ifndef _SPEC_FFT_H_
#define _SPEC_FFT_H_
#include <stdint.h>

#define SPEC_FFT_N      512      /* FFT 点数（2 的幂） */
#define SPEC_FFT_FS     24000.0f /* 输出采样率（audio_service 重采样到 codec output_rate） */
#define SPEC_BARS       16       /* 频谱柱数 */

#ifdef __cplusplus
extern "C" {
#endif

/* 音频任务调用：把输出 PCM 追加进环形缓冲（只 memcpy，无浮点） */
void spec_fft_feed_pcm(const int16_t* pcm, int n);

/* 显示任务调用：取最新 N 个样本到 dst（不足 N 个返回 0，取到返回 1） */
int spec_fft_read_latest(int16_t* dst, int n);

/* 显示任务调用：Hann 加窗 + 512 点 FFT + 对数分 16 段，
   输出 0~1.5 的柱值（已含高频补偿；自动增益/平滑归调用方） */
void spec_fft_compute_bars(const int16_t* frame, float* bars);

#ifdef __cplusplus
}
#endif

#endif
