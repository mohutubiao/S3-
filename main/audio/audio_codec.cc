#include "audio_codec.h"
#include "board.h"
#include "settings.h"
#include "orb_audio.h"      // orb_feed_pcm_global：把播放/录音 PCM 转成唤醒球的电平
#include "spec_fft.h"       // 2026-11-06 音乐频谱：输出 PCM 专用通道（只 memcpy，FFT 在显示任务做）

#include <esp_log.h>
#include <esp_timer.h>
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
    /* ── 2026-11-15 防「咔哒/字被切碎」：缓冲区余量本来是 0 ──────────────
       I2S DMA 是 6×240 帧，24kHz 下正好 60ms；而服务器一帧 PCM 也正好
       60ms。两者相等意味着 DMA 里没有任何余量：只要输出任务被调度晚
       10~20ms，喇叭就已经没数据了（实测单帧喂入耗时 costmax 78~89ms，
       超过 60ms 就是必然断流）。
       修法：把一帧拆成 20ms 一片喂，DMA 里始终压着约 40ms 的垫子，
       抗抖动能力从 0 变成 40ms，而多出来的只是每帧 2 次 Write 调用。
       ⚠️ 不要改成把 DMA 加深：RX 是 4 slot TDM，加深会把内部 RAM
       成倍吃掉（+11KB/倍），这里零内存成本拿同样的效果。 */
    const int output_rate = output_sample_rate_;
    const int slice = output_rate > 0 ? output_rate / 50 : 0;   /* 20ms 一片 */
    if (slice <= 0 || static_cast<int>(data.size()) <= slice) {
        Write(data.data(), data.size());
        return;
    }
    for (size_t offset = 0; offset < data.size(); offset += static_cast<size_t>(slice)) {
        int n = static_cast<int>(data.size() - offset);
        if (n > slice) {
            n = slice;
        }
        Write(data.data() + offset, n);
    }
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

namespace {
/* 2026-11-15 音量写盘去抖：
   原来每按一次音量键就 nvs_set 一次（还没 Commit），连按十几下就是十几次 flash
   写，按键线程会有一点点毛刺，也白磨 flash。改成「最后一次变化 800ms 后才写」。 */
esp_timer_handle_t s_volume_save_timer = nullptr;
int s_pending_volume = 10;

void SaveVolumeToNvs(void*) {
    Settings settings("audio", true);
    settings.SetInt("output_volume", s_pending_volume);
    settings.Commit();     // 不 Commit 重启会丢（这个坑踩过）
}
}  // namespace

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);

    s_pending_volume = volume;
    if (s_volume_save_timer == nullptr) {
        esp_timer_create_args_t args = {
            .callback = SaveVolumeToNvs,
            .name = "vol_save",
        };
        if (esp_timer_create(&args, &s_volume_save_timer) != ESP_OK) {
            s_volume_save_timer = nullptr;
            // 定时器都建不出来就退回立即写，功能优先
            Settings settings("audio", true);
            settings.SetInt("output_volume", output_volume_);
            settings.Commit();
            return;
        }
    }
    esp_timer_stop(s_volume_save_timer);          // 连按时不断重置计时
    esp_timer_start_once(s_volume_save_timer, 800 * 1000);
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
