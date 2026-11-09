#ifndef MQTT_PROTOCOL_H
#define MQTT_PROTOCOL_H


#include "protocol.h"
#include <mqtt.h>
#include <udp.h>
#include <cJSON.h>
#include <psa/crypto.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include <functional>
#include <string>
#include <map>
#include <mutex>
#include <memory>
#include <atomic>

#define MQTT_PING_INTERVAL_SECONDS 90
/* ⚠️ 2026-11-07：60s → 10s。用户实测「放一会音乐就连不上服务器」——
   断线后干等 60 秒体验极差，10 秒内自动回来。 */
#define MQTT_RECONNECT_INTERVAL_MS 10000

#define MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class MqttProtocol : public Protocol {
public:
    MqttProtocol();
    ~MqttProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    // Alive flag for safe scheduled callbacks - set to false in destructor
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    
    EventGroupHandle_t event_group_handle_;

    std::string publish_topic_;

    mutable std::mutex channel_mutex_;
    std::mutex crypto_mutex_;
    std::unique_ptr<Mqtt> mqtt_;
    std::unique_ptr<Udp> udp_;
    psa_key_id_t aes_key_id_ = PSA_KEY_ID_NULL;
    std::string aes_nonce_;
    std::string udp_server_;
    int udp_port_;
    uint32_t local_sequence_;
    uint32_t remote_sequence_;
    /* 2026-11-07 卡顿修复：UDP 乱序重排窗 —— 迟到的包先在这里等一等，
       缺口确认丢了才放弃（补静音帧）。实测对话期间 ~22% 包丢/乱序，
       旧逻辑把乱序包直接当丢包扔了。 */
    std::map<uint32_t, std::unique_ptr<AudioStreamPacket>> reorder_buf_;
    esp_timer_handle_t reconnect_timer_;

    bool StartMqttClient(bool report_error=false);
    void ParseServerHello(const cJSON* root);
    bool DecodeHexString(const std::string& hex_string, std::string& decoded);
    bool CryptAesCtr(const uint8_t* input, size_t input_size, const uint8_t* nonce, uint8_t* output);

    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};


#endif // MQTT_PROTOCOL_H
