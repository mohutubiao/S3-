#include "audio_codec.h"
#include "board.h"
#include "settings.h"
#include "orb_audio.h"      // orb_feed_pcm_global：把播放/录音 PCM 转成唤醒球的电平
#include "spec_fft.h"       // 2026-11-06 音乐频谱：输出 PCM 专用通道（只 memcpy，FFT 在显示任务做）

#include <esp_log.h>
#include <cstring>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    /* 2026-11-02：把播放中的 PCM 丢给唤醒声波 —— 这是所有板子共用的出口，
       一处挂钩对所有音频方案（ES8311/ES8388/NoCodec…）都生效。
       orb_feed_pcm_global 内部只做 Goertzel + 量化，不碰 LVGL、不加锁。 */
    if (!data.empty()) {
        orb_feed_pcm_global(data.data(), (int)data.size());
        /* 2026-11-06 音乐频谱专用通道：只 memcpy 进环形缓冲，FFT 在 LVGL
           tick 里做（音频路径零计算 —— 上次音乐卡顿的教训）。
           只加在 OutputData，InputData 不动：麦克风链路喂进来会导致
           待机听环境声时频谱乱跳。 */
        spec_fft_feed_pcm(data.data(), (int)data.size());
    }
    Write(data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        /* 聆听时让球跟着麦克风的音量/频谱起伏 */
        orb_feed_pcm_global(data.data(), samples);
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }

    ESP_LOGI(TAG, "Audio codec started");
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
    
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}
