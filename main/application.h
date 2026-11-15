#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <atomic>
#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <functional>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_PLAYBACK_DRAINED     (1 << 13)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }

    /* 2026-11-12 语音打断开关
       开（默认）= AI 说话时检测到唤醒词立刻打断（保持原有行为）
       关       = AI 说话期间忽略唤醒词，等它说完再响应
       存在 NVS「voice/interrupt」，网页和语音工具都能改。 */
    void SetVoiceInterruptEnabled(bool enable);
    bool IsVoiceInterruptEnabled() const { return voice_interrupt_enabled_; }
    void PlaySound(const std::string_view& sound);
    /* 2026-11-15 开机模式选择列表还开着时，启动成功音被押后（见
       HandleActivationDoneEvent）。列表收起后由显示层调用本函数补播。 */
    void PlaySuccessSoundIfIdle();
    AudioService& GetAudioService() { return audio_service_; }
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

    /**
     * 只关闭音频通道，**保留** protocol_ 对象（2026-11-12 图传模式用）
     *
     * 与 ResetProtocol 的区别很关键：ResetProtocol 会把 protocol_ 置空，之后
     * HandleWakeWordDetectedEvent 第一句 `if (!protocol_) return;` 就直接返回 ——
     * 设备从此叫不醒，只能重启。本函数只断 WS/TLS 会话，下次唤醒会重新
     * OpenAudioChannel，行为等同于「说完话回到待机后再唤醒」。
     *
     * 用途：图传要抢内部 SRAM，先把在线的加密会话（十几 KB）还回来。
     * 内部走 Schedule，在 application 主任务里执行。
     */
    void CloseAudioChannelForSuspend();

private:
    Application();
    ~Application();

    /* 2026-11-15 AEC 唯一执行入口：notify=false 时静默切换（菜单/网页的
       「语音打断」开关用，避免通知盖住菜单）。SetAecMode 是 notify=true 的包装。 */
    void ApplyAecMode(AecMode mode, bool notify);

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    /* 2026-11-14 唤醒加速：退出 listening 后**延迟**关音频通道。
       只要通道还在，下次唤醒直接 SetListeningMode（100~200ms），
       不必重走 TCP+TLS+server hello（800~1100ms）。空闲 60s 才真关。 */
    esp_timer_handle_t idle_channel_close_timer_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    bool voice_interrupt_enabled_ = true;   /* 2026-11-12 语音打断开关 */
    /* 2026-11-15 启动成功音押后标志（开机模式选择列表还开着时置位） */
    bool success_sound_pending_ = false;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    bool has_server_time_ = false;
    /* 2026-11-14 原子化：收包在协议任务里读，abort 在主任务里写 */
    std::atomic<bool> aborted_{false};
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    /* 2026-11-15：语音唤醒慢路径在 BeginWakeWordInvoke 里就已经"一叫就响"播过
       提示音了，ContinueWakeWordInvoke 不能再置 play_popup_on_listening_，
       否则进 listening 时会重复响一次（"叮叮"）。 */
    bool wake_popup_preplayed_ = false;
    /* 2026-11-15：进入 idle 的时刻（esp_timer 微秒）。用于"回待机 1.5s 内
       不接唤醒词"的冷却 —— 防止按 BOOT 退出后自己语音尾巴被误认。 */
    int64_t idle_enter_us_ = 0;
    bool pending_listening_start_ = false;  // Waiting for playback to drain before starting listening (auto mode)
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    /* 2026-11-14 唤醒加速用：安排/取消「空闲 60s 关音频通道」 */
    void ScheduleIdleChannelClose();
    void CancelIdleChannelClose();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void BeginWakeWordInvoke(const std::string& wake_word);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void StartListeningAudio();
    void ConfigureWakeWordForListening();

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
