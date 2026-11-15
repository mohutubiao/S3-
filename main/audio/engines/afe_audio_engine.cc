#include "afe_audio_engine.h"

#include <cassert>
#include <cmath>          // 2026-11-15 待机麦克风 RMS 仪表
#include <cstring>
#include <sstream>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vadn_models.h>
#include <freertos/idf_additions.h>   // xTaskCreateWithCaps / vTaskDeleteWithCaps

#include "audio_service.h"
#include "wake_word_models.h"
#include "wake_words/custom_wake_word.h"

#define TAG "AfeAudioEngine"

#if CONFIG_USE_AUDIO_PROCESSOR
static constexpr bool kUseAfeForVoiceProcessing = true;
#else
static constexpr bool kUseAfeForVoiceProcessing = false;
#endif

/* 2026-11-15 唤醒灵敏度（内置词）：
   内置唤醒词（WakeNet）此前完全用模型出厂门限 —— 开机日志
   `wakenet9l_tts1h8_你好小智_3_0.631_0.635, mode:0` 里的 0.631 就是它
   （mode:0 = DET_MODE_90）。用户实测要靠近/提嗓门才醒。
   esp-sr 的 set_wakenet_threshold 支持 0.40~0.9999 覆盖出厂值，
   **越小越灵敏**。
   取值经过一轮实测：0.55 时用户报「待机无故自己进对话」，日志对上
   `Wake word detected: 你好小智 (state: 3)`（state 3 = idle，排除播放
   回声），而静置 60s（环境只有 20~360 RMS）零误唤醒 —— 说明是有声音
   时被误认，灵敏度越界了。现取 0.60：介于出厂 0.631 与过灵敏的 0.55。
   ⚠️ 这是唯一旋钮：仍误唤醒 → 往上 0.63~0.66；又难叫 → 往下 0.58。 */
static constexpr float kWakeNetThreshold = 0.60f;

AfeAudioEngine::AfeAudioEngine() {
    event_group_ = xEventGroupCreate();
}

AfeAudioEngine::~AfeAudioEngine() {
    custom_wake_word_.reset();
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }
    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }
    if (owns_models_ && models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
}

bool AfeAudioEngine::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    if (afe_data_ != nullptr || (codec_ != nullptr && !kUseAfeForVoiceProcessing && wake_detector_ == WakeDetector::kNone)) {
        return true;
    }
    if (codec == nullptr) {
        ESP_LOGE(TAG, "Codec is null");
        return false;
    }

    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
    output_buffer_.reserve(frame_samples_);

    if (models_list == nullptr) {
        models_ = esp_srmodel_init("model");
        owns_models_ = models_ != nullptr;
    } else {
        models_ = models_list;
    }

    char* wakenet_model_name = nullptr;
    char* multinet_model_name = nullptr;
    std::vector<std::string> wakenets;
    if (models_ != nullptr && models_->num > 0) {
        for (int i = 0; i < models_->num; ++i) {
            ESP_LOGI(TAG, "Model %d: %s", i, models_->model_name[i]);
            if (strncmp(models_->model_name[i], ESP_WN_PREFIX, strlen(ESP_WN_PREFIX)) == 0) {
                wakenets.push_back(models_->model_name[i]);
            }
        }
        multinet_model_name = esp_srmodel_filter(models_, ESP_MN_PREFIX, nullptr);

        /* 用户在网页上选的词（存 NVS，重启生效）。没选过/选中的不在镜像里，
           都回退到第一个内置词，保证一定能唤醒。 */
        const std::string want = wakeword::GetMode();
        if (!want.empty() && want != "custom") {
            wakenet_model_name = esp_srmodel_filter(models_, ESP_WN_PREFIX, want.c_str());
            if (wakenet_model_name == nullptr) {
                ESP_LOGW(TAG, "选中唤醒词 %s 不在 assets 里，回退第一个内置词", want.c_str());
            }
        }
        if (wakenet_model_name == nullptr) {
            /* 没选过：优先 sdkconfig 那个（小智），别让 srmodels.bin 的字母序决定默认 */
            const std::string factory = wakeword::FactoryDefaultWakenet();
            if (!factory.empty()) {
                wakenet_model_name = esp_srmodel_filter(models_, ESP_WN_PREFIX, factory.c_str());
            }
        }
        if (wakenet_model_name == nullptr) {
            wakenet_model_name = esp_srmodel_filter(models_, ESP_WN_PREFIX, nullptr);
        }
        wakeword::RegisterAvailable(wakenets, multinet_model_name != nullptr);
        ESP_LOGI(TAG, "唤醒词: %s（内置可选 %d 个, Multinet %s）",
                 wakenet_model_name ? wakenet_model_name : "无",
                 (int)wakenets.size(), multinet_model_name ? "有" : "无");
    }

    /* 自定义词（MultiNet）优先条件：assets 里有 Multinet 模型，
       且用户选了 custom，或镜像里压根没有内置词。 */
    const bool use_custom = (multinet_model_name != nullptr) &&
                            (wakeword::GetMode() == "custom" || wakenet_model_name == nullptr);

    if (use_custom) {
        wake_detector_ = WakeDetector::kMultiNet;
        custom_wake_word_ = std::make_unique<CustomWakeWord>();
        custom_wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            last_detected_wake_word_ = wake_word;
            xEventGroupClearBits(event_group_, kWakeWordEnabled);
            UpdateActiveState();
            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(wake_word);
            }
        });
        if (!custom_wake_word_->Initialize(codec_, models_)) {
            ESP_LOGE(TAG, "Failed to initialize MultiNet wake-word detector");
            custom_wake_word_.reset();
            wake_detector_ = WakeDetector::kNone;
            return false;
        }
    } else if (wakenet_model_name != nullptr) {
        wake_detector_ = WakeDetector::kWakeNet;
        auto words = esp_srmodel_get_wake_words(models_, wakenet_model_name);
        if (words != nullptr) {
            std::stringstream stream(words);
            std::string word;
            while (std::getline(stream, word, ';')) {
                wake_words_.push_back(word);
            }
        }
#if CONFIG_SEND_WAKE_WORD_DATA
        if (!wake_word_audio_cache_.Initialize(16000 * 2)) {
            ESP_LOGW(TAG, "Wake-word audio upload disabled: PSRAM cache allocation failed");
        }
#endif
    }

    const bool needs_afe = kUseAfeForVoiceProcessing || wake_detector_ != WakeDetector::kNone;
    if (!needs_afe) {
        ESP_LOGI(TAG, "Initialized as raw engine because AFE features are disabled");
        return true;
    }

    int ref_num = codec_->input_reference() ? 1 : 0;
    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; ++i) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; ++i) {
        input_format.push_back('R');
    }

    char* vad_model_name = models_ == nullptr
        ? nullptr
        : esp_srmodel_filter(models_, ESP_VADN_PREFIX, nullptr);
    afe_config_t* afe_config = afe_config_init(
        input_format.c_str(), models_, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    if (afe_config == nullptr) {
        ESP_LOGE(TAG, "Failed to create AFE configuration");
        return false;
    }

    afe_config->aec_init = codec_->input_reference();
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->aec_nlp_level = AEC_NLP_LEVEL_VERYAGGR;
    afe_config->ns_init = false;
    afe_config->vad_init = kUseAfeForVoiceProcessing;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }
    afe_config->wakenet_init = wake_detector_ == WakeDetector::kWakeNet;
    afe_config->wakenet_model_name = wake_detector_ == WakeDetector::kWakeNet
        ? wakenet_model_name
        : nullptr;
    /* ⚠️ 2026-11-07 回退：AGC 实验 ❌ —— 开了之后用户持续报「AI 回话
       一段一段断」。机理：AGC 把 AEC 消不净的喇叭残余回声也放大了，
       上行到服务器，服务器 VAD 误判用户在说话 → 反复打断 TTS → 碎段。
       灵敏度改由「阈值 20→12」承担，AGC 保持关。 */
    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    if (afe_iface_ != nullptr) {
        afe_data_ = afe_iface_->create_from_config(afe_config);
    }
    afe_config_free(afe_config);

    if (afe_iface_ == nullptr || afe_data_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create FD AFE instance");
        afe_iface_ = nullptr;
        afe_data_ = nullptr;
        return false;
    }

    if (wake_detector_ == WakeDetector::kWakeNet) {
        afe_iface_->disable_wakenet(afe_data_);
        /* 2026-11-15：把内置词的判定门限从模型出厂值（你好小智 0.631）
           压到 kWakeNetThreshold，失败也继续跑（有版本该函数指针为 NULL）。 */
        if (afe_iface_->set_wakenet_threshold != nullptr) {
            int ret = afe_iface_->set_wakenet_threshold(afe_data_, 1, kWakeNetThreshold);
            ESP_LOGI(TAG, "内置词唤醒阈值 %.3f（出厂约 0.631），设置结果 %d", kWakeNetThreshold, ret);
        } else {
            ESP_LOGW(TAG, "当前 esp-sr 无 set_wakenet_threshold，唤醒阈值保持出厂值");
        }
    }
    if (codec_->input_reference()) {
        afe_iface_->disable_aec(afe_data_);
    }
    afe_iface_->print_pipeline(afe_data_);

    /* 2026-11-11：内部 SRAM 只剩 ~33KB 时，AFE 主体创建后连这 4KB 任务栈都
       分不出来（日志 "Failed to create AFE processing task"，随后唤醒全废）。
       项目已开 CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY，把栈挪到 PSRAM：
       省出 4KB 内部 RAM，代价是该任务栈访问偶有 cache miss。
       ⚠️ 用 WithCaps 创建就必须用 vTaskDeleteWithCaps 删除，否则栈不回收。 */
    BaseType_t task_created = xTaskCreateWithCaps([](void* arg) {
        auto* engine = static_cast<AfeAudioEngine*>(arg);
        engine->ProcessingTask();
        vTaskDeleteWithCaps(nullptr);
    }, "audio_afe", 4096, this, 3, &processing_task_, MALLOC_CAP_SPIRAM);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create AFE processing task");
        afe_iface_->destroy(afe_data_);
        afe_data_ = nullptr;
        afe_iface_ = nullptr;
        return false;
    }

    const char* detector = wake_detector_ == WakeDetector::kWakeNet
        ? "WakeNet"
        : (wake_detector_ == WakeDetector::kMultiNet ? "MultiNet" : "none");
    ESP_LOGI(TAG, "Initialized FD AFE, detector: %s, NS: off, feed: %d, fetch: %d",
        detector, afe_iface_->get_feed_chunksize(afe_data_), afe_iface_->get_fetch_chunksize(afe_data_));
    return true;
}

void AfeAudioEngine::Feed(std::vector<int16_t>&& data) {
    EventBits_t bits = xEventGroupGetBits(event_group_);
    if ((bits & kVoiceProcessingEnabled) && !kUseAfeForVoiceProcessing) {
        OutputRawAudio(data);
    }
    if (afe_data_ == nullptr || (bits & kAfeActive) == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if ((xEventGroupGetBits(event_group_) & kAfeActive) == 0) {
        return;
    }

    /* 2026-11-15 唤醒排查仪表：待机检测唤醒词时，每 5 秒打一次窗口内峰值 RMS。
       判读（16bit PCM）：正常说话 800~3000；隔房间/背对设备 200~600；
       只有几十 → 声音压根没进来，先查麦克风/输入增益/接线，别再调阈值。
       注：AFE 自带的 data_volume 在 VC 模式下无效，所以自己算。 */
    if (bits & kWakeWordEnabled) {
        static double s_peak = 0;
        static int64_t s_t0 = 0;
        if (!data.empty()) {
            double sum = 0;
            for (int16_t s : data) {
                sum += static_cast<double>(s) * static_cast<double>(s);
            }
            double rms = std::sqrt(sum / static_cast<double>(data.size()));
            if (rms > s_peak) {
                s_peak = rms;
            }
        }
        int64_t now = esp_timer_get_time();
        if (s_t0 == 0) {
            s_t0 = now;
        } else if (now - s_t0 >= 5 * 1000 * 1000) {
            ESP_LOGI(TAG, "待机麦克风峰值 RMS=%.0f（<200 = 声音没进来，先查麦克风/增益）", s_peak);
            s_peak = 0;
            s_t0 = now;
        }
    }

    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    size_t chunk_size = afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
    while (input_buffer_.size() >= chunk_size) {
        afe_iface_->feed(afe_data_, input_buffer_.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
    }
}

void AfeAudioEngine::EnableWakeWordDetection(bool enable) {
    if (!HasWakeWord()) {
        return;
    }

    // WakeNet enable/disable on the AFE instance is applied by ProcessingTask
    // (see ApplyAfeControls), driven by the kWakeWordEnabled bit.
    if (enable) {
        if (wake_detector_ == WakeDetector::kMultiNet) {
            custom_wake_word_->Start();
        }
        xEventGroupSetBits(event_group_, kWakeWordEnabled);
    } else {
        xEventGroupClearBits(event_group_, kWakeWordEnabled);
        if (wake_detector_ == WakeDetector::kMultiNet) {
            custom_wake_word_->Stop();
        }
    }
    UpdateActiveState();
}

void AfeAudioEngine::EnableVoiceProcessing(bool enable) {
    if (enable) {
        xEventGroupSetBits(event_group_, kVoiceProcessingEnabled);
    } else {
        xEventGroupClearBits(event_group_, kVoiceProcessingEnabled);
        is_speaking_ = false;
    }
    UpdateActiveState();
}

void AfeAudioEngine::EnableDeviceAec(bool enable) {
    device_aec_enabled_.store(enable);
    if (enable && (codec_ == nullptr || !codec_->input_reference())) {
        ESP_LOGW(TAG, "Device AEC requires a playback reference channel");
    }
    UpdateAecState();
}

bool AfeAudioEngine::HasWakeWord() const {
    return wake_detector_ != WakeDetector::kNone;
}

bool AfeAudioEngine::IsWakeWordDetectionEnabled() const {
    return event_group_ != nullptr && (xEventGroupGetBits(event_group_) & kWakeWordEnabled) != 0;
}

bool AfeAudioEngine::IsVoiceProcessingEnabled() const {
    return event_group_ != nullptr && (xEventGroupGetBits(event_group_) & kVoiceProcessingEnabled) != 0;
}

size_t AfeAudioEngine::GetFeedSize() const {
    return afe_data_ == nullptr ? frame_samples_ : afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioEngine::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = std::move(callback);
}

void AfeAudioEngine::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = std::move(callback);
}

void AfeAudioEngine::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = std::move(callback);
}

void AfeAudioEngine::UpdateActiveState() {
    EventBits_t bits = xEventGroupGetBits(event_group_);
    const bool afe_active = afe_data_ != nullptr &&
        ((bits & kWakeWordEnabled) ||
         (kUseAfeForVoiceProcessing && (bits & kVoiceProcessingEnabled)));
    if (afe_active) {
        xEventGroupSetBits(event_group_, kAfeActive);
    } else {
        xEventGroupClearBits(event_group_, kAfeActive);
        control_generation_.fetch_add(1);
        std::lock_guard<std::mutex> lock(input_buffer_mutex_);
        input_buffer_.clear();
        if (afe_data_ != nullptr) {
            // Don't call reset_buffer() here: this runs in the main task while
            // ProcessingTask may be inside fetch_with_delay() on the same AFE
            // instance, and a concurrent reset corrupts the ring buffer state.
            // Defer the reset to ProcessingTask, which owns the fetch side.
            reset_pending_ = true;
        }
    }
    if ((bits & kVoiceProcessingEnabled) == 0) {
        // output_buffer_ is owned by the task that produces output frames,
        // so let that task clear it instead of racing with it here.
        output_reset_pending_ = true;
    }
    afe_control_dirty_ = true;
}

void AfeAudioEngine::UpdateAecState() {
    if (afe_data_ == nullptr || codec_ == nullptr || !codec_->input_reference()) {
        return;
    }
    afe_control_dirty_ = true;
}

void AfeAudioEngine::ApplyAfeControls() {
    EventBits_t bits = xEventGroupGetBits(event_group_);
    if (wake_detector_ == WakeDetector::kWakeNet) {
        if (bits & kWakeWordEnabled) {
            afe_iface_->enable_wakenet(afe_data_);
        } else {
            afe_iface_->disable_wakenet(afe_data_);
        }
    }
    if (codec_->input_reference()) {
        /* 2026-11-15 唤醒灵敏度②：AEC 只在「真正收音/对话中」开。
           原因：待机只跑唤醒词时没有播放，参考通道是静音，AEC 没有回声可消，
           却仍会对麦克风信号做抑制——本机实测远场要"说很慢"才醒，而同门限的
           另一台（无 AEC 参考通道）正常，差异就在这里。
           ⚠️ 影响面：仅"待机检测唤醒词"这一段不再过 AEC；对话中
           （kVoiceProcessingEnabled）仍然按用户设置开 AEC，回声抑制不受影响。 */
        const bool enable_aec =
            kUseAfeForVoiceProcessing && device_aec_enabled_.load() && (bits & kVoiceProcessingEnabled);
        if (enable_aec) {
            afe_iface_->enable_aec(afe_data_);
        } else {
            afe_iface_->disable_aec(afe_data_);
        }
    }
}

void AfeAudioEngine::ApplyPendingReset() {
    if (!reset_pending_.exchange(false)) {
        return;
    }
    // Discard audio recorded before (re)activation. Holding input_buffer_mutex_
    // serializes the reset against Feed(); fetch/reset both run in this task.
    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    input_buffer_.clear();
    afe_iface_->reset_buffer(afe_data_);
}

void AfeAudioEngine::ProcessingTask() {
    while (true) {
        xEventGroupWaitBits(event_group_, kAfeActive, pdFALSE, pdTRUE, portMAX_DELAY);
        ApplyPendingReset();
        if ((xEventGroupGetBits(event_group_) & kAfeActive) == 0) {
            continue;
        }
        if (afe_control_dirty_.exchange(false)) {
            // WakeNet/AEC toggles are not safe against a concurrent fetch,
            // so they are applied here, in the task that owns the fetch side.
            ApplyAfeControls();
        }
        const uint32_t generation = control_generation_.load();
        auto* result = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if (generation != control_generation_.load() ||
            (xEventGroupGetBits(event_group_) & kAfeActive) == 0) {
            // A disable/re-enable may make an old blocked fetch return after the
            // AFE is active again. Reset immediately and never process that frame.
            ApplyPendingReset();
            continue;
        }
        if (result == nullptr || result->ret_value == ESP_FAIL) {
            if (result != nullptr) {
                ESP_LOGW(TAG, "AFE fetch failed: %d", result->ret_value);
            }
            continue;
        }

        EventBits_t bits = xEventGroupGetBits(event_group_);
        if (bits & kWakeWordEnabled) {
            HandleWakeWordResult(result);
        }
        if (kUseAfeForVoiceProcessing && (bits & kVoiceProcessingEnabled)) {
            HandleVoiceResult(result);
        }
    }
}

void AfeAudioEngine::HandleWakeWordResult(const afe_fetch_result_t* result) {
    if (wake_detector_ == WakeDetector::kMultiNet) {
        custom_wake_word_->FeedMono(
            result->data, result->data_size / sizeof(int16_t));
        return;
    }

#if CONFIG_SEND_WAKE_WORD_DATA
    wake_word_audio_cache_.Store(result->data, result->data_size / sizeof(int16_t));
#endif
    if (result->wakeup_state != WAKENET_DETECTED) {
        return;
    }

    int model_index = result->wakenet_model_index - 1;
    if (model_index < 0 || model_index >= static_cast<int>(wake_words_.size())) {
        ESP_LOGE(TAG, "Invalid WakeNet model index: %d", result->wakenet_model_index);
        return;
    }

    last_detected_wake_word_ = wake_words_[model_index];
    xEventGroupClearBits(event_group_, kWakeWordEnabled);
    // UpdateActiveState marks the AFE controls dirty; the next loop iteration
    // of ProcessingTask disables WakeNet via ApplyAfeControls.
    UpdateActiveState();
    if (wake_word_detected_callback_) {
        wake_word_detected_callback_(last_detected_wake_word_);
    }
}

void AfeAudioEngine::HandleVoiceResult(const afe_fetch_result_t* result) {
    if (output_reset_pending_.exchange(false)) {
        output_buffer_.clear();
    }
    if (vad_state_change_callback_) {
        if (result->vad_state == VAD_SPEECH && !is_speaking_) {
            is_speaking_ = true;
            vad_state_change_callback_(true);
        } else if (result->vad_state == VAD_SILENCE && is_speaking_) {
            is_speaking_ = false;
            vad_state_change_callback_(false);
        }
    }
    if (!output_callback_) {
        return;
    }

    size_t samples = result->data_size / sizeof(int16_t);
    output_buffer_.insert(output_buffer_.end(), result->data, result->data + samples);
    while (output_buffer_.size() >= static_cast<size_t>(frame_samples_)) {
        if (output_buffer_.size() == static_cast<size_t>(frame_samples_)) {
            output_callback_(std::move(output_buffer_));
            output_buffer_.clear();
            output_buffer_.reserve(frame_samples_);
        } else {
            output_callback_(std::vector<int16_t>(
                output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
            output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
        }
    }
}

void AfeAudioEngine::OutputRawAudio(const std::vector<int16_t>& data) {
    if (!output_callback_ || codec_ == nullptr) {
        return;
    }
    if (output_reset_pending_.exchange(false)) {
        output_buffer_.clear();
    }
    const size_t channels = codec_->input_channels();
    if (channels <= 1) {
        output_buffer_.insert(output_buffer_.end(), data.begin(), data.end());
    } else {
        for (size_t i = 0; i < data.size(); i += channels) {
            output_buffer_.push_back(data[i]);
        }
    }
    while (output_buffer_.size() >= static_cast<size_t>(frame_samples_)) {
        output_callback_(std::vector<int16_t>(
            output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
        output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
    }
}

void AfeAudioEngine::EncodeWakeWordData() {
    if (wake_detector_ == WakeDetector::kMultiNet) {
        custom_wake_word_->EncodeWakeWordData();
        return;
    }
    if (wake_detector_ != WakeDetector::kWakeNet) {
        return;
    }

    const size_t stack_size = 4096 * 6;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = static_cast<StackType_t*>(
            heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM));
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = static_cast<StaticTask_t*>(
            heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto* engine = static_cast<AfeAudioEngine*>(arg);
        auto start_time = esp_timer_get_time();
        esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
        void* encoder_handle = nullptr;
        auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
        if (encoder_handle == nullptr) {
            ESP_LOGE(TAG, "Failed to create wake-word encoder: %d", ret);
            engine->wake_word_audio_cache_.Clear();
            {
                std::lock_guard<std::mutex> lock(engine->wake_word_mutex_);
                engine->wake_word_opus_.emplace_back();
                engine->wake_word_cv_.notify_all();
            }
            vTaskDelete(nullptr);
            return;
        }

        int frame_size = 0;
        int outbuf_size = 0;
        esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
        frame_size /= sizeof(int16_t);
        int packets = 0;
        std::vector<int16_t> input(frame_size);
        esp_audio_enc_in_frame_t in = {};
        esp_audio_enc_out_frame_t out = {};

        const size_t cached_samples = engine->wake_word_audio_cache_.Size();
        for (size_t offset = 0;
             offset + static_cast<size_t>(frame_size) <= cached_samples;
             offset += frame_size) {
            if (engine->wake_word_audio_cache_.Read(
                    offset, input.data(), frame_size) != static_cast<size_t>(frame_size)) {
                break;
            }
            std::vector<uint8_t> opus_buf(outbuf_size);
            in.buffer = reinterpret_cast<uint8_t*>(input.data());
            in.len = frame_size * sizeof(int16_t);
            out.buffer = opus_buf.data();
            out.len = outbuf_size;
            out.encoded_bytes = 0;
            ret = esp_opus_enc_process(encoder_handle, &in, &out);
            if (ret == ESP_AUDIO_ERR_OK) {
                std::lock_guard<std::mutex> lock(engine->wake_word_mutex_);
                engine->wake_word_opus_.emplace_back(
                    opus_buf.data(), opus_buf.data() + out.encoded_bytes);
                engine->wake_word_cv_.notify_all();
                ++packets;
            } else {
                ESP_LOGE(TAG, "Failed to encode wake-word audio: %d", ret);
            }
        }

        engine->wake_word_audio_cache_.Clear();
        esp_opus_enc_close(encoder_handle);
        ESP_LOGI(TAG, "Encoded wake word into %d packets in %ld ms", packets,
            static_cast<long>((esp_timer_get_time() - start_time) / 1000));
        {
            std::lock_guard<std::mutex> lock(engine->wake_word_mutex_);
            engine->wake_word_opus_.emplace_back();
            engine->wake_word_cv_.notify_all();
        }
        vTaskDelete(nullptr);
    }, "encode_wake_word", stack_size, this, 2,
        wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool AfeAudioEngine::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    if (wake_detector_ == WakeDetector::kMultiNet) {
        return custom_wake_word_->GetWakeWordOpus(opus);
    }
    if (wake_detector_ != WakeDetector::kWakeNet) {
        return false;
    }
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() { return !wake_word_opus_.empty(); });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
