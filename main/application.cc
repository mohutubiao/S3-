#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#include "settings.h"
#include "system_info.h"
#include "text_glyph_payload.h"
#include "websocket_protocol.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <cstring>

#define TAG "Application"

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

    /* 2026-11-14 唤醒加速：空闲关通道定时器。
       回调只做一件事 —— 把真正的关闭动作 Schedule 到主任务，
       因为 protocol_ 的生命周期归主任务管，在 esp_timer 任务里直接动它是隐患。 */
    esp_timer_create_args_t idle_close_args = {
        .callback = [](void* arg) {
            static_cast<Application*>(arg)->Schedule([arg]() {
                auto* app = static_cast<Application*>(arg);
                if (app->protocol_ != nullptr && app->protocol_->IsAudioChannelOpened()) {
                    ESP_LOGI(TAG, "空闲超时，关闭音频通道（下次唤醒需重连）");
                    app->protocol_->CloseAudioChannel();
                }
            });
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "idle_close",
        .skip_unhandled_events = true};
    esp_timer_create(&idle_close_args, &idle_channel_close_timer_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (idle_channel_close_timer_ != nullptr) {
        esp_timer_stop(idle_channel_close_timer_);
        esp_timer_delete(idle_channel_close_timer_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* 2026-11-12 语音打断开关（NVS「voice/interrupt」，默认开）
       2026-11-15：打断与设备端 AEC 绑定成同一个状态。
       **Why:** 打断靠 AEC 把喇叭回声压掉，AI 说话时设备才听得到唤醒词；
       AEC 关着时喊了没反应（用户实测「语音打断没生效」）。以前 AEC 只跟
       编译宏/双击 BOOT 走、打断开关只写自己的 NVS，两者会分叉：网页显示
       「打断开」，实际 AEC 已被关掉，重启后又反过来。
       **现在唯一真源 = voice/interrupt**，重启时按它恢复 AEC。 */
    {
        Settings settings("voice", true);
        voice_interrupt_enabled_ = settings.GetInt("interrupt", 1) != 0;
        ESP_LOGI(TAG, "语音打断: %s（AEC %s）",
                 voice_interrupt_enabled_ ? "开" : "关",
                 voice_interrupt_enabled_ ? "设备端" : "关");
    }
    aec_mode_ = voice_interrupt_enabled_ ? kAecOnDeviceSide : kAecOff;

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();
    /* 2026-11-15：按「语音打断」开关恢复 AEC。此时音频引擎多半还没初始化，
       EnableDeviceAec 会先记住状态，InitializeAudioEngine() 里再真正应用。 */
    audio_service_.EnableDeviceAec(aec_mode_ == kAecOnDeviceSide);

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    // Drop the remaining packets. Leaving them in the queue would
                    // stall the Opus codec task (it waits for queue space), which in
                    // turn deadlocks the whole audio input pipeline, as no new
                    // MAIN_EVENT_SEND_AUDIO event would ever be triggered again.
                    while (audio_service_.PopPacketFromSendQueue())
                        ;
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    /* 2026-11-15：待机省电从 MAX_MODEM 降到 MIN_MODEM。
       理由同 OnAudioChannelClosed —— 深睡会让唤醒建连的 TCP 握手（实测 610ms）
       要等 DTIM 周期。待机多几 mA，换唤醒快一截。
       同日晚：ping 实测本机 RTT 呈 17→110ms 锯齿（网关基线 1~9ms），
       典型 DTIM 休眠特征；TCP 在这种链路上必然队头阻塞，音频成段晚到。
       用户明确优先流畅度 → 彻底关掉省电（PS_NONE 常驻），代价是待机电流上升。 */
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    /* 2026-11-15 用户定案：启动成功音必须等**进入系统之后**才响，
       不能在开机模式选择界面上就"叮"一声（会被误认为别的东西在启动）。
       列表还在等按键 → 押后，由 DafeiyuDisplay::ModeSelectFinish 调
       PlaySuccessSoundIfIdle() 补播；没有列表的板/流程照旧立刻播。 */
    if (display != nullptr && display->IsModeSelectActive()) {
        ESP_LOGI(TAG, "模式选择列表还开着，启动成功音押后播放");
        success_sound_pending_ = true;
    } else {
        Schedule([this]() {
            // Play the success sound to indicate the device is ready
            audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
        });
    }
}

/* 2026-11-15：列表收起后补播被押后的启动成功音（幂等，没押后就不响）。 */
void Application::PlaySuccessSoundIfIdle() {
    if (!success_sound_pending_) {
        return;
    }
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr && display->IsModeSelectActive()) {
        return;   /* 列表又开了（或还没收起），继续等 */
    }
    success_sound_pending_ = false;
    ESP_LOGI(TAG, "模式选择已结束，补播启动成功音");
    Schedule([this]() {
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("robot_2");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;  // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err,
                     ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay,
                     error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;  // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10;  // Reset retry delay

        if (ota_->HasNewVersion()) {
            /* ⚠️ 2026-11-04 双固件保护：OTA 会写到另一个槽（ota_1 = 游戏机），
               自动升级会把 GameBox 刷掉。禁用云升级，只提示版本号。
               固件升级走 USB 烧录（以后再做双槽安全升级）。 */
            ESP_LOGW(TAG, "发现新固件 %s，双固件模式下禁用自动升级（保护游戏机槽）",
                     ota_->GetFirmwareVersion().c_str());
        }
        if (false && ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    /* ⚠️ 2026-11-07 卡顿根治（第二轮干净实验）：WebSocket 优先。
       第一轮失败被误判 —— 当时 CONFIG_LWIP_STATS=y 同时在场（实测它单独
       就吃 ~9-12KB SRAM），变量没隔离。对照实验铁证：C3 板（老固件，
       走 ws 明文通道，无 PSRAM）音频零丢包零饿帧；S3 的 MQTT+UDP 丢
       53.7%。服务器 OTA 同时下发两种配置，ws:// 走明文没有大 TLS 缓冲。
       现在 LWIP_STATS 已关，SRAM 余量 16KB+，重切 WS。MQTT 保留兜底。 */
    if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() { DismissAlert(); });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        /* 2026-11-14 打断后丢弃迟到的 TTS 包（根治残音）：
           abort 消息发出 → 服务器停发 → 状态切到 listening 之间有个来回（几十~几百 ms），
           这段窗口里的包如果进了 decode queue，就会在用户刚说完话时突然冒出一小段
           上个回答的尾巴。aborted_ 在 tts start 时清零（见 577 行）。
           注意：只看状态不够 —— 状态切换是 Schedule 到主任务异步做的，
           而这行跑在协议任务里，中间那段时间状态还是 speaking。 */
        if (aborted_.load()) {
            return;
        }
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG,
                     "Server sample rate %d does not match device output sample rate %d, "
                     "resampling may cause distortion",
                     protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });

    protocol_->OnAudioChannelClosed([this, &board]() {
        /* 2026-11-15 唤醒加速②：LOW_POWER 是 WIFI_PS_MAX_MODEM（最激进的 modem sleep）。
           实测建连日志 `cost=670 tcp=610 handshake=60` —— 610ms 全耗在 TCP 上，
           典型原因就是 WiFi 深睡时 AP 要等 DTIM 周期才转发握手包。
           降到 BALANCED(MIN_MODEM)：待机多花几 mA，换唤醒建连快一截。
           2026-11-15 晚：ping 实测本机 RTT 锯齿 17→110ms（网关 1~9ms），
           MIN_MODEM 仍在睡，TCP 队头阻塞直接造成音频 0.3~1.9s 断档。
           用户优先流畅度 → 常驻 PS_NONE。 */
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no type");
            return;
        }
        /* 2026-11-15 取证日志：把所有下行消息的类型打出来。
           排查「说一句只回 tts stop、没有 sentence_start」时，
           这条能一眼看出服务端到底发了几条、缺哪条。 */
        {
            cJSON* st = cJSON_GetObjectItem(root, "state");
            ESP_LOGI(TAG, "DOWN type=%s state=%s", type->valuestring,
                     cJSON_IsString(st) ? st->valuestring : "-");
        }
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    /* 2026-11-15 修复「按 BOOT 回待机后自己又进对话」（真因是这条）：
                       客户端 exit 只发了 listen stop，服务端手上已经生成的 TTS
                       仍会继续推完。旧代码在这里**无条件**切 Speaking ——
                       于是待机（时钟界面）状态下被一段 tts start 拽进对话，
                       而且会反复循环（日志：idle -> speaking -> listening -> idle
                       每 2~4 秒一轮）。只有确实处于会话中才接受 TTS。 */
                    const auto st = GetDeviceState();
                    if (st == kDeviceStateIdle || st == kDeviceStateStarting ||
                        st == kDeviceStateActivating || st == kDeviceStateWifiConfiguring ||
                        st == kDeviceStateAudioTesting) {
                        ESP_LOGW(TAG, "待机状态收到 tts start，忽略（会话已结束）state=%d", (int)st);
                        audio_service_.SetUplinkSuppress(false);
                        return;
                    }
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                    /* 2026-11-07 AI 开口 → 开上行回声滤除（防服务器误打断碎段） */
                    audio_service_.SetUplinkSuppress(true);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this, display]() {
                    /* 2026-11-06 音乐结束收尾（照搬 C3 小车 application.cc）：
                       TTS 流 stop = 一句话/一首歌播完，退出音乐可视化 */
                    display->SetMusicPlaying(false);
                    /* AI 说完 → 恢复上行，否则用户下一句就传不出去了 */
                    audio_service_.SetUplinkSuppress(false);
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    std::vector<TextGlyph> glyphs;
                    uint8_t bpp = 0;
                    if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                        glyphs.clear();
                    }
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    /* 2026-11-06 音乐检测（照搬 C3 小车）：服务端播 AI 歌曲时，
                       会在歌词之前先下发一条 "% play_music..." 的 sentence_start
                       （搜歌阶段是 "% search_music..."）。音乐本身是当作 TTS 音频流
                       下发的，设备端从音频层面分不出「唱歌」和「说话」，
                       只能靠这条文字标记来识别 —— 比依赖模型调 MCP 工具可靠得多。
                       play_music 之后紧跟着的第一条文本就是歌名（实测顺序：
                       % search_music → % play_music → 《歌名》→ 歌词）。 */
                    const bool is_music =
                        (strstr(text->valuestring, "play_music") != nullptr);
                    static bool music_name_pending = false;
                    const bool grab_name = music_name_pending;
                    music_name_pending = is_music;
                    const bool is_internal_cmd =
                        (text->valuestring[0] == '%');
                    Schedule([display, message = std::string(text->valuestring),
                              glyphs = std::move(glyphs), bpp, is_music,
                              grab_name, is_internal_cmd]() {
                        if (is_music) {
                            /* 切音乐可视化；这条是内部指令，不当聊天文字 */
                            display->SetMusicPlaying(true);
                            return;
                        }
                        if (grab_name) {
                            display->SetSongTitle(message.c_str());
                            return;
                        }
                        display->AddTextGlyphs(glyphs, bpp);
                        if (!is_internal_cmd) {
                            display->SetChatMessage("assistant", message.c_str());
                        }
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring),
                          glyphs = std::move(glyphs), bpp]() {
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() { Reboot(); });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring,
                      Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule(
                    [this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                        display->SetChatMessage("system", payload_str.c_str());
                    });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

void Application::StopListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING); }

/* ── 2026-11-14 唤醒加速（用户实测「按 BOOT 约 1 秒延迟」）──
   旧逻辑：退出 listening 时立刻 protocol_->CloseAudioChannel()，
   下次唤醒就得重走 TCP + TLS 握手 + websocket upgrade + server hello
   ≈ 0.8~1.1 秒（TLS 是最大头）。
   现在：退出 listening 只挂一个 60s 定时器。这期间再唤醒，通道还在，
   直接 SetListeningMode（100~200ms）。60s 没人理才真关，避免白占服务器会话。
   必须立刻关的场景（断网/图传暂停/OTA/切 AEC）保持原样，不走这里。 */
void Application::ScheduleIdleChannelClose() {
    if (idle_channel_close_timer_ == nullptr) {
        return;
    }
    constexpr uint64_t kIdleKeepUs = 60ULL * 1000 * 1000;   // 60s
    esp_timer_stop(idle_channel_close_timer_);
    esp_timer_start_once(idle_channel_close_timer_, kIdleKeepUs);
}

void Application::CancelIdleChannelClose() {
    if (idle_channel_close_timer_ != nullptr) {
        esp_timer_stop(idle_channel_close_timer_);
    }
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        CancelIdleChannelClose();   /* 要开始干活了，别再定时关 */
        if (!protocol_->IsAudioChannelOpened()) {
            /* 慢路径（通道已关，需 TCP+TLS 重连 ≈1s）：
               2026-11-14 按下即响 —— 立刻放本地提示音（几十 ms，不走网络），
               让用户知道按到了，真实连接在后台跑。
               ⚠️ 这里必须显式把 play_popup_on_listening_ 清掉：
               否则进 listening 时 StartListeningAudio 会再播一次，变成"叮叮"两声。 */
            play_popup_on_listening_ = false;
            SetDeviceState(kDeviceStateConnecting);
            /* 2026-11-15：和语音唤醒一致 —— 表情先按下去，再放提示音，
               否则"叮"已经响了、脸还没变。 */
            {
                auto display = Board::GetInstance().GetDisplay();
                display->SetStatus(Lang::Strings::CONNECTING);
                display->SetEmotion("neutral");
            }
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        /* 快路径（通道还开着，2026-11-14 起闲置 60s 内都是这条）：
           直接进 listening。提示音交给 StartListeningAudio 播 ——
           它是在 EnableVoiceProcessing→ResetDecoder 之后播的，不会被清掉；
           在这里提前播反而会被那次 ResetDecoder 抹掉。 */
        play_popup_on_listening_ = true;
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        /* ⚠️ 2026-11-15 修复（用户报「唤醒后再按退不出去」）：
           昨天那版把这里改成只挂空闲定时器、不关通道 —— 但「关通道」本来
           也是让状态机从 listening 回到 idle 的触发条件（OnDisconnected →
           on_audio_channel_closed_）。通道不关，状态就永远停在 listening，
           再按 BOOT 只是重复挂定时器，听起来完全没反应。
           正确做法：显式停止收音 + 切回 idle，**通道单独留着**给下次唤醒复用。
           与 HandleStopListeningEvent 保持同一套动作。 */
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
        ScheduleIdleChannelClose();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    /* 2026-11-08 连接重试计数（跨唤醒保留，成功/放弃时清零） */
    static uint8_t open_retry_count = 0;

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            /* ⚠️ 2026-11-08 修复「唤醒连不上服务器」：WS 每次唤醒都要重建
               连接（握手+upgrade+hello），失败一次就直接回 idle 放弃 ——
               WiFi 刚从省电模式拉起、DNS 未热、服务器瞬时抖动都会撞上。
               加自动重试：2 秒后再试，最多 3 次；重试期间保持 connecting
               状态，用户看到的是"多等一瞬"而不是报错离线。
               （esp_timer 计时，回调再 Schedule 回主循环执行。） */
            if (open_retry_count < 3) {
                open_retry_count++;
                ESP_LOGW(TAG, "OpenAudioChannel failed, retry %d/3 in 2s", open_retry_count);
                struct RetryCtx { Application* app; ListeningMode mode; esp_timer_handle_t timer; };
                auto* ctx = new RetryCtx{this, mode, nullptr};
                esp_timer_create_args_t timer_args = {};
                timer_args.callback = [](void* p) {
                    auto* c = static_cast<RetryCtx*>(p);
                    esp_timer_handle_t t = c->timer;
                    auto app = c->app;
                    auto m = c->mode;
                    delete c;
                    esp_timer_delete(t);
                    app->Schedule([app, m]() { app->ContinueOpenAudioChannel(m); });
                };
                timer_args.arg = ctx;
                timer_args.name = "ws_open_retry";
                esp_timer_create(&timer_args, &ctx->timer);
                esp_timer_start_once(ctx->timer, 2000000);
                return;
            }
            // 连续 3 次失败：放弃本轮，回 idle（下次唤醒再试）
            open_retry_count = 0;
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error)
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }
    open_retry_count = 0;   // 连接成功（或本来就已打开），清零重试计数

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        CancelIdleChannelClose();   /* 2026-11-14：通道马上要用，别被定时器关掉 */
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
        /* 2026-11-14：不再立刻关通道。留 60s 给下次唤醒复用，
           省掉 TCP+TLS+server hello ≈1s 的建连时间。 */
        ScheduleIdleChannelClose();
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        /* 2026-11-15 防「刚按 BOOT 回待机又被自己唤醒」：
           用户按 BOOT 退出对话的瞬间，麦克风链路里还压着他自己说话的尾巴
           （AFE 重排/复位需要几帧），0.60 门限偶尔会把它认成唤醒词 ——
           表现就是"回到时钟界面，过一下自己又进对话了"。
           回待机后 1.5s 内不接唤醒词：这段时间正常用户不会立刻再叫，
           却正好覆盖掉自己的语音尾巴。实测日志里那几次误唤醒都在这个窗口附近。
           ⚠️ 忽略时必须恢复唤醒检测（检测到唤醒词的那一刻引擎会把
           kWakeWordEnabled 清掉），否则设备从此再也叫不醒。 */
        const int64_t since_idle_us = esp_timer_get_time() - idle_enter_us_;
        /* 2026-11-15 实测日志：一次误唤醒发生在回待机后 1.85s（冷却 1.5s 没挡住），
           另一处 tts 拉回是 190ms。冷却放到 3s，覆盖自己的语音尾巴 + 提示音余响。
           ⚠️ 只在"回待机"这一段生效，对话中（listening/speaking）不受影响。 */
        if (idle_enter_us_ != 0 && since_idle_us < 3000 * 1000) {
            ESP_LOGI(TAG, "回待机冷却中（%.2fs），忽略这次唤醒词", since_idle_us / 1000000.0);
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        /* 2026-11-12 语音打断关掉时：AI 正在说话，唤醒词只记日志、不打断。
           listening 状态不拦 —— 那时本来就在听用户说话。 */
        if (state == kDeviceStateSpeaking && !voice_interrupt_enabled_) {
            ESP_LOGI(TAG, "语音打断已关闭，忽略这次唤醒词（等它说完）");
            return;
        }
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::BeginWakeWordInvoke(const std::string& wake_word) {
    // Must run in the main task with the device in idle state
    CancelIdleChannelClose();   /* 2026-11-14：语音唤醒也要开始用通道了 */
    audio_service_.EncodeWakeWord();

    // Always pass through the connecting state, even if the audio channel is
    // already opened. ContinueWakeWordInvoke() rejects any other state, so
    // skipping this transition would silently drop the wake word invocation.
    if (!SetDeviceState(kDeviceStateConnecting)) {
        // Wake word detection was stopped by the detection itself; restore it
        // so the device does not become unresponsive to wake words.
        audio_service_.EnableWakeWordDetection(true);
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        /* 2026-11-15 语音唤醒也「一叫就响」（用户要求：音效和表情立刻出来，
           连接时间藏到后面，体感才快）。通道闲置 60s 后会自动关，所以语音
           唤醒绝大多数走这条慢路径，重连 0.8~1s 里原来是全程死寂。
           这里和按 BOOT 的慢路径完全同一套：
             · 本地提示音立刻播（不走网络，几十 ms 出音）；
             · 表情/状态由上面 SetDeviceState(Connecting) 同步刷新，两者同时；
             · 真连接在 Schedule 里后台跑。
           ⚠️ 必须显式清 play_popup_on_listening_，否则进 listening 时
           StartListeningAudio 会再播一次，变成"叮叮"两声。 */
        play_popup_on_listening_ = false;
        wake_popup_preplayed_ = true;
        /* 2026-11-15「先出声音后出表情」修复：表情必须先于提示音。
           原来只让状态机去刷表情，而 HandleStateChangedEvent 要等主循环
           下一次轮询（一次 Schedule 往返）；音频则是直接进解码队列马上播，
           所以耳朵先到、眼睛后到。这里同步把表情/状态先按下去，再播音。 */
        {
            auto display = Board::GetInstance().GetDisplay();
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
        }
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second
        Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
        return;
    }
    wake_popup_preplayed_ = false;   /* 快路径：通道还热，进 listening 时补播 */
    // Channel already opened, continue directly
    ContinueWakeWordInvoke(wake_word);
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error), and
            // wake word detection is re-enabled by the idle state handler.
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    /* 2026-11-15：慢路径（通道刚重连）的提示音已经在 BeginWakeWordInvoke 里
       播过了，再置位就是第二声"叮"。只有快路径（通道还热）才需要这里补播。 */
    play_popup_on_listening_ = !wake_popup_preplayed_;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;
    // Any state change invalidates a pending deferred listening start;
    // the Listening case below re-arms it when needed.
    pending_listening_start_ = false;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();    // Clear messages first
            display->SetEmotion("neutral");  // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            /* 2026-11-15：记下回待机的时刻，供 HandleWakeWordDetectedEvent 的
               1.5s 冷却判断用（防"刚回待机就被自己的语音尾巴唤醒"）。 */
            idle_enter_us_ = esp_timer_get_time();
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                /* 2026-11-15 修复「最后一句话丢音」：
                   进入 listening 会走 StartListeningAudio → EnableVoiceProcessing(true)
                   → ResetDecoder()，而 ResetDecoder 会把 decode/playback 两个队列
                   整个清空。原本这个"先等播放排空"的保护只给了 kListeningModeAutoStop，
                   但 AEC 默认开着时走的是 kListeningModeRealtime ——
                   tts stop 一到就立刻清队列，服务端最后 200~400ms 音频直接没了，
                   听感正是「最后一句话被吃掉」。
                   改为**任何模式**只要还有音频没播完，就先挂 pending，等
                   MAIN_EVENT_PLAYBACK_DRAINED 再真正开麦（延迟最多 ~400ms，
                   换来结尾完整）。真正要打断的场景走 AbortSpeaking → ResetDecoder，
                   不受这里影响。 */
                if (!audio_service_.IsPlaybackIdle()) {
                    pending_listening_start_ = true;
                } else {
                    StartListeningAudio();
                }
            } else {
                ConfigureWakeWordForListening();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::StartListeningAudio() {
    // Runs in the main loop, either directly from HandleStateChangedEvent or
    // deferred via MAIN_EVENT_PLAYBACK_DRAINED once the playback queue drains.
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }

    // Send the start listening command
    protocol_->SendStartListening(listening_mode_);
    audio_service_.EnableVoiceProcessing(true);

    ConfigureWakeWordForListening();

    // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}

void Application::ConfigureWakeWordForListening() {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // Enable wake word detection in listening mode (configured via Kconfig)
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
    // Disable wake word detection in listening mode
    audio_service_.EnableWakeWordDetection(false);
#endif
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    /* 2026-11-07 打断即恢复上行 —— abort 后服务器不一定再发 tts stop，
       所有打断路径统一关闸，避免回声滤除一直开着。 */
    audio_service_.SetUplinkSuppress(false);
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();                              // Restart audio service
        // 恢复待机省电：与 OnAudioChannelClosed 保持一致（常驻 PS_NONE），
        // 否则升级失败后设备回待机又进入 modem sleep，链路抖动会让 TTS 断档。
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        // May be called from outside the main task (e.g. board button
        // callbacks), so schedule the invocation instead of running it here
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateIdle) {
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SetVoiceInterruptEnabled(bool enable) {
    voice_interrupt_enabled_ = enable;
    Settings settings("voice", true);
    settings.SetInt("interrupt", enable ? 1 : 0);
    /* ⚠️ 必须 Commit：不落盘就重启会丢（这个坑踩过） */
    settings.Commit();

    /* 2026-11-15 同步 AEC：打断开 = 设备端 AEC 必须开，否则 AI 说话时的
       回声把唤醒词淹掉，喊了没反应（用户报的「打断没生效」就是这个）。
       静默切换：菜单/网页切开关时不要弹「RTC 模式」通知盖住界面。 */
    ApplyAecMode(enable ? kAecOnDeviceSide : kAecOff, false);
    ESP_LOGI(TAG, "语音打断: %s（AEC 同步%s）",
             enable ? "开" : "关", enable ? "设备端开启" : "关闭");
}

void Application::SetAecMode(AecMode mode) { ApplyAecMode(mode, true); }

void Application::ApplyAecMode(AecMode mode, bool notify) {
    aec_mode_ = mode;
    Schedule([this, notify]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                if (notify) display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                if (notify) display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                if (notify) display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

/* 2026-11-12 图传模式：断开会话但保留 protocol_，下次唤醒能正常重连。
   必须在主任务里做（和 ResetProtocol 同样理由：协议对象的生命周期归主任务管）。 */
void Application::CloseAudioChannelForSuspend() {
    Schedule([this]() {
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
            ESP_LOGW(TAG, "暂停小智：音频通道已关闭（释放 WSS/TLS 缓冲）");
        }
    });
}
