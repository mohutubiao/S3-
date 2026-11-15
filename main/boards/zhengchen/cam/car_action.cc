/**
 * 2026-11-11 小车动作调度实现。语义对照 xiaoche-c3（C3MINI）那套：
 *   self.car.act 的 21 个预设 + PlaySequence 的多步序列 + 情绪联动的车体小动作。
 *   从机只吃单步 {steer,throttle,ms}，所以节奏全部在这里拆段。
 */
#include "car_action.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dafeiyu_display.h"
#include "esp_now_link.h"

#define TAG "CarAction"

namespace {

constexpr int kMaxSteps = 64;
constexpr int kTaskStack = 4096;

struct Step {
    int8_t steer;
    int8_t throttle;
    uint16_t ms;
};

struct Job {
    uint8_t count;
    Step steps[kMaxSteps];
};

QueueHandle_t g_q = nullptr;
TaskHandle_t g_task = nullptr;
volatile bool g_abort = false;
volatile bool g_busy = false;

/* ── 预设动作表（幅度/时长照 C3MINI 的口径，混控交给从机 Motor2WD）──
   注意 steer>0 = 右转，throttle>0 = 前进（与 car_cmd_t 一致）。 */
const Step kForward[]   = {{0, 70, 1500}};
const Step kBackward[]  = {{0, -60, 1500}};
const Step kTurnLeft[]  = {{-45, 45, 600}};
const Step kTurnRight[] = {{45, 45, 600}};
const Step kSpinLeft[]  = {{-100, 0, 1500}};   /* 原地左转 / 掉头 */
const Step kSpinRight[] = {{100, 0, 1500}};
const Step kQuickFwd[]  = {{0, 70, 300}};
const Step kQuickBwd[]  = {{0, -60, 300}};
const Step kStop[]      = {{0, 0, 0}};
const Step kWiggle[]    = {{-70, 35, 200}, {70, 35, 200}, {-70, 35, 200}, {70, 35, 200}};

/* ── 装饰动作库（2026-11-14 扩充）──
   ⚠️ 三条铁律，都是为了「每次回到原位」（用户明确要求）：
     ① 优先纯转向（throttle=0）：转向不产生位移，天然回位；而且从机 Drive()
        里 drive_net_ = v_l+v_r = 0，不会被距离保护拦 —— 前后动就有这个风险：
        前进段被 BlocksForward() 拒掉、后退段照跑，结果只后退不回位。
     ② 要前后动的必须「正负同值、同 ms」，抵消干净（原来 speak_start 只前进
        250ms、speak_end 只后退 220ms，一长一短，几十次之后车就一路往前漂）。
     ③ 绝不写单边动作（只有第二段、没有回程段）。
   幅度/时长照 C3MINI 口径；steer>0 = 右转，throttle>0 = 前进。 */
const Step dWake[]       = {{0, 45, 150}, {0, -45, 150}};                  /* 点头 */
const Step dSpeakStart[] = {{0, 55, 200}, {0, -55, 200}};                  /* 前冲再收回来 */
const Step dSpeakEnd[]   = {{0, -40, 160}, {0, 40, 160}};                  /* 收一下再回 */
const Step dHappy[]      = {{-55, 0, 120}, {55, 0, 120},
                            {-55, 0, 120}, {55, 0, 120}};                  /* 开心左右摇 */
const Step dLoving[]     = {{0, 35, 140}, {0, -35, 140},
                            {0, 35, 140}, {0, -35, 140}};                  /* 蹭两下 */
const Step dSad[]        = {{0, -35, 200}, {0, 35, 260}};                  /* 缩一下，慢慢回 */
const Step dShock[]      = {{-75, 0, 100}, {75, 0, 100},
                            {0, -55, 90}, {0, 55, 140}};                   /* 甩头 + 后缩回 */
const Step dAngry[]      = {{-80, 0, 90}, {80, 0, 90},
                            {-80, 0, 90}, {80, 0, 90}};                    /* 快速抖 */
const Step dThink[]      = {{-45, 0, 200}, {45, 0, 200}};                  /* 歪头 */
const Step dConfused[]   = {{-60, 0, 130}, {60, 0, 130},
                            {-60, 0, 130}, {60, 0, 130}};                  /* 左右看 */
const Step dNod[]        = {{0, 40, 120}, {0, -40, 120},
                            {0, 40, 120}, {0, -40, 120}};                  /* 连点两下 */
const Step dShy[]        = {{-35, 0, 220}, {35, 0, 220}};                  /* 害羞扭一下 */
const Step dLookAround[] = {{-70, 0, 260}, {70, 0, 260},
                            {-70, 0, 260}, {70, 0, 260}};                  /* 环顾四周 */

struct DecorPreset {
    const char* name;
    const Step* steps;
    uint8_t count;
};

/* 段数用编译期算，别手数——加一段忘了改数字就会崩 */
#define DECOR_N(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

const DecorPreset kDecors[] = {
    {"wake",        dWake,       DECOR_N(dWake)},
    {"speak_start", dSpeakStart, DECOR_N(dSpeakStart)},
    {"speak_end",   dSpeakEnd,   DECOR_N(dSpeakEnd)},
    {"happy",       dHappy,      DECOR_N(dHappy)},
    {"loving",      dLoving,     DECOR_N(dLoving)},
    {"wake_up",     dNod,        DECOR_N(dNod)},
    {"sad",         dSad,        DECOR_N(dSad)},
    {"angry",       dAngry,      DECOR_N(dAngry)},
    {"surprised",   dShock,      DECOR_N(dShock)},
    {"thinking",    dThink,      DECOR_N(dThink)},
    {"confused",    dConfused,   DECOR_N(dConfused)},
    {"shy",         dShy,        DECOR_N(dShy)},
    {"nod",         dNod,        DECOR_N(dNod)},
    {"look_around", dLookAround, DECOR_N(dLookAround)},
};

/* 节流：服务端一句话里可能连发几个 emotion，太频繁车会一直抖 */
constexpr int64_t kDecorIntervalMs = 800;

/* 情绪小动作：照 HandleMotorActionForEmotion —— 车"点头/缩一下"配合表情 */
const Step kHappy[]     = {{0, 55, 180}};
const Step kSad[]       = {{0, -45, 180}};
const Step kAngry[]     = {{0, -80, 100}, {0, 80, 150}};   /* 后撤→前冲 */
const Step kExcited[]   = {{0, 60, 120}, {0, 0, 120}, {0, 60, 120}};
const Step kConfused[]  = {{-60, 35, 180}, {60, 35, 180}}; /* 左右摆头 */

struct Preset {
    const char* name;
    const Step* steps;
    uint8_t count;
};

const Preset kPresets[] = {
    {"forward",        kForward,   1},
    {"backward",       kBackward,  1},
    {"turn_left",      kTurnLeft,  1},
    {"left",           kTurnLeft,  1},
    {"turn_right",     kTurnRight, 1},
    {"right",          kTurnRight, 1},
    {"spin_left",      kSpinLeft,  1},
    {"spin_right",     kSpinRight, 1},
    {"spin_around",    kSpinLeft,  1},   /* 掉头：给足时长才转得成圈 */
    {"quick_forward",  kQuickFwd,  1},
    {"quick_backward", kQuickBwd,  1},
    {"wiggle",         kWiggle,    4},
    {"stop",           kStop,      1},
    {"wake_up",        kHappy,     1},
    {"happy",          kHappy,     1},
    {"loving",         kHappy,     1},
    {"excited",        kExcited,   3},
    {"sad",            kSad,       1},
    {"angry",          kAngry,     2},
    {"surprised",      kAngry,     2},
    {"confused",       kConfused,  2},
    {"thinking",       kStop,      1},   /* 只换表情，车不动 */
    {"listening",      kStop,      1},
    {"speaking",       kStop,      1},
};

/* ── 装饰动作：让眼睛/表情跟着动作走（设备端自己做，不依赖大模型）── */
void ApplyDeco(const char* name, uint32_t total_ms) {
    auto* d = DafeiyuDisplay::Instance();
    if (d == nullptr || name == nullptr) return;
    uint32_t hold = total_ms + 500;
    if (hold < 800) hold = 800;
    if (hold > 8000) hold = 8000;
    d->CarDriveAction(name, hold);
}

void Worker(void*) {
    for (;;) {
        Job job;
        if (xQueueReceive(g_q, &job, portMAX_DELAY) != pdTRUE) continue;

        for (int i = 0; i < job.count; ++i) {
            if (g_abort) break;
            if (uxQueueMessagesWaiting(g_q) > 0) break;   /* 新任务抢占，别拖旧尾巴 */

            EspNowLink::Get().Send(job.steps[i].steer, job.steps[i].throttle, job.steps[i].ms);

            /* 20ms 一片地等：急停要能立刻断掉，不能一口气 vTaskDelay(5000) */
            uint32_t waited = 0;
            while (waited < job.steps[i].ms) {
                vTaskDelay(pdMS_TO_TICKS(20));
                waited += 20;
                if (g_abort || uxQueueMessagesWaiting(g_q) > 0) break;
            }
        }

        if (g_abort) {
            EspNowLink::Get().Send(0, 0, 0);
            g_abort = false;
        }
        g_busy = (uxQueueMessagesWaiting(g_q) > 0);
    }
}

void EnsureTask() {
    if (g_q == nullptr) {
        g_q = xQueueCreate(1, sizeof(Job));   /* 深度 1 + xQueueOverwrite = 最新指令覆盖旧的 */
    }
    if (g_task == nullptr && g_q != nullptr) {
        if (xTaskCreate(Worker, "car_action", kTaskStack, nullptr, 3, &g_task) != pdPASS) {
            ESP_LOGE(TAG, "动作任务创建失败，动作退化为单步直发");
            g_task = nullptr;
        }
    }
}

uint32_t TotalMs(const Job& job) {
    uint32_t t = 0;
    for (int i = 0; i < job.count; ++i) t += job.steps[i].ms;
    return t;
}

/** 按第一段猜一个装饰动作名，序列也能有像样的表情 */
const char* DecoFromStep(const Step& s) {
    if (s.steer > 60 && s.throttle > -20 && s.throttle < 20) return "spin_right";
    if (s.steer < -60 && s.throttle > -20 && s.throttle < 20) return "spin_left";
    if (s.throttle > 20) return "forward";
    if (s.throttle < -20) return "backward";
    if (s.steer > 20) return "right";
    if (s.steer < -20) return "left";
    return "stop";
}

bool Submit(Job& job, const char* deco) {
    EnsureTask();
    if (g_q == nullptr || g_task == nullptr) {
        /* 兜底：没有任务就只发第一段，至少能动 */
        if (job.count > 0) {
            EspNowLink::Get().Send(job.steps[0].steer, job.steps[0].throttle, job.steps[0].ms);
        }
        ApplyDeco(deco, TotalMs(job));
        return true;
    }
    g_abort = false;
    g_busy = true;
    xQueueOverwrite(g_q, &job);
    ApplyDeco(deco, TotalMs(job));
    ESP_LOGI(TAG, "动作 %s：%d 段 / %u ms", deco != nullptr ? deco : "(装饰)",
             (int)job.count, (unsigned)TotalMs(job));
    return true;
}

/** 舞蹈：前 300 / 原地左 250 / 原地右 250 / 后 300 = 一轮 1100ms，按需循环拼段 */
bool BuildDance(Job& job, int duration_ms) {
    static const int kSteer[4] = {0, -100, 100, 0};
    static const int kThr[4]   = {70, 0, 0, -60};
    static const int kMs[4]    = {300, 250, 250, 300};

    uint32_t target = (duration_ms <= 0) ? 1100u : (uint32_t)duration_ms;
    if (target > 15000u) target = 15000u;

    uint32_t acc = 0;
    int idx = 0;
    while (acc < target && job.count < kMaxSteps) {
        uint32_t remain = target - acc;
        uint32_t ms = (uint32_t)kMs[idx & 3];
        if (ms > remain) ms = remain;
        if (ms < 100) break;   /* 尾巴不够一段就不抖了 */
        job.steps[job.count].steer = (int8_t)kSteer[idx & 3];
        job.steps[job.count].throttle = (int8_t)kThr[idx & 3];
        job.steps[job.count].ms = (uint16_t)ms;
        job.count++;
        acc += ms;
        ++idx;
    }
    return job.count > 0;
}

int ClampPct(int v) {
    if (v > 100) return 100;
    if (v < -100) return -100;
    return v;
}

}  // namespace

namespace car_action {

bool Run(const char* name, int duration_ms) {
    if (name == nullptr || name[0] == '\0') return false;

    Job job = {};

    if (strcmp(name, "dance") == 0) {
        if (!BuildDance(job, duration_ms)) return false;
        return Submit(job, "dance");
    }

    const Preset* found = nullptr;
    for (size_t i = 0; i < sizeof(kPresets) / sizeof(kPresets[0]); ++i) {
        if (strcmp(kPresets[i].name, name) == 0) {
            found = &kPresets[i];
            break;
        }
    }
    if (found == nullptr) {
        ESP_LOGW(TAG, "不认识的预设动作: %s", name);
        return false;
    }

    job.count = found->count;
    for (int i = 0; i < found->count; ++i) {
        job.steps[i] = found->steps[i];
    }
    /* 时长覆盖：单段动作才允许改（"后退 2 秒"），多段动作保持原节奏 */
    if (duration_ms > 0 && job.count == 1 && strcmp(name, "stop") != 0) {
        int ms = duration_ms;
        if (ms < 100) ms = 100;
        if (ms > 10000) ms = 10000;
        job.steps[0].ms = (uint16_t)ms;
    }
    return Submit(job, name);
}

bool RunSequence(const char* steps) {
    if (steps == nullptr || steps[0] == '\0') return false;

    Job job = {};
    const char* p = steps;
    while (*p != '\0' && job.count < kMaxSteps) {
        int s = 0, t = 0, ms = 0, n = 0;
        if (sscanf(p, "%d,%d,%d%n", &s, &t, &ms, &n) != 3) return false;
        if (ms < 50 || ms > 5000) return false;
        job.steps[job.count].steer = (int8_t)ClampPct(s);
        job.steps[job.count].throttle = (int8_t)ClampPct(t);
        job.steps[job.count].ms = (uint16_t)ms;
        job.count++;
        p += n;
        while (*p == ' ') ++p;
        if (*p == ';') { ++p; continue; }
        break;
    }
    if (job.count == 0) return false;

    return Submit(job, DecoFromStep(job.steps[0]));
}

void Decor(const char* kind) {
    if (kind == nullptr || kind[0] == '\0') return;

    /* ⚠️ 2026-11-12：探索期间绝不发装饰动作。
       装饰动作都是非零 steer/throttle（speak_start = 前进 60 持续 250ms），
       从机把「收到非零指令」当成用户手动接管 → 立刻退出探索 → 停车。
       实测链路：碰撞上报给大模型 → AI 开口播报「被拦截」→ speak_start
       → 探索被踢掉。这就是用户看到的「一拦截就停」，不是距离保护停的车。 */
    {
        LinkStatus st;
        if (EspNowLink::Get().Status(&st) && (st.flags & TLM_FLAG_EXPLORE) != 0) {
            ESP_LOGD(TAG, "装饰动作 %s 跳过：从机正在探索", kind);
            return;
        }
    }

    /* 忙窗口（对应 C3 的 MarkMotorBusyForApp）：用户动作/序列在跑时一律跳过，
       否则装饰动作会插队把"跳舞 5 秒"顶掉，看起来就是动作一跳一跳。 */
    if (g_busy) {
        ESP_LOGD(TAG, "装饰动作 %s 跳过：用户动作在跑", kind);
        return;
    }

    static int64_t last_ms = 0;
    const int64_t now = esp_timer_get_time() / 1000;
    if (last_ms != 0 && now - last_ms < kDecorIntervalMs) return;

    const DecorPreset* found = nullptr;
    for (size_t i = 0; i < sizeof(kDecors) / sizeof(kDecors[0]); ++i) {
        if (strcmp(kDecors[i].name, kind) == 0) {
            found = &kDecors[i];
            break;
        }
    }
    if (found == nullptr) return;

    Job job = {};
    job.count = found->count;
    for (int i = 0; i < found->count; ++i) job.steps[i] = found->steps[i];

    last_ms = now;
    Submit(job, nullptr);   /* nullptr = 不动屏幕表情，表情由情绪/动作管线管 */
    ESP_LOGI(TAG, "装饰动作 %s：%u ms", kind, (unsigned)TotalMs(job));
}

void Stop() {
    g_abort = true;
    g_busy = false;
    EspNowLink::Get().Send(0, 0, 0);   /* 不等任务，立刻断电 */
    if (auto* d = DafeiyuDisplay::Instance()) {
        d->CarDriveAction("stop", 800);
    }
}

bool Busy() { return g_busy; }

}  // namespace car_action
