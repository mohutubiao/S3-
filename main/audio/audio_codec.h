#ifndef _AUDIO_CODEC_H
#define _AUDIO_CODEC_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <driver/i2s_std.h>
#include <esp_idf_version.h>

#include <vector>
#include <string>
#include <functional>

#include "board.h"

/* 2026-11-17 卡顿修复（单点改动）：6 → 12。
   24kHz 下 6×240 = 1440 帧 = 60ms，正好等于服务端一帧的时长，硬件余量为 0。
   audio_output 任务只要被抢占/等锁超过 60ms，DMA 就空，喇叭立刻断一下
   （对应 AUDIO out 日志里的 local 计数）。加到 12×240 = 120ms，
   可吸收 60~120ms 的调度抖动；代价是稳态播放延迟 +60ms、内部 RAM 约 +17KB
   （TX 立体声与 RX TDM 4ch 的 DMA 深度各翻倍）。
   回滚：把 12 改回 6 即可。 */
#define AUDIO_CODEC_DMA_DESC_NUM 12
#define AUDIO_CODEC_DMA_FRAME_NUM 240

// ESP-IDF 6 removed i2s_port_t and changed i2s_chan_config_t::id to an integer.
// Keep numeric I2S controller IDs usable on targets where IDF 5 does not expose
// every value through the target-specific i2s_port_t enum (for example ESP32-C3).
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define XIAOZHI_I2S_PORT(port) (port)
#else
#define XIAOZHI_I2S_PORT(port) static_cast<i2s_port_t>(port)
#endif

class AudioCodec {
public:
    AudioCodec();
    virtual ~AudioCodec();
    
    virtual void SetOutputVolume(int volume);
    virtual void SetInputGain(float gain);
    virtual void EnableInput(bool enable);
    virtual void EnableOutput(bool enable);

    virtual void OutputData(std::vector<int16_t>& data);
    virtual bool InputData(std::vector<int16_t>& data);
    virtual void Start();

    inline bool duplex() const { return duplex_; }
    inline bool input_reference() const { return input_reference_; }
    inline int input_sample_rate() const { return input_sample_rate_; }
    inline int output_sample_rate() const { return output_sample_rate_; }
    inline int input_channels() const { return input_channels_; }
    inline int output_channels() const { return output_channels_; }
    inline int output_volume() const { return output_volume_; }
    inline float input_gain() const { return input_gain_; }
    inline bool input_enabled() const { return input_enabled_; }
    inline bool output_enabled() const { return output_enabled_; }

protected:
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;

    bool duplex_ = false;
    bool input_reference_ = false;
    bool input_enabled_ = false;
    bool output_enabled_ = false;
    int input_sample_rate_ = 0;
    int output_sample_rate_ = 0;
    int input_channels_ = 1;
    int output_channels_ = 1;
    int output_volume_ = 70;
    float input_gain_ = 0.0;

    virtual int Read(int16_t* dest, int samples) = 0;
    virtual int Write(const int16_t* data, int samples) = 0;
};

#endif // _AUDIO_CODEC_H
