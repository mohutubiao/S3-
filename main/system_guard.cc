#include "system_guard.h"

#include <cstdio>
#include <cstring>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

const char* TAG = "SystemGuard";

/* 阈值：主任务 6 秒、LVGL 5 秒。
   取值依据：正常路径最长阻塞是 WS/HTTPS 握手（1~2 秒），留 3 倍余量；
   而实测卡死是分钟级，所以这个阈值不会漏，也不会误杀。 */
constexpr uint32_t kMainTimeoutMs = 6000;
constexpr uint32_t kLvglTimeoutMs = 5000;

volatile uint32_t s_main_ms = 0;   /* 0 = 还没开始喂，不做判定 */
volatile uint32_t s_lvgl_ms = 0;
volatile bool s_fired = false;
RTC_DATA_ATTR char s_last_event[96] = "none";

uint32_t NowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

/* 打印全部任务：名字 / 状态 / 优先级 / 栈余量。
   卡死时最有用的是「哪个任务处于 Blocked 且从未喂心跳」——配合已知的
   main / LVGL 两路，就能判断是谁在等锁。 */
void DumpTasks() {
    static TaskStatus_t s_tasks[24];
    const UBaseType_t n = uxTaskGetSystemState(s_tasks, 24, nullptr);
    ESP_LOGE(TAG, "==== 任务快照（%u 个）====", static_cast<unsigned>(n));
    for (UBaseType_t i = 0; i < n; ++i) {
        const char* st = "?";
        switch (s_tasks[i].eCurrentState) {
            case eRunning:   st = "RUNNING"; break;
            case eReady:     st = "READY"; break;
            case eBlocked:   st = "BLOCKED"; break;
            case eSuspended: st = "SUSPEND"; break;
            case eDeleted:   st = "DELETED"; break;
            default:         st = "INVALID"; break;
        }
        ESP_LOGE(TAG, "  %-16s %-8s prio=%u stack_hw=%u",
                 s_tasks[i].pcTaskName, st,
                 static_cast<unsigned>(s_tasks[i].uxCurrentPriority),
                 static_cast<unsigned>(s_tasks[i].usStackHighWaterMark));
    }
    ESP_LOGE(TAG, "========================");
}

void Fire(const char* who, uint32_t idle_ms) {
    if (s_fired) {
        return;
    }
    s_fired = true;
    snprintf(s_last_event, sizeof(s_last_event), "%s 卡死 %ums 后自动重启",
             who, static_cast<unsigned>(idle_ms));
    ESP_LOGE(TAG, "!!! %s 超过 %ums 没有心跳（UI/按键已失效），打印任务表后自动重启 !!!",
             who, static_cast<unsigned>(idle_ms));
    DumpTasks();
    /* 给串口一点时间把上面几行发出去（115200 下 ~30 行约 30ms，留 100ms 余量） */
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

void TimerCb(void*) {
    const uint32_t now = NowMs();
    const uint32_t m = s_main_ms;
    if (m != 0 && (now - m) > kMainTimeoutMs) {
        Fire("主任务(Application)", now - m);
        return;
    }
    const uint32_t l = s_lvgl_ms;
    if (l != 0 && (now - l) > kLvglTimeoutMs) {
        Fire("LVGL(界面)", now - l);
    }
}

}  // namespace

void SystemGuardInit() {
    static bool inited = false;
    if (inited) {
        return;
    }
    inited = true;
    esp_timer_create_args_t args = {};
    args.callback = &TimerCb;
    args.arg = nullptr;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "sys_guard";
    args.skip_unhandled_events = true;
    esp_timer_handle_t t = nullptr;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_periodic(t, 1000 * 1000);  /* 1 秒查一次 */
        ESP_LOGI(TAG, "心跳护栏已启动（主任务 %ums / LVGL %ums）",
                 static_cast<unsigned>(kMainTimeoutMs),
                 static_cast<unsigned>(kLvglTimeoutMs));
    } else {
        ESP_LOGE(TAG, "心跳护栏启动失败（esp_timer_create）");
    }
}

void SystemGuardNotifyMain() {
    if (s_main_ms == 0) {
        s_main_ms = NowMs();   /* 第一拍只登记，不判超时 */
    } else {
        s_main_ms = NowMs();
    }
}

void SystemGuardNotifyLvgl() {
    if (s_lvgl_ms == 0) {
        s_lvgl_ms = NowMs();
    } else {
        s_lvgl_ms = NowMs();
    }
}

const char* SystemGuardLastEvent() {
    return s_last_event;
}
