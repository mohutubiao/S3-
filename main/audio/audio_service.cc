#include "audio_service.h"
#include <esp_log.h>
#include <esp_heap_caps.h>   // heap_caps_malloc / MALLOC_CAP_SPIRAM：opus 任务栈挪 PSRAM 用
#include <esp_timer.h>       // 2026-11-15 输出断档定责：取单调微秒
#include <esp_wifi.h>        // 2026-11-15 播放期查询 WiFi 省电级别
#include <algorithm>         // 2026-11-15 抖动缓冲 min/max
#include <cstring>
#include <cmath>             // 2026-11-07 上行回声门限 sqrt/RMS

/* 2026-11-15 跨模块诊断（定义在 protocols/websocket_protocol.cc）：
   最后一包下行音频到达的单调毫秒。AudioOutputTask 用它判断"输出断档时
   上游到底有没有数据"，从而把卡音分成 local（设备端）和 upstream（上游）。 */
extern uint32_t g_ws_last_audio_arrival_ms;

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)        \
    (esp_ae_rate_cvt_cfg_t)                                  \
    {                                                        \
        .src_rate        = (uint32_t)(_src_rate),            \
        .dest_rate       = (uint32_t)(_dest_rate),           \
        .channel         = (uint8_t)(_channel),              \
        .bits_per_sample = ESP_AUDIO_BIT16,                  \
        .complexity      = 2,                                \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,  \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                                    \
    (esp_opus_dec_cfg_t)                                                                                  \
    {                                                                                                     \
        .sample_rate    = (uint32_t)(_sample_rate),                                                       \
        .channel        = ESP_AUDIO_MONO,                                                                 \
        .frame_duration = (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms),  \
        .self_delimited = false,                                                                          \
    }

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
#include "engines/afe_audio_engine.h"
#else
#include "engines/lite_audio_engine.h"
#endif

#define TAG "AudioService"

AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
}

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
    }
}

void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(codec->output_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
    } else {
        decoder_sample_rate_ = codec->output_sample_rate();
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create input resampler, error code: %d", resampler_ret);
        }
    }

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
    audio_engine_ = std::make_unique<AfeAudioEngine>();
#else
    audio_engine_ = std::make_unique<LiteAudioEngine>();
#endif
    audio_engine_->OnOutput([this](std::vector<int16_t>&& data) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });
    audio_engine_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });
    audio_engine_->OnWakeWordDetected([this](const std::string& wake_word) {
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        if (callbacks_.on_wake_word_detected) {
            callbacks_.on_wake_word_detected(wake_word);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    service_stopped_.store(false);
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING | AS_EVENT_AUDIO_INPUT_STOP_REQUEST);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 0);

    /* Start the audio output task
       2026-11-15：prio 4 → 5 并绑 core1。原来它没绑核，会随调度器在两个核之间漂，
       和 WiFi(0)/audio_input(0,prio8) 撞上时 I2S 喂数被打断 → 爆音。
       core1 上只有 opus_codec(5) 和 LVGL(1)，LVGL 压不过它，节奏稳定得多。 */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
        /* 2026-11-15 优先级 5 → 7：实测出现过一次超过 1 秒的输出断档，
           而任务本身大部分时间阻塞在 I2S 写入上、几乎不占 CPU ——
           提优先级只影响"醒来后多快能跑"，不会饿死别的任务。
           高于 opus 解码(5)，避免同优先级时间片轮转把喂数推迟。 */
    }, "audio_output", 2048 * 2, this, 7, &audio_output_task_handle_, 1);
#else
    /* Start the audio input task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048, this, 4, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
    // ⚠️ 2026-10-31 改动说明（这台机器内部 SRAM 极紧，历史最低只剩 263 字节）：
    //  1) 原先用 xTaskCreate 动态分配，栈在内部 SRAM。抓到过实锤崩溃
    //     "A stack overflow in task opus_codec has been detected"，
    //     时机在 StateMachine: connecting -> listening，即用户语音唤醒那一瞬。
    //  2) 第一次修法把栈从 16KB 提到 24KB —— 结果音频干脆起不来（语音唤醒不了、
    //     唤醒也不说话）—— 说明在内部 SRAM 里扩栈这条路走不通。
    //  3) 现在的做法：栈和 TCB 都从 PSRAM 出（板上有 8MB，基本闲置），
    //     内部 SRAM 一点不占，于是可以放心给到 32KB 余量。
    //     拿不到 PSRAM 时退回动态创建，保住「至少能用」。
    constexpr uint32_t kOpusStackBytes = 2048 * 16;   // 32KB
    void* stack_buf = heap_caps_malloc(kOpusStackBytes, MALLOC_CAP_SPIRAM);
    // ⚠️ TCB 绝对不能放 PSRAM！FreeRTOS 会断言：
    //   assert failed: xTaskCreateStaticPinnedToCore
    //     freertos_tasks_c_additions.h:300 (xPortCheckValidTCBMem(pxTaskBuffer))
    // 这条约束只针对 TCB，栈是可以放 PSRAM 的（xPortCheckValidStackMem 允许）。
    // 我第一版把两块都丢进 PSRAM，结果每次启机必崩、无限重启 —— 别再犯。
    void* task_buf = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    if (stack_buf != nullptr && task_buf != nullptr) {
        opus_codec_task_handle_ = xTaskCreateStaticPinnedToCore(
            [](void* arg) {
                auto* audio_service = static_cast<AudioService*>(arg);
                audio_service->OpusCodecTask();
                vTaskDelete(NULL);
            },
            "opus_codec", kOpusStackBytes / sizeof(StackType_t), this, 5,
            static_cast<StackType_t*>(stack_buf), static_cast<StaticTask_t*>(task_buf),
            1);   /* ⚠️ 2026-11-07 卡顿优化：prio 2→5、绑 core1 —— 原来 2 比
                     彩灯(3)/AFE(3)都低还到处漂，解码这种每 60ms 一帧的硬实时
                     活被随意挤占。core1 与 LVGL(prio1) 同核：LVGL 压不过它；
                     core0 留给 audio_input(prio8)+WiFi。 */
        ESP_LOGI(TAG, "opus_codec 栈已落在 PSRAM (%u B)，内部 SRAM 不占",
                 (unsigned)kOpusStackBytes);
        return;
    }
    ESP_LOGW(TAG, "PSRAM 分配失败，opus_codec 栈退回内部 RAM (16KB)");
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();
        vTaskDelete(NULL);
    }, "opus_codec", 2048 * 8, this, 5, &opus_codec_task_handle_);
}

void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_.store(true);
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    bool notify_drained = false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        ++playback_generation_;
        audio_encode_queue_.clear();
        audio_decode_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
    }
    if (notify_drained && callbacks_.on_playback_drained) {
        callbacks_.on_playback_drained();
    }
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (input_resampler_ != nullptr) {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            uint32_t output_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num, &output_samples);
            auto resampled = std::vector<int16_t>(output_samples * codec_->input_channels());
            uint32_t actual_output = output_samples;
            esp_ae_rate_cvt_process(input_resampler_, (esp_ae_sample_t)data.data(), in_sample_num,
                                   (esp_ae_sample_t)resampled.data(), &actual_output);
            resampled.resize(actual_output * codec_->input_channels());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
    constexpr EventBits_t kAudioInputActiveBits = AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING;

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, kAudioInputActiveBits |
            AS_EVENT_AUDIO_INPUT_STOP_REQUEST,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_.load()) {
            // ADC continuous mode keeps its hardware mutex from start until stop,
            // so the input task that started it must also stop it before exiting.
            if (codec_->input_enabled()) {
                codec_->EnableInput(false);
            }
            break;
        }

        if (bits & AS_EVENT_AUDIO_INPUT_STOP_REQUEST) {
            xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);

            // Recheck the active state in this task. Audio capture may have been
            // enabled after the timer posted the stop request.
            bits = xEventGroupGetBits(event_group_);
            if ((bits & kAudioInputActiveBits) == 0) {
                if (codec_->input_enabled()) {
                    codec_->EnableInput(false);
                }
                // Do not process the stale active bits returned by waitBits().
                continue;
            }
        }

        if (audio_input_need_warmup_.exchange(false)) {
            /* ⚠️ 2026-11-07 延迟优化：120 → 40ms。原来唤醒后麦克风白等
               120ms 才开始收音 —— 40ms 足够 codec 启动稳定，砍掉 80ms
               唤醒响应延迟。开头若偶有一帧杂音，AFE 的 AEC/VAD 会兜住。 */
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the selected audio engine */
        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            int samples = 160; // 10ms
            std::vector<int16_t> data;
            if (ReadAudioData(data, 16000, samples)) {
                audio_engine_->Feed(std::move(data));
                continue;
            }
        }

        // Read timeout/error should not terminate the input task.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return !audio_playback_queue_.empty() || service_stopped_.load();
        });
        if (service_stopped_.load()) {
            break;
        }

        /* ── 2026-11-15 自适应抖动缓冲（用户选型：平时无感、卡过变稳）─────────
           实测现象：服务端按分片推流，一批 9~14 包（0.5~0.8s）一次性到达，
           批与批之间停 0.6~1.0s；而设备端总缓冲只有 ~0.42s，停顿超过它就断一下。
           修法：每轮开口前先攒够 jitter_target_ms_ 的音频，用"晚开口"换"不断续"。
           自适应：断流一次 → 目标 +200ms（上限 800ms）；一整轮没断 → -100ms
           （下限 200ms）。所以正常对话几乎无感，只有在真卡过的会话里才多缓一点。
           ⚠️ 计数要算「播放队列 + 解码队列」：单看播放队列永远到不了 6 帧以上，
           会一直等到超时。 */
        if (jitter_priming_) {
            const int frame_ms = decoder_duration_ms_ > 0 ? decoder_duration_ms_ : 60;
            const size_t need = (size_t)std::max(1, jitter_target_ms_ / frame_ms);
            /* 2026-11-17 实测修正：把等待上限拉到 1200ms 试过一版 ——
               用户反馈「还是卡，而且段落延迟更长」，说明上游是按 1x 实数率
               出的，等再久也等不到多余的水，只是白白拖慢开口。
               回退成短等待（目标 + 300ms），延迟优先；蓄水不足如实打日志。 */
            const int wait_ms = jitter_target_ms_ + 300;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
            size_t buffered = 0;
            while (!service_stopped_.load() && std::chrono::steady_clock::now() < deadline) {
                buffered = audio_playback_queue_.size() + audio_decode_queue_.size() +
                           (decode_in_flight_ ? 1 : 0);
                if (buffered >= need) {
                    break;
                }
                audio_queue_cv_.wait_for(lock, std::chrono::milliseconds(20));
            }
            jitter_priming_ = false;
            ESP_LOGI(TAG, "抖动缓冲就绪：目标 %dms，实攒 %u 帧（≈%ums）%s", jitter_target_ms_,
                     (unsigned)buffered, (unsigned)(buffered * frame_ms),
                     buffered >= need ? "" : " ← 蓄水不足（服务端出流慢），开播后可能断一下");
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();

        // 卡顿诊断（2026-11-15 升级）：
        // 旧指标只看"取帧时队列空"，可它无法区分「解码慢」和「上游断供」——
        // 播放节奏≈到达节奏时队列本来就常空，数值高低并不等于听感卡顿。
        // 真正可听的爆音 = 相邻两次喂 I2S 的间隔明显超过一帧时长（60ms），
        // DMA 在那段时间里没数据，喇叭就是静音/咔哒。所以统计三件事：
        //   gap  = 相邻 OutputData 调用间隔 >90ms 的次数。
        //          2026-11-17：门槛从 200ms 降到 90ms —— 喇叭断 40ms 就听得出来，
        //          旧的 200ms 门槛把用户实际听到的 100~190ms 小洞全漏掉了，
        //          于是「日志零断档、耳朵一直抖」，自适应缓冲也永远不知道要加码。
        //   gmax = 本窗口最大间隔 ms（2 秒 = 整句没声，200ms = 抖一下）；
        //   cost = OutputData 自身耗时（>60ms 说明任务被抢占/写 I2S 阻塞）。
        // 对照同时刻的 "ws audio stats"：gmax 大且 ws 同期也没包 = 上游出流洞；
        // gmax 大而 ws 同期有包 = 设备端解码/调度问题。
        static uint32_t s_total = 0, s_gap = 0, s_gmax = 0, s_costmax = 0, s_local = 0, s_upstream = 0;
        /* 2026-11-17 静音帧取证：quiet = 本窗口内"峰值极低"的帧数。 */
        static uint32_t s_quiet = 0;
        static auto s_last_out = std::chrono::steady_clock::now();
        static bool s_has_last = false;
        /* 与 g_ws_last_audio_arrival_ms 同一时钟域（esp_timer 毫秒），
           用于判断"断档期间有没有收到包"。 */
        static uint32_t s_last_out_ms = 0;

        output_in_flight_ = true;
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }

        auto out_begin = std::chrono::steady_clock::now();
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_has_last) {
            auto gap_ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(out_begin - s_last_out)
                              .count();
            /* ⚠️ >2s 的间隔是对话静默（一轮说完等下一轮），不是播放断档。
               以前把它算进 gmax，才会出现 "gmax=122532ms" 这种废数据，
               连 local/upstream 归因也一起被污染。直接丢弃这一样本。 */
            if (gap_ms > 90 && gap_ms <= 2000) {
                s_gap++;
                /* 自适应：真断了一次就把缓冲目标加上去，下一轮开口更晚但不断 */
                jitter_turn_had_gap_ = true;
                if (jitter_target_ms_ < kJitterMaxMs) {
                    jitter_target_ms_ = std::min(kJitterMaxMs, jitter_target_ms_ + 200);
                    ESP_LOGW(TAG, "断流 %ums → 抖动缓冲目标提到 %dms", (unsigned)gap_ms, jitter_target_ms_);
                }
                /* 2026-11-15 断档定责（第三版，修正前两版的偏差）：
                   旧判据「窗口内有没有收到包」是错的 —— 断档结束那个包本身就是
                   "窗口内到达"的，于是 upstream 断供也会被算成 local。
                   正确判据：**数据到手后闲置了多久才被喂进 I2S**。
                     · 最后一包在 100ms 之前就到了，我们却一直没输出
                       → local（设备端 stall，数据在手却卡住）；
                     · 最后一包刚刚才到（≤100ms）→ upstream（上游断供，
                       是这包把播放叫醒的，改设备没用）。 */
                uint32_t last_arrival = g_ws_last_audio_arrival_ms;
                if (last_arrival <= now_ms && (now_ms - last_arrival) > 100) {
                    s_local++;
                } else {
                    s_upstream++;
                }

                /* 2026-11-15 断档后的"接缝"处理：上游洞（实测 1.2~1.8s）设备端填不平，
                   但可以让它**听不出来是故障** —— 洞结束时 PCM 是从非零幅度突然续上，
                   喇叭就是"啪"一下的咔哒。给这一帧开头 15ms 线性淡入，接缝变成自然起音。 */
                /* ⚠️ 解码输出是单声道 PCM（多声道扩张由 codec 的 Write 做），
                   所以这里按单声道处理，别用 output_channels() 去算步长。 */
                const int rate = codec_->output_sample_rate();
                int ramp_frames = rate > 0 ? rate * 15 / 1000 : 0;
                if (ramp_frames > (int)task->pcm.size()) {
                    ramp_frames = (int)task->pcm.size();
                }
                for (int f = 0; f < ramp_frames; ++f) {
                    const int32_t gain = (int32_t)(65536.0f * (float)f / (float)ramp_frames);
                    task->pcm[f] = (int16_t)(((int32_t)task->pcm[f] * gain) >> 16);
                }
            } else if (gap_ms > 2000) {
                /* 跨轮静默：本窗口不下结论，直接跳过，别污染统计 */
            }
            if (gap_ms > 90 && gap_ms <= 2000 && gap_ms > s_gmax) {
                s_gmax = gap_ms;
            }
        }
        s_last_out = out_begin;
        s_last_out_ms = now_ms;
        s_has_last = true;

        /* 2026-11-17 静音帧取证（纯统计，不改播放）：
           现有 gap 只看两次 OutputData 的间隔。服务端用静音帧填出流洞时，
           输出节奏完全正常（gap=0），喇叭里却是空的 —— 这就是"日志零断档、
           耳朵一直顿"的盲区。这里按帧峰值判定"这一帧其实没声音"（<500 ≈ -36dBFS），
           与 gap 对照即可定责：
             · gap=0 且 quiet 涨 → 上游静音填洞，设备端只能做防咔哒，不能造声音；
             · gap>0 且 quiet 不涨 → 设备端喂数断，继续动 DMA/调度；
             · 两者都为 0 → 数据本身连续，顿来自句子拼接等非音频因素。
           峰值用比较实现（1440 次），每帧 60ms 一次，开销可忽略。 */
        {
            int32_t peak = 0;
            for (size_t i = 0; i < task->pcm.size(); ++i) {
                int32_t v = task->pcm[i];
                if (v < 0) {
                    v = -v;
                }
                if (v > peak) {
                    peak = v;
                }
            }
            if (peak < 500) {
                s_quiet++;
            }
        }

        codec_->OutputData(task->pcm);

        {
            auto cost_ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - out_begin)
                               .count();
            if (cost_ms > s_costmax) {
                s_costmax = cost_ms;
            }
            if (++s_total >= 128) {
                /* 2026-11-15 播放期省电状态：ping 实测本机 MTU 往返 18~121ms 锯齿，
                   是 modem sleep 的典型特征。这行直接给出播放时 WiFi 到底睡不睡 ——
                   ps=0(NONE 不睡) / 1(MIN_MODEM) / 2(MAX_MODEM)。
                   若这里是 1/2，说明省电没压住，抖动和 TCP 队头阻塞都由此而来。 */
                wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
                esp_wifi_get_ps(&ps);
                ESP_LOGW(TAG,
                         "AUDIO out: frames=%u gap>90ms=%u(local=%u upstream=%u) gmax=%ums costmax=%ums "
                         "quiet=%u ps=%d",
                         s_total, s_gap, s_local, s_upstream, (unsigned)s_gmax, (unsigned)s_costmax,
                         (unsigned)s_quiet, (int)ps);
                s_total = 0;
                s_gap = 0;
                s_gmax = 0;
                s_costmax = 0;
                s_local = 0;
                s_upstream = 0;
                s_quiet = 0;
            }
        }

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;

        bool notify_drained = false;
        lock.lock();
#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp for server AEC */
        if (task->timestamp > 0) {
            timestamp_queue_.push_back(task->timestamp);
        }
#endif
        output_in_flight_ = false;
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (notify_drained && callbacks_.on_playback_drained) {
            callbacks_.on_playback_drained();
        }
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_.load() ||
                !audio_encode_queue_.empty() ||
                (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_.load()) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            decode_in_flight_ = true;
            const uint32_t generation = playback_generation_;
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            bool decoded = false;
            if (packet->payload.empty()) {
                /* ── PLC 丢包补偿（2026-11-07 卡顿修复）──
                   协议层对确认丢失的帧补了空 payload 包：这里不解码，
                   直接输出一帧静音保持播放流连续 —— UDP 丢 22% 的包时，
                   旧逻辑表现为声音一段段断，现在听感是"短暂弱音"。 */
                task->pcm.assign(decoder_frame_size_, 0);
                decoded = true;
            } else if (opus_decoder_ != nullptr) {
                task->pcm.resize(decoder_frame_size_);
                esp_audio_dec_in_raw_t raw = {
                    .buffer = (uint8_t *)(packet->payload.data()),
                    .len = (uint32_t)(packet->payload.size()),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out_frame = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(task->pcm.size() * sizeof(int16_t)),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                decoder_lock.unlock();
                if (ret == ESP_AUDIO_ERR_OK) {
                    task->pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                    if (decoder_sample_rate_ != codec_->output_sample_rate() && output_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task->pcm.size(), &target_size);
                        std::vector<int16_t> resampled(target_size);
                        uint32_t actual_output = target_size;
                        esp_ae_rate_cvt_process(output_resampler_, (esp_ae_sample_t)task->pcm.data(), task->pcm.size(),
                                                (esp_ae_sample_t)resampled.data(), &actual_output);
                        resampled.resize(actual_output);
                        task->pcm = std::move(resampled);
                    }
                    decoded = true;
                } else {
                    ESP_LOGE(TAG, "Failed to decode audio after resize, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG, "Audio decoder is not configured");
            }

            lock.lock();
            if (decoded && generation == playback_generation_ && !service_stopped_.load()) {
                audio_playback_queue_.push_back(std::move(task));
            }
            decode_in_flight_ = false;
            debug_statistics_.decode_count++;
            const bool notify_drained = MarkPlaybackDrainedLocked();
            audio_queue_cv_.notify_all();
            lock.unlock();
            if (notify_drained && callbacks_.on_playback_drained) {
                callbacks_.on_playback_drained();
            }
            lock.lock();
        }
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty()) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;

            if (opus_encoder_ != nullptr && task->pcm.size() == encoder_frame_size_) {
                /* 上行门限：RMS 低于它视为回声残余（发静音）。真人说话 RMS
                   通常 >1000，回声残余 <300 —— 原值 400 居中偏安全。
                   ⚠️ 2026-11-15 实测日志三次打断都是 `suppressed=NN passed=0`：
                   AI 说话期间**一帧都没放行**，说明 400 对轻声打断偏高，
                   用户直接开口的声音被整段填成静音，服务器收不到 → 只能等超时
                   回待机（表现为"打断像没反应"）。降到 280 让正常说话容易过，
                   离回声残余（<300）仍有区分度；若出现 AI 被自己声音打断，
                   再往 320~350 回调。
                   ⚠️ 2026-11-16 用户反馈「语音打断有点太灵敏」→ 280 → 340。
                   这就是上面预留的回调区间：轻声/环境噪声不再能戳穿门限，
                   正常开口的音量（RMS 通常 >1000）仍远高于它，打断可用。
                   若之后又变成"喊了没反应"，降到 310 试；
                   若依旧灵敏，往上最多到 380（再高回声残余就可能漏进去）。
                   ⚠️ 2026-11-16 第二次反馈「还是太灵敏」：单纯抬门限治不了 ——
                   回声残余偶尔整帧冲过 340，一帧就够服务器 VAD 起判。改成
                   **连续帧确认**：必须连续 kUplinkGateConfirmFrames 帧（60ms/帧）
                   都高于门限才放行，中途掉一帧就清零重新数。真人开口 300ms+
                   稳过 3 帧；一声咳嗽/关门/回声尖峰只有 1 帧，打断不了。 */
                   static constexpr double kUplinkGateRms = 340.0;
                   static constexpr uint8_t kUplinkGateConfirmFrames = 3;   /* 180ms */
                /* ── 2026-11-07 上行静音门限（AI 回话碎段根治）──
                   现象：AI 说话被服务器切成一段一段（两个通道都复现，
                   日志见 WS 收到服务器 0x8 关闭帧）。机理：喇叭声的 AEC
                   残余被麦克风收走上行，服务器 VAD 误判用户插话 → 掐 TTS。
                   方案：AI 说话（uplink_suppress_）时算短时 RMS —— 低于门限
                   （回声残余）发静音帧；高于门限（真人喊话，能量远高于回声）
                   原样放行，语音打断功能保留。唤醒词/BOOT 打断不走这条上行，
                   不受影响。 */
                if (uplink_suppress_.load() && task->type == kAudioTaskTypeEncodeToSendQueue) {
                    int64_t acc = 0;
                    for (int16_t s : task->pcm) {
                        acc += (int64_t)s * s;
                    }
                    const double rms = sqrt((double)acc / (double)task->pcm.size());
                    /* 连续帧确认：低于门限立刻清零；高于门限要连中 N 帧才开口。
                       前 N-1 帧仍然发静音 —— 宁可慢 180ms 让真人开口，也不让
                       单帧回声尖峰把 AI 的话掐掉。 */
                    bool allow = false;
                    if (rms < kUplinkGateRms) {
                        gate_hot_frames_ = 0;
                    } else if (gate_hot_frames_ < kUplinkGateConfirmFrames) {
                        gate_hot_frames_++;
                        allow = (gate_hot_frames_ >= kUplinkGateConfirmFrames);
                    } else {
                        allow = true;   /* 已在放行段，保持 */
                    }
                    if (allow) {
                        gate_passed_++;
                    } else {
                        std::fill(task->pcm.begin(), task->pcm.end(), 0);
                        gate_suppressed_++;
                    }
                }
                std::vector<uint8_t> buf(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = buf.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet->payload.assign(buf.data(), buf.data() + out.encoded_bytes);

                    if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            /* Never let a full send queue stall encoding: stale realtime
                             * audio is useless to the server, so drop the oldest packet. */
                            if (audio_send_queue_.size() >= MAX_SEND_PACKETS_IN_QUEUE) {
                                audio_send_queue_.pop_front();
                            }
                            audio_send_queue_.push_back(std::move(packet));
                        }
                        if (callbacks_.on_send_queue_available) {
                            callbacks_.on_send_queue_available();
                        }
                    } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        audio_testing_queue_.push_back(std::move(packet));
                    }
                    debug_statistics_.encode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG, "Failed to encode audio: encoder not configured or invalid frame size (got %u, expected %u)",
                         task->pcm.size(), encoder_frame_size_);
            }
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetUplinkSuppress(bool suppress) {
    gate_hot_frames_ = 0;   /* 每次开关闸都从零开始数连续帧，别带上一次的残留 */
    if (!suppress && (gate_suppressed_ > 0 || gate_passed_ > 0)) {
        /* 每次关闭（AI 说完/被打断）打一条：如果 suppressed 为 0 = 门限
           没拦到任何回声（说明残余回声 RMS 全部高于门限 → 门限值偏低，
           服务器仍会误判）；passed 远大于 suppressed = 大量帧在放行。 */
        ESP_LOGI(TAG, "uplink gate: suppressed=%u passed=%u (threshold check)",
                 gate_suppressed_, gate_passed_);
        gate_suppressed_ = 0;
        gate_passed_ = 0;
    }
    uplink_suppress_.store(suppress);
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (decoder_sample_rate_ == sample_rate && decoder_duration_ms_ == frame_duration) {
        return;
    }
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    decoder_lock.unlock();
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
        return;
    }
    decoder_sample_rate_ = sample_rate;
    decoder_duration_ms_ = frame_duration;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (decoder_sample_rate_ != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", decoder_sample_rate_, codec->output_sample_rate());
        if (output_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(output_resampler_);
            output_resampler_ = nullptr;
        }
        esp_ae_rate_cvt_cfg_t output_resampler_cfg = RATE_CVT_CFG(
            decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
        }
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);

    uint32_t dropped_total = 0;
    {
        /* Push the task to the encode queue */
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);

        /* If the task is to send queue, we need to set the timestamp */
        if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
            if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
                task->timestamp = timestamp_queue_.front();
            } else {
                ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp", timestamp_queue_.size());
            }
            timestamp_queue_.pop_front();
        }

        /* Microphone audio is realtime, so drop the oldest frame instead of blocking.
         * Blocking here would stall the audio engine task (AFE fetch) and deadlock the
         * whole input pipeline when the send queue stops being drained (e.g. network
         * congestion or a failed UDP send). */
        if (audio_encode_queue_.size() >= MAX_ENCODE_TASKS_IN_QUEUE) {
            audio_encode_queue_.pop_front();
            dropped_total = ++debug_statistics_.encode_drop_count;
        }
        audio_encode_queue_.push_back(std::move(task));
        audio_queue_cv_.notify_all();
    }

    /* Log outside the lock (UART writes are slow and would starve the codec task),
     * at most once per second. */
    if (dropped_total > 0) {
        int64_t now = esp_timer_get_time();
        if (now - last_encode_drop_log_time_ >= 1000000) {
            last_encode_drop_log_time_ = now;
            ESP_LOGW(TAG, "Encode queue is full, dropping oldest frame (dropped %lu so far)",
                     (unsigned long)dropped_total);
        }
    }
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this]() {
                return service_stopped_.load() ||
                    audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE;
            });
        } else {
            /* 2026-11-15 吞音定责：解码队列满（MAX_DECODE_PACKETS_IN_QUEUE =
               2400ms）时这里**直接把这包音频丢掉**，听感就是"字被吃掉"。
               以前完全静默，出问题时看不出来 —— 加计数，每 20 包打一条。 */
            static uint32_t s_decode_drop = 0;
            if (++s_decode_drop % 20 == 1) {
                ESP_LOGW(TAG, "解码队列满，丢弃下行包（累计 %u 包 ≈ %.2fs 音频）",
                         (unsigned)s_decode_drop, (double)s_decode_drop * 0.06);
            }
            return false;
        }
    }
    if (service_stopped_.load()) {
        return false;
    }
    playback_drained_notified_ = false;
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (audio_engine_) {
        audio_engine_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    static const std::string empty;
    return audio_engine_ ? audio_engine_->GetLastDetectedWakeWord() : empty;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (audio_engine_ && audio_engine_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!InitializeAudioEngine() || !audio_engine_->HasWakeWord()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_engine_->EnableWakeWordDetection(true);
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        if (audio_engine_initialized_) {
            audio_engine_->EnableWakeWordDetection(false);
        }
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");

    if (enable) {
        if (!InitializeAudioEngine()) {
            return;
        }
        ResetDecoder();
        audio_input_need_warmup_ = true;
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_engine_->EnableVoiceProcessing(true);
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        if (audio_engine_initialized_) {
            audio_engine_->EnableVoiceProcessing(false);
        }
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        if (!audio_decode_queue_.empty()) {
            playback_drained_notified_ = false;
        }
        audio_queue_cv_.notify_all();
    }
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    device_aec_enabled_ = enable;

    if (audio_engine_initialized_) {
        audio_engine_->EnableDeviceAec(enable);
    } else {
        ESP_LOGI(TAG, "Deferring AEC change until the audio engine is initialized");
    }
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

void AudioService::PlaySound(const std::string_view& ogg) {
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    const auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();

    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnDemuxerFinished([this](const uint8_t* data, int sample_rate, size_t size){
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = 60;
        packet->payload.resize(size);
        std::memcpy(packet->payload.data(), data, size);
        PushPacketToDecodeQueue(std::move(packet), true);
    });
    demuxer->Reset();
    demuxer->Process(buf, size);
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && IsPlaybackDrainedLocked() && audio_testing_queue_.empty();
}

bool AudioService::IsPlaybackIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return IsPlaybackDrainedLocked();
}

void AudioService::ResetDecoder() {
    bool notify_drained = false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        /* 2026-11-15 吞音取证：这个函数会把还没播的音频整段扔掉。
           以前完全静默 —— 用户报"最后一句话丢音"时无法确认是这个清掉的。
           pending ≈ 剩余未播时长（60ms/包 + 已在播放队列里的 PCM 帧）。 */
        const size_t pending = audio_decode_queue_.size() + audio_playback_queue_.size();
        if (pending > 0) {
            ESP_LOGW(TAG, "ResetDecoder 丢弃 %u 包未播音频（≈%ums）", (unsigned)pending,
                     (unsigned)(pending * 60));
        }
        /* 2026-11-15 自适应抖动缓冲：新一轮开始前重新蓄水。
           上一轮完全没断流就把目标收回去（-100ms），保持平时低延迟。 */
        if (!jitter_turn_had_gap_ && jitter_target_ms_ > kJitterMinMs) {
            jitter_target_ms_ = std::max(kJitterMinMs, jitter_target_ms_ - 100);
        }
        jitter_turn_had_gap_ = false;
        jitter_priming_ = true;

        ++playback_generation_;
        std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
        if (opus_decoder_ != nullptr) {
            esp_opus_dec_reset(opus_decoder_);
        }
        decoder_lock.unlock();
        timestamp_queue_.clear();
        audio_decode_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
    }
    if (notify_drained && callbacks_.on_playback_drained) {
        callbacks_.on_playback_drained();
    }
}

bool AudioService::IsPlaybackDrainedLocked() const {
    return audio_decode_queue_.empty() && audio_playback_queue_.empty() &&
        !decode_in_flight_ && !output_in_flight_;
}

bool AudioService::MarkPlaybackDrainedLocked() {
    if (!IsPlaybackDrainedLocked() || playback_drained_notified_) {
        return false;
    }
    playback_drained_notified_ = true;
    return true;
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        // ADC continuous start/stop must run in the same task. Wake the audio
        // input task instead of closing the codec from the esp_timer task.
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        // Keep TX clock when duplex RX is active; otherwise RX may stall on some boards.
        if (!(codec_->duplex() && codec_->input_enabled())) {
            codec_->EnableOutput(false);
        }
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }

    // 内部 SRAM 是这台设备的硬约束：不够用时 esp-tls 会 select() "Not enough space"、
    // MQTT 连不上、音频跟着爆音。这里把各音频任务的栈用量打出来，
    // 好按真实用量把栈调到刚好够（high water mark = 历史最小剩余，越小越危险）。
    static uint32_t s_dbg_ticks = 0;
    if (++s_dbg_ticks == 30) {  // 启动 30 秒后各任务都跑过一轮了，采样最准
        auto wm = [](TaskHandle_t h, const char* name, size_t total_bytes) {
            if (h == nullptr) {
                return;
            }
            const size_t left = uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t);
            ESP_LOGW(TAG, "stack %-12s used %4u / %u B", name, (unsigned)(total_bytes - left),
                     (unsigned)total_bytes);
        };
        wm(audio_input_task_handle_, "audio_input", 2048 * 3);
        wm(audio_output_task_handle_, "audio_output", 2048 * 2);
        wm(opus_codec_task_handle_, "opus_codec", 2048 * 8);
        if (TaskHandle_t t = xTaskGetHandle("taskLVGL"); t != nullptr) {
            ESP_LOGW(TAG, "stack %-12s left %4u B", "taskLVGL",
                     (unsigned)(uxTaskGetStackHighWaterMark(t) * sizeof(StackType_t)));
        }
        ESP_LOGW(TAG, "internal sram free %u KB", (unsigned)(esp_get_free_internal_heap_size() / 1024));
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    if (audio_engine_initialized_ && models_list_ != models_list) {
        ESP_LOGW(TAG, "Ignoring speech model replacement after audio engine initialization");
        return;
    }
    models_list_ = models_list;
}

bool AudioService::IsAfeWakeWord() {
    return audio_engine_initialized_ && audio_engine_->IsAfeWakeWord();
}

bool AudioService::InitializeAudioEngine() {
    if (!audio_engine_) {
        return false;
    }
    if (audio_engine_initialized_) {
        return true;
    }
    if (!audio_engine_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_)) {
        ESP_LOGE(TAG, "Failed to initialize audio engine");
        return false;
    }
    audio_engine_initialized_ = true;
    audio_engine_->EnableDeviceAec(device_aec_enabled_);
    return true;
}
