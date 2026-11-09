#include "mqtt_protocol.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "wifi_manager.h"
#include "lwip/stats.h"

#include <esp_log.h>
#include <arpa/inet.h>
#include <cstring>
#include "assets/lang_config.h"

#define TAG "MQTT"

/* 乱序重排窗深度（2026-11-07 卡顿修复）：3 包 ≈ 180ms。
   真实 UDP 乱序一般在 20~50ms 内回来，3 包足够覆盖；
   超过它还等不到的缺口按真丢包处理（补静音帧，流不断）。 */
static constexpr size_t kReorderHoldPackets = 3;

MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();

    // Initialize reconnect timer
    esp_timer_create_args_t reconnect_timer_args = {
        .callback =
            [](void* arg) {
                MqttProtocol* protocol = (MqttProtocol*)arg;
                auto& app = Application::GetInstance();
                /* ⚠️ 2026-11-07 修复「放一会就再也连不上」：旧逻辑只在 Idle
                   才重连 —— 正忙（播音乐/对话）时断线就直接跳过，且不排下一
                   次，设备从此离线到用户手动唤醒。现在忙时也继续排 10s 后
                   再试，直到回到 Idle 完成重连。 */
                if (app.GetDeviceState() == kDeviceStateIdle) {
                    ESP_LOGI(TAG, "Reconnecting to MQTT server");
                    auto alive = protocol->alive_;  // Capture alive flag
                    app.Schedule([protocol, alive]() {
                        if (*alive) {
                            protocol->StartMqttClient(false);
                        }
                    });
                } else {
                    esp_timer_start_once(protocol->reconnect_timer_,
                                         MQTT_RECONNECT_INTERVAL_MS * 1000);
                }
            },
        .arg = this,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
}

MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "MqttProtocol deinit");

    // Mark as dead first to prevent any pending scheduled tasks from executing
    *alive_ = false;

    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }

    std::unique_ptr<Udp> udp;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp = std::move(udp_);
    }
    udp.reset();
    mqtt_.reset();

    {
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        if (aes_key_id_ != PSA_KEY_ID_NULL) {
            psa_destroy_key(aes_key_id_);
            aes_key_id_ = PSA_KEY_ID_NULL;
        }
    }

    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool MqttProtocol::Start() { return StartMqttClient(false); }

bool MqttProtocol::StartMqttClient(bool report_error) {
    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Mqtt client already started");
        mqtt_.reset();
    }

    Settings settings("mqtt", false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 240);
    publish_topic_ = settings.GetString("publish_topic");

    if (endpoint.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(0);
    mqtt_->SetKeepAlive(keepalive_interval);

    mqtt_->OnDisconnected([this]() {
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        ESP_LOGI(TAG, "MQTT disconnected, schedule reconnect in %d seconds",
                 MQTT_RECONNECT_INTERVAL_MS / 1000);
        esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
    });

    mqtt_->OnConnected([this]() {
        if (on_connected_ != nullptr) {
            on_connected_();
        }
        esp_timer_stop(reconnect_timer_);
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGE(TAG, "Message type is invalid");
            cJSON_Delete(root);
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            ESP_LOGI(TAG, "Received goodbye message, session_id: %s",
                     cJSON_IsString(session_id) ? session_id->valuestring : "null");
            if (cJSON_IsString(session_id) && session_id_ == session_id->valuestring) {
                auto alive = alive_;  // Capture alive flag
                Application::GetInstance().Schedule([this, alive]() {
                    if (*alive) {
                        // Server initiated goodbye, don't send goodbye back to avoid ping-pong
                        CloseAudioChannel(false);
                    }
                });
            }
        } else if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    ESP_LOGI(TAG, "Connecting to endpoint %s", endpoint.c_str());
    std::string broker_address;
    int broker_port = 8883;
    size_t pos = endpoint.find(':');
    if (pos != std::string::npos) {
        broker_address = endpoint.substr(0, pos);
        broker_port = std::stoi(endpoint.substr(pos + 1));
    } else {
        broker_address = endpoint;
    }
    if (!mqtt_->Connect(broker_address, broker_port, client_id, username, password)) {
        ESP_LOGE(TAG, "Failed to connect to endpoint, code=%d", mqtt_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        /* ⚠️ 2026-11-07：连接失败绝不能一死了之 —— 10 秒后再试，
           否则一次网络抖动 = 永久离线（用户实测「放一会连不上服务器」）。 */
        esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
        return false;
    }

    ESP_LOGI(TAG, "Connected to endpoint");
    return true;
}

bool MqttProtocol::SendText(const std::string& text) {
    if (publish_topic_.empty()) {
        return false;
    }
    if (!mqtt_->Publish(publish_topic_, text)) {
        ESP_LOGE(TAG, "Failed to publish message: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ == nullptr) {
        return false;
    }

    constexpr size_t kAudioHeaderSize = 16;
    if (aes_nonce_.size() != kAudioHeaderSize || packet->payload.size() > UINT16_MAX) {
        ESP_LOGE(TAG, "Invalid AES nonce or audio payload length: %zu", packet->payload.size());
        return false;
    }

    std::string nonce(aes_nonce_);
    const uint16_t payload_len = htons(static_cast<uint16_t>(packet->payload.size()));
    const uint32_t timestamp = htonl(packet->timestamp);
    const uint32_t sequence = htonl(++local_sequence_);
    memcpy(nonce.data() + 2, &payload_len, sizeof(payload_len));
    memcpy(nonce.data() + 8, &timestamp, sizeof(timestamp));
    memcpy(nonce.data() + 12, &sequence, sizeof(sequence));

    std::string encrypted;
    encrypted.resize(aes_nonce_.size() + packet->payload.size());
    memcpy(encrypted.data(), nonce.data(), nonce.size());

    if (!CryptAesCtr(reinterpret_cast<const uint8_t*>(packet->payload.data()),
                     packet->payload.size(), reinterpret_cast<const uint8_t*>(nonce.data()),
                     reinterpret_cast<uint8_t*>(&encrypted[nonce.size()]))) {
        ESP_LOGE(TAG, "Failed to encrypt audio data");
        return false;
    }

    return udp_->Send(encrypted) > 0;
}

void MqttProtocol::CloseAudioChannel(bool send_goodbye) {
    std::unique_ptr<Udp> udp;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp = std::move(udp_);
    }
    udp.reset();

    ESP_LOGI(TAG, "Closing audio channel, send_goodbye: %d", send_goodbye);

    // Only send goodbye when client initiates the close
    // Don't send if server already sent goodbye (to avoid ping-pong)
    if (send_goodbye) {
        std::string message = "{";
        message += "\"session_id\":\"" + session_id_ + "\",";
        message += "\"type\":\"goodbye\"";
        message += "}";
        SendText(message);
    }

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    auto udp = network->CreateUdp(2);
    udp->OnMessage([this](const std::string& data) {
        /*
         * UDP Encrypted OPUS Packet Format:
         * |type 1u|flags 1u|payload_len 2u|ssrc 4u|timestamp 4u|sequence 4u|
         * |payload payload_len|
         */
        constexpr size_t kAudioHeaderSize = 16;
        if (data.size() < kAudioHeaderSize) {
            ESP_LOGE(TAG, "Invalid audio packet size: %zu", data.size());
            return;
        }
        if (static_cast<uint8_t>(data[0]) != 0x01) {
            ESP_LOGE(TAG, "Invalid audio packet type: %x", static_cast<uint8_t>(data[0]));
            return;
        }
        uint16_t payload_len = 0;
        uint32_t timestamp = 0;
        uint32_t sequence = 0;
        memcpy(&payload_len, data.data() + 2, sizeof(payload_len));
        memcpy(&timestamp, data.data() + 8, sizeof(timestamp));
        memcpy(&sequence, data.data() + 12, sizeof(sequence));
        payload_len = ntohs(payload_len);
        timestamp = ntohl(timestamp);
        sequence = ntohl(sequence);
        if (data.size() != kAudioHeaderSize + payload_len) {
            ESP_LOGE(TAG, "Audio payload length mismatch: header=%u, datagram=%zu",
                     static_cast<unsigned>(payload_len), data.size() - kAudioHeaderSize);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            if (sequence <= remote_sequence_) {
                return;
            }
            /* 跳跃警告挪到下面重排逻辑里，这里只拦老包 */
        }

        const size_t decrypted_size = payload_len;
        auto nonce = reinterpret_cast<const uint8_t*>(data.data());
        auto encrypted = reinterpret_cast<const uint8_t*>(data.data() + kAudioHeaderSize);
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = server_sample_rate_;
        packet->frame_duration = server_frame_duration_;
        packet->timestamp = timestamp;
        packet->payload.resize(decrypted_size);
        if (!CryptAesCtr(encrypted, decrypted_size, nonce,
                         reinterpret_cast<uint8_t*>(packet->payload.data()))) {
            ESP_LOGE(TAG, "Failed to decrypt audio data");
            return;
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(channel_mutex_);
            /* ── 链路判定仪表（2026-11-07）：每 100 包打一条，用数据分清责任方：
               · jumps/saved = 乱序与重排救回；
               · plc = 确认丢失后补的静音帧（听感小空洞）；
               · gaps = 相邻包到达间隔 >150ms 的次数 —— 音频流 60ms 一包，
                 正常到达间隔稳如时钟。gap 多 = 包一阵一阵来（链路抖动或
                 服务器出流慢），gap 少 = 链路平稳；
               · rssi = WiFi 信号强度（<-70dBm 信号差，丢包大概率来自这里）。
               判读：gaps 高+jumps 高 = 网络链路问题；gaps 高+jumps≈0 =
               服务器出流慢；两者都低还卡 = 设备端问题，继续查设备。 */
            static uint32_t stat_pkts = 0, stat_jumps = 0, stat_saved = 0, stat_plc = 0, stat_gaps = 0;
            static std::chrono::steady_clock::time_point stat_last_arrival;
            auto arrival = std::chrono::steady_clock::now();
            if (stat_pkts > 0 && arrival - stat_last_arrival > std::chrono::milliseconds(150)) {
                stat_gaps++;
            }
            stat_last_arrival = arrival;
            stat_pkts++;
            /* ── 乱序重排 + 丢包补偿：按序包立即放行（零额外延迟）；跳跃包
               进重排窗等一等，迟到的按序补放（救回真包）；积压超过窗口
               （≈180ms）才确认缺口真丢 —— 补静音帧保持流连续。 */
            if (sequence <= remote_sequence_) {
                return;   // 老包/重复包，照旧丢弃
            }
            if (sequence == remote_sequence_ + 1) {
                remote_sequence_ = sequence;
                if (on_incoming_audio_ != nullptr) {
                    on_incoming_audio_(std::move(packet));
                }
                /* 把重排窗里现在连续的包按序补放出去 */
                for (;;) {
                    auto hit = reorder_buf_.find(remote_sequence_ + 1);
                    if (hit == reorder_buf_.end()) {
                        break;
                    }
                    remote_sequence_ = hit->first;
                    stat_saved++;
                    if (on_incoming_audio_ != nullptr) {
                        on_incoming_audio_(std::move(hit->second));
                    }
                    reorder_buf_.erase(hit);
                }
                return;
            }
            /* 跳跃：先 hold 住（真乱序的包等它的前驱，别当丢包扔了） */
            stat_jumps += sequence - remote_sequence_ - 1;
            reorder_buf_[sequence] = std::move(packet);
            if (reorder_buf_.size() > kReorderHoldPackets) {
                while (reorder_buf_.size() > 0) {
                    /* 缺口确认丢失：补一帧静音，把序列推进到下一个手上的包 */
                    auto plc = std::make_unique<AudioStreamPacket>();
                    plc->sample_rate = server_sample_rate_;
                    plc->frame_duration = server_frame_duration_;
                    plc->timestamp = 0;
                    plc->payload.clear();   /* 空 payload = 解码层输出静音帧 */
                    remote_sequence_++;
                    stat_plc++;
                    if (on_incoming_audio_ != nullptr) {
                        on_incoming_audio_(std::move(plc));
                    }
                    auto hit = reorder_buf_.find(remote_sequence_);
                    if (hit == reorder_buf_.end()) {
                        break;   /* 下一个还在路上，继续等 */
                    }
                    remote_sequence_ = hit->first;
                    stat_saved++;
                    if (on_incoming_audio_ != nullptr) {
                        on_incoming_audio_(std::move(hit->second));
                    }
                    reorder_buf_.erase(hit);
                }
            }
            if (stat_pkts % 100 == 0) {
                int8_t rssi = (int8_t)WifiManager::GetInstance().GetRssi();
                /* 定责关键：lwIP 协议栈收到的 UDP datagram 增量。
                   若它 ≈ pkts 增量 → 协议栈全收下了，丢包发生在上游（网络/服务器）；
                   若它 明显大于 pkts 增量 → 协议栈收到了但应用层没消化 → 设备端责任。 */
                static uint32_t stat_last_udp_in = 0;
#if CONFIG_LWIP_STATS
                uint32_t udp_in_now = lwip_stats.udp.recv;
                uint32_t udp_in_delta = udp_in_now - stat_last_udp_in;
                stat_last_udp_in = udp_in_now;
#else
                uint32_t udp_in_delta = 0;   /* lwIP 统计未开启 */
#endif
                ESP_LOGI(TAG, "audio stats: pkts=%u(+100) lwip_udp_in=+%d jumps=%u saved=%u plc=%u gaps=%u (%.1f%% loss) rssi=%d",
                         stat_pkts, (int)udp_in_delta, stat_jumps, stat_saved, stat_plc, stat_gaps,
                         stat_pkts > 0 ? 100.0f * stat_plc / (stat_pkts + stat_plc) : 0.0f,
                         (int)rssi);
            }
        }
        /* ⚠️ 这里绝不能再碰 packet：重排逻辑里 packet 已被 move 进回调或
           reorder_buf_，旧代码残留的放行调用在这里拿到的是空指针 ——
           就是它把系统搞崩的（LoadProhibited @ EXCVADDR 0x8）。 */
    });

    if (!udp->Connect(udp_server_, udp_port_)) {
        ESP_LOGE(TAG, "Failed to connect UDP audio channel");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_ = std::move(udp);
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

std::string MqttProtocol::GetHelloMessage() {
    // 发送 hello 消息申请 UDP 通道
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    AddTextFontCapabilities(root);
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void MqttProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported or missing transport");
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    // Get sample rate from hello message
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    auto server = cJSON_GetObjectItem(udp, "server");
    auto port = cJSON_GetObjectItem(udp, "port");
    auto key_item = cJSON_GetObjectItem(udp, "key");
    auto nonce_item = cJSON_GetObjectItem(udp, "nonce");
    if (!cJSON_IsString(server) || !cJSON_IsNumber(port) || port->valueint <= 0 ||
        port->valueint > UINT16_MAX || !cJSON_IsString(key_item) || !cJSON_IsString(nonce_item)) {
        ESP_LOGE(TAG, "Invalid UDP server, port, key, or nonce");
        return;
    }
    const std::string udp_server = server->valuestring;
    const int udp_port = port->valueint;

    // auto encryption = cJSON_GetObjectItem(udp, "encryption")->valuestring;
    // ESP_LOGI(TAG, "UDP server: %s, port: %d, encryption: %s", udp_server_.c_str(), udp_port_,
    // encryption);
    std::string aes_nonce;
    std::string aes_key;
    if (!DecodeHexString(nonce_item->valuestring, aes_nonce) ||
        !DecodeHexString(key_item->valuestring, aes_key) || aes_nonce.size() != 16 ||
        aes_key.size() != 16) {
        ESP_LOGE(TAG, "Invalid AES key or nonce length");
        return;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to initialize PSA Crypto, status: %ld", static_cast<long>(status));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        if (aes_key_id_ != PSA_KEY_ID_NULL) {
            psa_destroy_key(aes_key_id_);
            aes_key_id_ = PSA_KEY_ID_NULL;
        }

        psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
        psa_set_key_algorithm(&attributes, PSA_ALG_CTR);
        psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&attributes, 128);
        status = psa_import_key(&attributes, reinterpret_cast<const uint8_t*>(aes_key.data()),
                                aes_key.size(), &aes_key_id_);
        psa_reset_key_attributes(&attributes);
    }
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Failed to import AES key, status: %ld", static_cast<long>(status));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_server_ = udp_server;
        udp_port_ = udp_port;
        aes_nonce_ = std::move(aes_nonce);
        local_sequence_ = 0;
        remote_sequence_ = 0;
        reorder_buf_.clear();   /* 新通道新序列空间，重排窗里的旧包全作废 */
    }
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

bool MqttProtocol::CryptAesCtr(const uint8_t* input, size_t input_size, const uint8_t* nonce,
                               uint8_t* output) {
    std::lock_guard<std::mutex> lock(crypto_mutex_);
    if (aes_key_id_ == PSA_KEY_ID_NULL || input == nullptr || nonce == nullptr ||
        output == nullptr) {
        return false;
    }

    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
    psa_status_t status = psa_cipher_encrypt_setup(&operation, aes_key_id_, PSA_ALG_CTR);
    if (status == PSA_SUCCESS) {
        status = psa_cipher_set_iv(&operation, nonce, 16);
    }

    size_t output_len = 0;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_update(&operation, input, input_size, output, input_size, &output_len);
    }

    uint8_t finish_output[16];
    size_t finish_len = 0;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(&operation, finish_output, sizeof(finish_output), &finish_len);
    }
    psa_cipher_abort(&operation);

    if (status != PSA_SUCCESS || output_len != input_size || finish_len != 0) {
        ESP_LOGE(TAG, "AES-CTR operation failed, status: %ld", static_cast<long>(status));
        return false;
    }
    return true;
}

static inline int CharToHex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

bool MqttProtocol::DecodeHexString(const std::string& hex_string, std::string& decoded) {
    decoded.clear();
    if ((hex_string.size() % 2) != 0) {
        return false;
    }
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        int high = CharToHex(hex_string[i]);
        int low = CharToHex(hex_string[i + 1]);
        if (high < 0 || low < 0) {
            decoded.clear();
            return false;
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

bool MqttProtocol::IsAudioChannelOpened() const {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}
