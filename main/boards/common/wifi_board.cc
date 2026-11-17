#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <lwip/netdb.h>
#include <utility>

#include <material_symbols.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#endif

static const char *TAG = "WifiBoard";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;

/* ── 2026-11-15 静态 IP（修：以前从来没生效过）────────────────────────────
   原来在 WifiManager.Initialize() 之后立刻查 netif 并设置，但 STA netif 是
   WifiStation::Start() 里才创建的 → 永远拿到 nullptr，日志「STA netif 还没建好」，
   实际一直走 DHCP。实测 DHCP 要 5.8 秒才拿到地址（1720ms 开始扫描 → 9880ms
   才 Got IP），开机到可用 11.3 秒里有 5.8 秒耗在这。
   现在两条腿：
     ① 一次性任务轮询 netif（10ms × 最多 300 次），建好就停 DHCP + 钉死地址；
     ② 注册 STA_CONNECTED 兜底再设一次，并**手动补发 IP_EVENT_STA_GOT_IP** ——
        静态 IP 不走 DHCP，不会自动产生这个事件，不补的话上层永远认为没联网。
   地址挑 .50：家用路由 DHCP 池一般 .100~.199，池外不会撞车。
   想换地址只改下面四个数字。 */
namespace {

constexpr int kStaticIp[4]   = {192, 168, 1, 50};
constexpr int kStaticGw[4]   = {192, 168, 1, 1};
constexpr int kStaticMask[4] = {255, 255, 255, 0};

// 幂等：netif 不存在时返回 false，调用方自己决定重试/放弃
bool ApplyStaticIp() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        return false;
    }
    esp_netif_ip_info_t ip_info = {};
    IP4_ADDR(&ip_info.ip, kStaticIp[0], kStaticIp[1], kStaticIp[2], kStaticIp[3]);
    IP4_ADDR(&ip_info.gw, kStaticGw[0], kStaticGw[1], kStaticGw[2], kStaticGw[3]);
    IP4_ADDR(&ip_info.netmask, kStaticMask[0], kStaticMask[1], kStaticMask[2], kStaticMask[3]);

    // 必须先停 DHCP，否则它拿到的地址会把静态地址覆盖掉。
    // 已经停了会返回错误，属正常，忽略。
    esp_netif_dhcpc_stop(netif);
    if (esp_netif_set_ip_info(netif, &ip_info) != ESP_OK) {
        return false;
    }

    /* ⚠️ 2026-11-15 踩坑：静态 IP 只设 IP/网关/掩码，**DNS 是空的** ——
       DNS 平时由 DHCP 下发，停掉 DHCP 后域名一个都解析不了，
       表现就是「WiFi 连上了、网页能开、但连不上服务器」。
       主 DNS 用网关（家用路由通常自己代理 DNS），备用走阿里公共 DNS。 */
    esp_netif_dns_info_t dns_main = {};
    IP_ADDR4(&dns_main.ip, kStaticGw[0], kStaticGw[1], kStaticGw[2], kStaticGw[3]);
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_main);

    esp_netif_dns_info_t dns_backup = {};
    IP_ADDR4(&dns_backup.ip, 223, 5, 5, 5);
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns_backup);
    return true;
}

/* 兜底：静态 IP 配置错（换网络 / 网关不是 192.168.1.1）时，DNS 会全挂，
   设备表现为「WiFi 连上、网页能开、就是连不上服务器」。
   连上 20 秒后测一次域名解析，失败就退掉静态 IP、重新走 DHCP 并重连，
   宁可 IP 变回动态也不要变成哑巴。 */
/* ⚠️ 2026-11-15 踩坑：任务函数**绝不能 return**！
   FreeRTOS 的任务入口返回地址是 0，return 后 CPU 跳到 0x0，
   表现为 `Guru Meditation Error (IllegalInstruction)` + `A0 = 0x00000000`，
   而且是每 20 秒（本任务的 vTaskDelay）崩一次，看起来像"唤醒就重启"。
   所有出口必须 vTaskDelete(nullptr) 后再返回。 */
void StaticIpHealthTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(20000));
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr || !esp_netif_is_netif_up(netif)) {
        vTaskDelete(nullptr);        // 还没联网，不误判
        return;
    }
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    struct addrinfo* res = nullptr;
    const int err = getaddrinfo("api.tenclass.net", nullptr, &hints, &res);
    if (res != nullptr) {
        freeaddrinfo(res);
    }
    if (err == 0) {
        ESP_LOGI(TAG, "静态 IP 自检通过（域名解析正常）");
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGE(TAG, "静态 IP 下域名解析失败(err=%d)，回退 DHCP 并重连", err);
    esp_netif_dhcpc_start(netif);
    esp_wifi_disconnect();
    esp_wifi_connect();
    vTaskDelete(nullptr);
}

void StaticIpEventHandler(void*, esp_event_base_t base, int32_t id, void*) {
    if (base != WIFI_EVENT || id != WIFI_EVENT_STA_CONNECTED) {
        return;
    }
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr || !ApplyStaticIp()) {
        ESP_LOGE(TAG, "静态 IP 兜底设置失败，回退 DHCP");
        if (netif != nullptr) {
            esp_netif_dhcpc_start(netif);
        }
        return;
    }
    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return;
    }
    ip_event_got_ip_t evt = {};
    evt.esp_netif = netif;
    evt.ip_info = ip_info;
    evt.ip_changed = true;
    esp_event_post(IP_EVENT, IP_EVENT_STA_GOT_IP, &evt, sizeof(evt), 0);
    ESP_LOGI(TAG, "静态 IP 生效: " IPSTR, IP2STR(&ip_info.ip));

    /* 只起一次自检（重连时事件还会再来，不要重复起任务） */
    static bool health_started = false;
    if (!health_started) {
        health_started = true;
        xTaskCreate(StaticIpHealthTask, "static_ip_check", 4096, nullptr, 3, nullptr);
    }
}

void StaticIpPollTask(void*) {
    for (int i = 0; i < 300; ++i) {          // 最多等 3 秒
        if (ApplyStaticIp()) {
            esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ip_info = {};
            if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                ESP_LOGI(TAG, "静态 IP 已钉死: " IPSTR, IP2STR(&ip_info.ip));
            }
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "静态 IP：3 秒内没等到 STA netif，继续走 DHCP");
    vTaskDelete(nullptr);
}

}  // namespace

WifiBoard::WifiBoard() {
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiBoard::~WifiBoard() {
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = "Xiaozhi";
    config.language = Lang::CODE;
    config.show_ota_config = true;
    config.show_sleep_config = true;

    // Set a DHCP hostname so the router shows a friendly name instead of "espressif".
    // Uses the same "<prefix>-<last 2 MAC bytes>" scheme as the config AP SSID.
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        char hostname[32];
        snprintf(hostname, sizeof(hostname), "%s-%02X%02X", config.ssid_prefix.c_str(), mac[4], mac[5]);
        config.station_hostname = hostname;
    }
    wifi_manager.Initialize(config);

    // Set unified event callback - forward to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    // ⚠️ 2026-10-31：这台机器人固定摆在桌上，DHCP 每次重启都换个地址，结果就是
    // 「明明活着却按老地址连不上」，还会让人误以为是重启死机。
    // 2026-11-15 修：原来在这里直接设置，但 netif 还没建（WifiStation::Start 里才建），
    // 一直静默失败走 DHCP。改成轮询任务 + STA_CONNECTED 兜底，见文件头注释。
    {
        static bool static_ip_hooked = false;
        if (!static_ip_hooked) {
            static_ip_hooked = true;
            esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                       &StaticIpEventHandler, nullptr);
            xTaskCreate(StaticIpPollTask, "static_ip", 3072, nullptr, 5, nullptr);
        }
    }

    // Try to connect or enter config mode
    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    /* 配网模式保持期间不反复去连：以前这里会因 WifiConfigModeExit 回调再进来，
       没有 SSID 时又走一遍 StartWifiConfigMode → 再播报一次 AP 提示音。 */
    if (config_mode_started_) {
        ESP_LOGW(TAG, "配网模式保持中，跳过重复连接尝试");
        return;
    }

    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        // Start connection attempt with timeout
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
        // No SSID configured, enter config mode
        // Wait for the board version to be shown
        vTaskDelay(pdMS_TO_TICKS(1500));
        StartWifiConfigMode();
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            // Stop timeout timer
            esp_timer_stop(connect_timer_);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
            // make sure blufi resources has been released
            Blufi::GetInstance().deinit();
#endif
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            /* 只有真正退出配网才允许下一次进入（新凭据去连 / 连失败再回来配网）。
               先清零再 TryWifiConnect，否则会被上面的保持守卫挡掉。 */
            config_mode_started_ = false;
            // Try to connect with the new credentials
            TryWifiConnect();
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);

    /* 已经在配网里就不再做任何动作：既不 StopStation（会再发一次 Disconnected
       让上层状态机抖动），也不重进配网（会重复播报提示音）。 */
    if (board->config_mode_started_ || board->in_config_mode_) {
        ESP_LOGW(TAG, "配网模式已在保持中，忽略连接超时");
        return;
    }

    ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");

    WifiManager::GetInstance().StopStation();
    board->StartWifiConfigMode();
}

void WifiBoard::StartWifiConfigMode() {
    /* 幂等：配网只拉起一次。AP 一旦开起来就常驻，提示音只播一次；
       重复调用（超时重试 / 按键 / 事件回调 / 状态机抖动）全部忽略。 */
    if (config_mode_started_) {
        ESP_LOGW(TAG, "配网模式已在进行中，忽略重复进入（AP 保持）");
        return;
    }
    config_mode_started_ = true;

    in_config_mode_ = true;
    // Transition to wifi configuring state
    Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    auto& wifi_manager = WifiManager::GetInstance();

    wifi_manager.StartConfigAp();

    // Show config prompt after a short delay
    Application::GetInstance().Schedule([&wifi_manager]() {
        std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
        hint += wifi_manager.GetApSsid();
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += wifi_manager.GetApWebUrl();

        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);
    });
#elif CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto &blufi = Blufi::GetInstance();
    // initialize esp-blufi protocol
    blufi.init();
#endif
}

void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode called");

    /* 已经在配网里（AP 常驻、提示音已播过）：不去重复 ResetProtocol / 重开 AP，
       否则按键连点或上层重复调用会表现为「反复进入配网」。 */
    if (config_mode_started_) {
        ESP_LOGW(TAG, "已在配网模式并保持中，忽略重复进入");
        return;
    }

    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);

    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    if (state == kDeviceStateSpeaking || state == kDeviceStateListening || state == kDeviceStateIdle) {
        // Reset protocol (close audio channel, reset protocol)
        Application::GetInstance().ResetProtocol();

        xTaskCreate([](void* arg) {
            auto* board = static_cast<WifiBoard*>(arg);

            // Wait for 1 second to allow speaking to finish gracefully
            vTaskDelay(pdMS_TO_TICKS(1000));

            // Stop any ongoing connection attempt
            esp_timer_stop(board->connect_timer_);
            WifiManager::GetInstance().StopStation();

            // Enter config mode
            board->StartWifiConfigMode();

            vTaskDelete(NULL);
        }, "wifi_cfg_delay", 4096, this, 2, NULL);
        return;
    }

    if (state != kDeviceStateStarting) {
        ESP_LOGE(TAG, "EnterWifiConfigMode called but device state is not starting or speaking, device state: %d", state);
        return;
    }

    // Stop any ongoing connection attempt
    esp_timer_stop(connect_timer_);
    WifiManager::GetInstance().StopStation();

    StartWifiConfigMode();
}

bool WifiBoard::IsInWifiConfigMode() const {
    return WifiManager::GetInstance().IsConfigMode();
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return MATERIAL_SYMBOLS_WIFI;
    }
    if (!wifi.IsConnected()) {
        return MATERIAL_SYMBOLS_WIFI_OFF;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return MATERIAL_SYMBOLS_WIFI;
    } else if (rssi >= -75) {
        return MATERIAL_SYMBOLS_WIFI_2_BAR;
    }
    return MATERIAL_SYMBOLS_WIFI_1_BAR;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";
    json += R"("manufacturer":")" + std::string(BOARD_MANUFACTURER) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    // Chip temperature
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
