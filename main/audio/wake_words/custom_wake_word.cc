#include "custom_wake_word.h"
#include "audio_service.h"
#include "system_info.h"
#include "assets.h"
#include "wake_word_models.h"

#include <esp_log.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <cJSON.h>

#include <cstring>
#include <string>
#include <vector>

#define TAG "CustomWakeWord"

/* ═══ 2026-11-14：多个发音变体 + 拼音音节白名单 ═══════════════════════════
   事故：网页上填 "dou bao doubao  dou bao bao dou bao a"（想加 4 个念法），
        旧代码把**整串**当成一条命令交给 esp_mn_commands_add →
        内部 phrase_init_from_phoneme_cn 查不到音节 → 空指针 →
        开机无限重启（每次都在 AudioService 初始化时崩）。
   现在两道闸：
     ① 按空格/逗号/分号切 token，连续合法音节组成一条变体；
     ② 每个 token 必须命中下面的**无调拼音音节表**，不命中就切断变体并丢弃。
        宁可少认一个词，也绝不让 esp-sr 拿到它会崩的字符串
        （它是查表失败直接解引用 NULL，返回值拦不住，必须预校验）。
   多出来的表项只会让非法串漏过、可能再崩；少一项只会丢变体、不会崩。
   所以这张表按标准汉语拼音音节全量写，宁宽勿窄。 */
namespace {

const char kPinyinSyllables[] =
    " "
    "a ai an ang ao "
    "ba bai ban bang bao bei ben beng bi bian biao bie bin bing bo bu "
    "ca cai can cang cao ce cen ceng cha chai chan chang chao che chen cheng "
    "chi chong chou chu chua chuai chuan chuang chui chun chuo ci cong cou cu "
    "cuan cui cun cuo "
    "da dai dan dang dao de dei den deng di dia dian diao die ding diu dong "
    "dou du duan dui dun duo "
    "e ei en eng er "
    "fa fan fang fei fen feng fo fou fu "
    "ga gai gan gang gao ge gei gen geng gong gou gu gua guai guan guang gui "
    "gun guo "
    "ha hai han hang hao he hei hen heng hong hou hu hua huai huan huang hui "
    "hun huo "
    "ji jia jian jiang jiao jie jin jing jiong jiu ju juan jue jun "
    "ka kai kan kang kao ke ken keng kong kou ku kua kuai kuan kuang kui kun kuo "
    "la lai lan lang lao le lei leng li lia lian liang liao lie lin ling liu lo "
    "long lou lu luan lue lun luo lv lve "
    "ma mai man mang mao me mei men meng mi mian miao mie min ming miu mo mou mu "
    "na nai nan nang nao ne nei nen neng ni nian niang niao nie nin ning niu "
    "nong nou nu nuan nue nuo nv nve "
    "o ou "
    "pa pai pan pang pao pei pen peng pi pian piao pie pin ping po pou pu "
    "qi qia qian qiang qiao qie qin qing qiong qiu qu quan que qun "
    "ran rang rao re ren reng ri rong rou ru rua ruan rui run ruo "
    "sa sai san sang sao se sen seng sha shai shan shang shao she shei shen "
    "sheng shi shou shu shua shuai shuan shuang shui shun shuo si song sou su "
    "suan sui sun suo "
    "ta tai tan tang tao te teng ti tian tiao tie ting tong tou tu tua tuan tui "
    "tun tuo "
    "wa wai wan wang wei wen weng wo wu "
    "xi xia xian xiang xiao xie xin xing xiong xiu xu xuan xue xun "
    "ya yan yang yao ye yi yin ying yo yong you yu yuan yue yun "
    "za zai zan zang zao ze zei zen zeng zha zhai zhan zhang zhao zhe zhei zhen "
    "zheng zhi zhong zhou zhu zhua zhuai zhuan zhuang zhui zhun zhuo zi zong "
    "zou zu zuan zui zun zuo "
    " ";

/* 单个音节：纯小写字母、长度 1~6，且在表里整词命中（表首尾都有空格，
   所以 " a " 不会误匹配到 " ba "）。 */
bool IsValidPinyinToken(const std::string& t) {
    if (t.empty() || t.size() > 6) {
        return false;
    }
    for (char c : t) {
        if (c < 'a' || c > 'z') {
            return false;
        }
    }
    const std::string key = " " + t + " ";
    return strstr(kPinyinSyllables, key.c_str()) != nullptr;
}

/* 把整串拆成若干条合法变体。分隔规则（用户实际写法见事故说明）：
     · 逗号 / 分号 / 2 个以上连续空格 → **强制**分段（一段一个变体）
     · 单个空格 → 音节分隔（一条变体内部）
     · 非法音节（不在白名单）→ 直接丢弃，但不算分隔
     · 一段里音节数 > 4 → 按 3 音节切（"dou bao bao dou bao a"
       → "dou bao bao" + "dou bao a"）；≤ 4 就整段一条
   例子："dou bao doubao  dou bao bao dou bao a"
        → ["dou bao", "dou bao bao", "dou bao a"]   （doubao 被丢） */
std::vector<std::string> SplitValidVariants(const std::string& raw) {
    /* ① 规范化：把所有强制分隔符统一成 '|' */
    std::string norm;
    norm.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == ',' || c == ';' || c == '\t') {
            norm += '|';
            continue;
        }
        if (c == ' ') {
            size_t j = i;
            while (j < raw.size() && raw[j] == ' ') {
                ++j;
            }
            norm += ((j - i) >= 2) ? '|' : ' ';
            i = j - 1;
            continue;
        }
        norm += c;
    }

    std::vector<std::string> out;
    auto join = [](const std::vector<std::string>& v, size_t from, size_t n) {
        std::string s;
        for (size_t k = 0; k < n; ++k) {
            if (!s.empty()) {
                s += ' ';
            }
            s += v[from + k];
        }
        return s;
    };

    /* ② 逐段处理 */
    size_t start = 0;
    while (true) {
        size_t bar = norm.find('|', start);
        if (bar == std::string::npos) {
            bar = norm.size();
        }

        std::vector<std::string> sylls;
        std::string tok;
        for (size_t i = start; i <= bar; ++i) {
            if (i == bar || norm[i] == ' ') {
                if (!tok.empty()) {
                    if (IsValidPinyinToken(tok)) {
                        sylls.push_back(tok);
                    } else {
                        ESP_LOGW(TAG, "丢弃非法音节「%s」（不在拼音表里）", tok.c_str());
                    }
                    tok.clear();
                }
            } else {
                tok.push_back(norm[i]);
            }
        }

        if (!sylls.empty()) {
            /* ⚠️ 2026-11-14 修正：**绝不自动切分**。
               上一版为了猜 "dou bao bao dou bao a" 的意图，加了「>4 音节按 3 音节切」，
               结果把用户真正的 "ni hao dou bao er"（你好豆包儿）劈成
               "ni hao dou" + "bao er" —— 两条都不是人会喊的话，等于唤醒词直接失效，
               用户看到的现象是「设备卡死、叫不应」。
               变体边界只能由用户显式给出的分隔符决定（逗号/分号/2 个以上空格）。
               单条过长就整条丢弃并告警，不做任何猜测性改写。 */
            if (sylls.size() <= 12) {
                out.push_back(join(sylls, 0, sylls.size()));
            } else {
                ESP_LOGW(TAG, "变体过长（%u 音节），已丢弃：%s", (unsigned)sylls.size(),
                         join(sylls, 0, sylls.size()).c_str());
            }
        }

        if (bar >= norm.size()) {
            break;
        }
        start = bar + 1;
    }

    /* esp-sr 的 ESP_MN_MAX_PHRASE_NUM 有限；4 条足够覆盖常见念法 */
    if (out.size() > 4) {
        out.resize(4);
    }
    return out;
}

/* 一条完整命令是否安全：每个音节都必须在白名单里。
   用于最后一道闸，覆盖 assets index.json 这种「不是我们写进去的」来源。 */
bool IsSafePhrase(const std::string& p) {
    if (p.empty()) {
        return false;
    }
    std::string token;
    for (char c : p) {
        if (c == ' ') {
            if (!token.empty() && !IsValidPinyinToken(token)) {
                return false;
            }
            token.clear();
        } else {
            token.push_back(c);
        }
    }
    return token.empty() || IsValidPinyinToken(token);
}

}  // namespace

CustomWakeWord::CustomWakeWord()
    : wake_word_opus_() {
}

CustomWakeWord::~CustomWakeWord() {
    if (multinet_model_data_ != nullptr && multinet_ != nullptr) {
        multinet_->destroy(multinet_model_data_);
        multinet_model_data_ = nullptr;
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
}

void CustomWakeWord::ParseWakenetModelConfig() {
    // Read index.json
    auto& assets = Assets::GetInstance();
    void* ptr = nullptr;
    size_t size = 0;
    if (!assets.GetAssetData("index.json", ptr, size)) {
        ESP_LOGE(TAG, "Failed to read index.json");
        return;
    }
    cJSON* root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse index.json");
        return;
    }
    cJSON* multinet_model = cJSON_GetObjectItem(root, "multinet_model");
    if (cJSON_IsObject(multinet_model)) {
        cJSON* language = cJSON_GetObjectItem(multinet_model, "language");
        cJSON* duration = cJSON_GetObjectItem(multinet_model, "duration");
        cJSON* threshold = cJSON_GetObjectItem(multinet_model, "threshold");
        cJSON* commands = cJSON_GetObjectItem(multinet_model, "commands");
        if (cJSON_IsString(language)) {
            language_ = language->valuestring;
        }
        if (cJSON_IsNumber(duration)) {
            duration_ = duration->valueint;
        }
        if (cJSON_IsNumber(threshold)) {
            threshold_ = threshold->valuedouble;
        }
        if (cJSON_IsArray(commands)) {
            for (int i = 0; i < cJSON_GetArraySize(commands); i++) {
                cJSON* command = cJSON_GetArrayItem(commands, i);
                if (cJSON_IsObject(command)) {
                    cJSON* command_name = cJSON_GetObjectItem(command, "command");
                    cJSON* text = cJSON_GetObjectItem(command, "text");
                    cJSON* action = cJSON_GetObjectItem(command, "action");
                    if (cJSON_IsString(command_name) && cJSON_IsString(text) && cJSON_IsString(action)) {
                        commands_.push_back({command_name->valuestring, text->valuestring, action->valuestring});
                        ESP_LOGI(TAG, "Command: %s, Text: %s, Action: %s", command_name->valuestring, text->valuestring, action->valuestring);
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
}


bool CustomWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;
    commands_.clear();

    if (models_list == nullptr) {
        language_ = "cn";
        models_ = esp_srmodel_init("model");
        owns_models_ = models_ != nullptr;
#ifdef CONFIG_CUSTOM_WAKE_WORD
        threshold_ = CONFIG_CUSTOM_WAKE_WORD_THRESHOLD / 100.0f;
        commands_.push_back({CONFIG_CUSTOM_WAKE_WORD, CONFIG_CUSTOM_WAKE_WORD_DISPLAY, "wake"});
#endif
    } else {
        models_ = models_list;
        ParseWakenetModelConfig();
    }

    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }

    /* 网页上设置的自定义词 / 灵敏度（重启后生效）优先于 assets index.json 里的默认值。
       esp_mn_commands_add 需要拼音（如 "xiao tu dou"），中文只用于显示和问候语。 */
    const std::string ovr_cmd = wakeword::GetCustomText();
    if (!ovr_cmd.empty()) {
        if (!wakeword::IsValidCommand(ovr_cmd)) {
            /* 传中文进 esp_mn_commands_add 会在 phrase_init_from_phoneme_cn 里空指针崩溃
               → 开机无限重启。这里直接丢弃，沿用 assets 里的默认词，保证能开机。 */
            ESP_LOGW(TAG, "设置里的自定义词「%s」不是拼音，已忽略（沿用镜像默认词）",
                     ovr_cmd.c_str());
        } else {
            std::string display = wakeword::GetCustomDisplay();
            if (display.empty()) {
                display = ovr_cmd;
            }
            /* 2026-11-14：拆成多条发音变体，逐条过音节白名单（见文件头事故说明）。
               一条都拆不出来的话保持 assets 里的默认词不动 —— 绝不能把
               esp-sr 会崩的字符串交给 esp_mn_commands_add。 */
            const auto variants = SplitValidVariants(ovr_cmd);
            if (variants.empty()) {
                ESP_LOGW(TAG, "设置里的自定义词「%s」没有合法拼音音节，已忽略（沿用镜像默认词）",
                         ovr_cmd.c_str());
            } else {
                commands_.clear();
                for (const auto& v : variants) {
                    commands_.push_back({v, display, "wake"});
                    ESP_LOGI(TAG, "自定义唤醒词变体: %s (%s)", v.c_str(), display.c_str());
                }
            }
        }
    }
    threshold_ = wakeword::GetThreshold() / 100.0f;

    // 初始化 multinet (命令词识别)
    mn_name_ = esp_srmodel_filter(models_, ESP_MN_PREFIX, language_.c_str());
    if (mn_name_ == nullptr) {
        ESP_LOGW(TAG, "Language '%s' multinet not found, falling back to any multinet model", language_.c_str());
        mn_name_ = esp_srmodel_filter(models_, ESP_MN_PREFIX, NULL);
    }
    if (mn_name_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize multinet, mn_name is nullptr");
        ESP_LOGI(TAG, "Please refer to https://pcn7cs20v8cr.feishu.cn/wiki/CpQjwQsCJiQSWSkYEvrcxcbVnwh to add custom wake word");
        return false;
    }

    multinet_ = esp_mn_handle_from_name(mn_name_);
    multinet_model_data_ = multinet_->create(mn_name_, duration_);
    multinet_->set_det_threshold(multinet_model_data_, threshold_);
    input_buffer_.reserve(multinet_->get_samp_chunksize(multinet_model_data_));
    /* ⚠️ 最后一道闸（2026-11-14）：不管来源是网页设置还是 assets 的 index.json，
       非法拼音串一律剔除。必须在 esp_mn_commands_add 之前做 ——
       它内部 phrase_init_from_phoneme_cn 查表失败是直接解引用 NULL，崩了就救不回来。
       而且只能改数组、不能只跳过：detect 回调里用 commands_[command_id-1] 取显示名，
       id 和下标必须严格对应。 */
    for (auto it = commands_.begin(); it != commands_.end();) {
        if (!IsSafePhrase(it->command)) {
            ESP_LOGW(TAG, "丢弃非法命令词「%s」（含拼音音节表外的音节）", it->command.c_str());
            it = commands_.erase(it);
        } else {
            ++it;
        }
    }
    if (commands_.empty()) {
        ESP_LOGE(TAG, "没有一条合法唤醒词，MultiNet 不会工作（请到网页重设）");
    }

    esp_mn_commands_clear();
    for (int i = 0; i < commands_.size(); i++) {
        esp_mn_commands_add(i + 1, commands_[i].command.c_str());
    }
    esp_mn_commands_update();
    
    multinet_->print_active_speech_commands(multinet_model_data_);
#if CONFIG_SEND_WAKE_WORD_DATA
    if (!wake_word_audio_cache_.Initialize(16000 * 2)) {
        ESP_LOGW(TAG, "Wake-word audio upload disabled: PSRAM cache allocation failed");
    }
#endif
    return true;
}

void CustomWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void CustomWakeWord::Start() {
    running_ = true;
}

void CustomWakeWord::Stop() {
    running_ = false;

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    input_buffer_.clear();
}

void CustomWakeWord::Feed(const std::vector<int16_t>& data) {
    FeedSamples(data.data(), data.size(), false);
}

void CustomWakeWord::FeedMono(const int16_t* data, size_t samples) {
    FeedSamples(data, samples, true);
}

void CustomWakeWord::FeedSamples(const int16_t* data, size_t samples, bool mono) {
    if (multinet_model_data_ == nullptr || data == nullptr || samples == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    // Check running state inside lock to avoid TOCTOU race with Stop()
    if (!running_) {
        return;
    }

    // If input channels is 2, we need to fetch the left channel data
    if (!mono && codec_->input_channels() > 1) {
        for (size_t i = 0; i < samples; i += codec_->input_channels()) {
            input_buffer_.push_back(data[i]);
        }
    } else {
        input_buffer_.insert(input_buffer_.end(), data, data + samples);
    }
    
    int chunksize = multinet_->get_samp_chunksize(multinet_model_data_);
    while (input_buffer_.size() >= chunksize) {
#if CONFIG_SEND_WAKE_WORD_DATA
        wake_word_audio_cache_.Store(input_buffer_.data(), chunksize);
#endif

        esp_mn_state_t mn_state = multinet_->detect(multinet_model_data_, input_buffer_.data());
        
        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn_result = multinet_->get_results(multinet_model_data_);
            for (int i = 0; i < mn_result->num && running_; i++) {
                ESP_LOGI(TAG, "Custom wake word detected: command_id=%d, string=%s, prob=%f", 
                        mn_result->command_id[i], mn_result->string, mn_result->prob[i]);
                auto& command = commands_[mn_result->command_id[i] - 1];
                if (command.action == "wake") {
                    last_detected_wake_word_ = command.text;
                    running_ = false;
                    input_buffer_.clear();
                    
                    if (wake_word_detected_callback_) {
                        wake_word_detected_callback_(last_detected_wake_word_);
                    }
                }
            }
            multinet_->clean(multinet_model_data_);
        } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGD(TAG, "Command word detection timeout, cleaning state");
            multinet_->clean(multinet_model_data_);
        }
        
        if (!running_) {
            break;
        }
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunksize);
    }
}

size_t CustomWakeWord::GetFeedSize() {
    if (multinet_model_data_ == nullptr) {
        return 0;
    }
    return multinet_->get_samp_chunksize(multinet_model_data_);
}

void CustomWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 7;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (CustomWakeWord*)arg;
        {
            auto start_time = esp_timer_get_time();
            // Create encoder
            esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
            void* encoder_handle = nullptr;
            auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &encoder_handle);
            if (encoder_handle == nullptr) {
                ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
                this_->wake_word_audio_cache_.Clear();
                std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                this_->wake_word_opus_.push_back(std::vector<uint8_t>());
                this_->wake_word_cv_.notify_all();
                vTaskDelete(nullptr);
                return;
            }
            // Get frame size
            int frame_size = 0;
            int outbuf_size = 0;
            esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
            frame_size = frame_size / sizeof(int16_t);
            // Encode all PCM data
            int packets = 0;
            std::vector<int16_t> in_buffer(frame_size);
            esp_audio_enc_in_frame_t in = {};
            esp_audio_enc_out_frame_t out = {};
            const size_t cached_samples = this_->wake_word_audio_cache_.Size();
            for (size_t offset = 0;
                 offset + static_cast<size_t>(frame_size) <= cached_samples;
                 offset += frame_size) {
                if (this_->wake_word_audio_cache_.Read(
                        offset, in_buffer.data(), frame_size) != static_cast<size_t>(frame_size)) {
                    break;
                }
                std::vector<uint8_t> opus_buf(outbuf_size);
                in.buffer = reinterpret_cast<uint8_t*>(in_buffer.data());
                in.len = frame_size * sizeof(int16_t);
                out.buffer = opus_buf.data();
                out.len = outbuf_size;
                out.encoded_bytes = 0;
                ret = esp_opus_enc_process(encoder_handle, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                    this_->wake_word_opus_.emplace_back(opus_buf.data(), opus_buf.data() + out.encoded_bytes);
                    this_->wake_word_cv_.notify_all();
                    packets++;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            }
            this_->wake_word_audio_cache_.Clear();
            // Close encoder
            esp_opus_enc_close(encoder_handle);
            auto end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "Encode wake word opus %d packets in %ld ms", packets, (long)((end_time - start_time) / 1000));

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(NULL);
    }, "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_, wake_word_encode_task_buffer_);
}

bool CustomWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() {
        return !wake_word_opus_.empty();
    });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
