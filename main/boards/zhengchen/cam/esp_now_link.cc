#include "esp_now_link.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <mutex>

#include "application.h"   /* SendMcpMessage：把拦截原因告诉大模型 */

#define TAG "EspNowLink"

namespace {
/* 与从机 CAR_MAGIC 完全一致（写成 0x52414331 时内存里是 "1CAR"，
   从机注释写 "CAR1" 但值相同，不影响） */
constexpr uint32_t kCarMagic = 0x52414331u;
constexpr uint32_t kCfgMagic = 0x31474643u;   /* "CFG1"，与从机 CAR_CFG_MAGIC 一致 */
constexpr uint32_t kEvtMagic = 0x31545645u;   /* "EVT1"，与从机 CAR_EVT_MAGIC 一致 */
const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* 与从机 car_evt_t 逐字节一致（16 字节，默认对齐） */
struct CarEventRaw {
    uint32_t magic;
    uint8_t  type;        /* 1 = 碰撞 2 = 跌落 */
    uint8_t  state;
    uint16_t mm;
    uint32_t up_ms;
    uint8_t  reserved[3];
    uint8_t  crc;         /* 前 15 字节异或 */
};
static_assert(sizeof(CarEventRaw) == 16, "CarEventRaw 必须与从机 car_evt_t 一致");

volatile int g_blk_type = 0;
volatile uint16_t g_blk_mm = 0;
volatile uint32_t g_blk_ms = 0;
volatile int g_blk_count = 0;

/* 遥测（5Hz）：主机显示实时距离 + 判断从机在线 */
volatile uint32_t g_tlm_ms = 0;
volatile uint16_t g_tlm_mm = 0;
volatile int g_tlm_state = 0;
volatile uint8_t g_tlm_flags = 0;
volatile uint8_t g_tlm_fps = 0;
volatile uint16_t g_tlm_obs_mm = 0;   /* 从机实际生效阈值 */
volatile uint16_t g_tlm_fall_mm = 0;
uint8_t g_exp_seq = 0;

/* 心跳周期。从机扫描时每个信道停 400ms，13 个信道一轮 5.2 秒；
   200ms 一个包 => 无论从机停在哪一信道，400ms 窗口内至少撞到 2 个包，
   必然完成对频。之前「单发一次」的方案实测 92% 概率被错过（C3 一直快闪）。 */
constexpr int kHeartbeatMs = 200;
/* 单次 Send 的即时连发：让「说一句就走」的第一下也有较大概率命中 */
constexpr int kBurstCount = 3;
constexpr int kBurstGapMs = 8;

std::mutex g_mu;
bool g_running = false;
uint8_t g_seq = 0;
uint8_t g_cfg_seq = 0;
esp_timer_handle_t g_hb_timer = nullptr;

/* 当前指令（心跳据此重发） */
int g_steer = 0;
int g_throttle = 0;
bool g_hold = false;        /* true = 有速度指令，需要心跳维持 */
int64_t g_expire_us = 0;    /* 到期时刻；0 = 无时限（持续指令） */

int64_t NowUs() { return esp_timer_get_time(); }

void SendRawLocked(int steer, int throttle, uint16_t ms) {
    CarCmd c = {};
    c.magic = kCarMagic;
    c.steer = static_cast<int8_t>(steer);
    c.throttle = static_cast<int8_t>(throttle);
    c.ms = ms;
    c.seq = g_seq++;

    /* CRC = 前 7 字节异或（与从机算法一致） */
    uint8_t crc = 0;
    for (int i = 0; i < 7; ++i) {
        crc ^= reinterpret_cast<const uint8_t*>(&c)[i];
    }
    c.crc = crc;

    esp_err_t err = esp_now_send(kBroadcast, reinterpret_cast<const uint8_t*>(&c), sizeof(c));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send 失败: %s", esp_err_to_name(err));
    }
}

void HeartbeatCb(void*) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_running) return;

    const int64_t now = NowUs();
    const bool active = g_hold && (g_expire_us == 0 || g_expire_us > now);
    if (active) {
        uint16_t remain_ms = 0;   /* 0 = 一直跑到下一条（持续指令） */
        if (g_expire_us > 0) {
            int64_t r = (g_expire_us - now) / 1000;
            if (r < 1) r = 1;
            remain_ms = static_cast<uint16_t>(r);
        }
        SendRawLocked(g_steer, g_throttle, remain_ms);
    } else {
        if (g_hold) {
            ESP_LOGI(TAG, "指令到期，发停止包");
        }
        g_steer = 0;
        g_throttle = 0;
        g_hold = false;
        g_expire_us = 0;
        SendRawLocked(0, 0, 0);   /* 空闲心跳：帮从机对频，同时保证车是停的 */
    }
}

/* 从机 → 主机：拦截事件（2026-11-12）
   ⚠️ 运行在 WiFi 任务上下文，这里只做轻量保存 + SendMcpMessage（内部会 Schedule
   到主任务），绝不能直接碰 UI/电机。 */
void OnEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    (void)info;
    if (data == nullptr) return;

    /* ── 遥测（5Hz）：只更新状态，不打扰任何任务 ── */
    if (len == static_cast<int>(sizeof(CarTelemetry))) {
        CarTelemetry t;
        memcpy(&t, data, sizeof(t));
        if (t.magic != CAR_TLM_MAGIC) return;

        /* ⚠️ 只算 crc 字段之前（索引 0~9）。之前写 11 把 crc 自身也异或
           进去，接收端得 0，所有遥测被丢 → 「动作正常但显示从机未连接」。 */
        uint8_t crc = 0;
        for (int i = 0; i < 10; ++i) {
            crc ^= reinterpret_cast<const uint8_t*>(&t)[i];
        }
        if (crc != t.crc) {
            /* 限流日志：区分「包没到」和「CRC 对不上」 */
            static uint32_t bad = 0;
            if ((++bad % 50) == 1) {
                ESP_LOGW(TAG, "遥测 CRC 错: 收到 %02x 算出 %02x（累计 %u 个）",
                         t.crc, crc, static_cast<unsigned>(bad));
            }
            return;
        }

        g_tlm_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        g_tlm_mm = t.mm;
        g_tlm_state = t.state;
        g_tlm_flags = t.flags;
        g_tlm_fps = t.fps;
        g_tlm_obs_mm = t.obstacle_mm;
        g_tlm_fall_mm = t.fall_mm;
        return;
    }

    if (len != static_cast<int>(sizeof(CarEventRaw))) return;

    CarEventRaw e;
    memcpy(&e, data, sizeof(e));
    if (e.magic != kEvtMagic) return;

    uint8_t crc = 0;
    for (int i = 0; i < 15; ++i) {
        crc ^= reinterpret_cast<const uint8_t*>(&e)[i];
    }
    if (crc != e.crc) return;

    g_blk_type = e.type;
    g_blk_mm = e.mm;
    g_blk_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    g_blk_count = g_blk_count + 1;

    const char* what = (e.type == 2) ? "前方悬空（防跌落）" : "前方有障碍（防碰撞）";
    ESP_LOGW(TAG, "从机拦截: %s %umm", what, static_cast<unsigned>(e.mm));

    /* 让大模型知道是什么拦的：主动发一条 MCP notification。
       服务端不支持 notification 时会忽略，不影响本地；
       MCP 工具 self.car.distance 的 get 也会返回同一条，双保险。 */
    char payload[320];
    snprintf(payload, sizeof(payload),
             "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/message\","
             "\"params\":{\"level\":\"warning\",\"logger\":\"car_guard\","
             "\"data\":\"%s，距离 %u 厘米，已自动停车，前进被拒绝\"}}",
             what, static_cast<unsigned>(e.mm / 10));
    Application::GetInstance().SendMcpMessage(payload);
}
}  // namespace

EspNowLink& EspNowLink::Get() {
    static EspNowLink inst;
    return inst;
}

bool EspNowLink::Running() const { return g_running; }

bool EspNowLink::StartLocked() {
    if (g_running) return true;

    /* ⚠️ 2026-11-12：不再容忍 ESP_ERR_ESPNOW_INTERNAL。
       WiFi 没起来时 esp_now_init 常返回 INTERNAL，旧代码当成成功继续走，
       结果 g_running=true 但实际没通 —— 表现为「永远显示从机未连接」而且
       再也不会重试。现在一律返回 false，交给上调用的 2 秒重试。 */
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init 失败: %s（WiFi 还没起来？）", esp_err_to_name(err));
        return false;
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, kBroadcast, sizeof(kBroadcast));
    peer.channel = 0;              /* 0 = 跟随当前 WiFi 信道 */
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "esp_now_add_peer 失败: %s", esp_err_to_name(err));
        esp_now_deinit();
        return false;
    }

    /* 接收拦截事件。每次启动都注册一次 —— esp_now_deinit() 会把回调一起清掉，
       用 static 标记跳过第二次会导致「Stop 再 Start 后收不到上报」。 */
    esp_now_register_recv_cb(OnEspNowRecv);

    g_running = true;
    g_hold = false;
    g_expire_us = 0;

    if (g_hb_timer == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = &HeartbeatCb,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "espnow_hb",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &g_hb_timer) != ESP_OK) {
            ESP_LOGW(TAG, "心跳定时器创建失败，退化为单发模式");
            g_hb_timer = nullptr;
        }
    }
    if (g_hb_timer != nullptr) {
        esp_timer_stop(g_hb_timer);
        esp_timer_start_periodic(g_hb_timer, kHeartbeatMs * 1000ULL);
    }

    uint8_t ch = 0;
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&ch, &sec);
    ESP_LOGI(TAG, "ESP-NOW 就绪，广播信道=%u，心跳 %dms", ch, kHeartbeatMs);
    return true;
}

bool EspNowLink::Start() {
    std::lock_guard<std::mutex> lk(g_mu);
    return StartLocked();
}

void EspNowLink::Stop() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_running) return;
    if (g_hb_timer != nullptr) {
        esp_timer_stop(g_hb_timer);
    }
    esp_now_del_peer(kBroadcast);
    esp_now_deinit();
    g_running = false;
    g_steer = 0;
    g_throttle = 0;
    g_hold = false;
    g_expire_us = 0;
    ESP_LOGW(TAG, "ESP-NOW 已停止");
}

bool EspNowLink::Send(int steer, int throttle, uint16_t ms) {
    if (steer > 100) steer = 100;
    if (steer < -100) steer = -100;
    if (throttle > 100) throttle = 100;
    if (throttle < -100) throttle = -100;

    std::lock_guard<std::mutex> lk(g_mu);
    if (!StartLocked()) return false;

    g_steer = steer;
    g_throttle = throttle;
    g_hold = (steer != 0 || throttle != 0);
    /* ms>0：到点自动停（心跳到点转 stop）；ms=0 且带速度：持续，
       由下一次 Send/stop 结束 —— 与原语义一致 */
    g_expire_us = (g_hold && ms > 0) ? (NowUs() + static_cast<int64_t>(ms) * 1000) : 0;

    for (int i = 0; i < kBurstCount; ++i) {
        SendRawLocked(steer, throttle, ms);
        if (i + 1 < kBurstCount) {
            vTaskDelay(pdMS_TO_TICKS(kBurstGapMs));
        }
    }
    return true;
}

bool EspNowLink::SendConfig(int obstacle_mm, int fall_mm, bool enabled) {
    /* 夹紧规则与从机 DistanceGuard::SetThresholds 保持一致，
       避免「主机显示 30、从机实际 40」这种对不上的情况。 */
    if (obstacle_mm < 30) obstacle_mm = 30;
    if (obstacle_mm > 2000) obstacle_mm = 2000;
    if (fall_mm < obstacle_mm + 50) fall_mm = obstacle_mm + 50;
    if (fall_mm > 8000) fall_mm = 8000;

    std::lock_guard<std::mutex> lk(g_mu);
    if (!StartLocked()) return false;

    CarCfg c = {};
    c.magic = kCfgMagic;
    c.obstacle_mm = static_cast<uint16_t>(obstacle_mm);
    c.fall_mm = static_cast<uint16_t>(fall_mm);
    c.flags = enabled ? 0x01 : 0x00;
    c.seq = g_cfg_seq++;

    uint8_t crc = 0;
    for (int i = 0; i < 10; ++i) {
        crc ^= reinterpret_cast<const uint8_t*>(&c)[i];
    }
    c.crc = crc;

    /* 从机在扫描信道时每 400ms 才停在某个信道一次，单发容易错过；
       运动指令用了 3 连发，这里照做。 */
    for (int i = 0; i < kBurstCount; ++i) {
        esp_err_t err = esp_now_send(kBroadcast, reinterpret_cast<const uint8_t*>(&c), sizeof(c));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "配置下发失败: %s", esp_err_to_name(err));
            return false;
        }
        if (i + 1 < kBurstCount) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    ESP_LOGI(TAG, "距离保护配置已下发: 障碍<=%dmm 跌落>=%dmm enabled=%d",
             obstacle_mm, fall_mm, (int)enabled);
    return true;
}

bool EspNowLink::LastBlock(BlockInfo* out) const {
    if (out == nullptr) return false;
    const int type = g_blk_type;
    if (type == 0) return false;

    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    out->type = type;
    out->mm = g_blk_mm;
    out->age_ms = static_cast<uint32_t>(now - g_blk_ms);
    out->count = g_blk_count;
    return true;
}

bool EspNowLink::Status(LinkStatus* out) const {
    if (out == nullptr) return false;
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const uint32_t last = g_tlm_ms;

    out->age_ms = static_cast<uint32_t>(now - last);
    out->mm = g_tlm_mm;
    out->state = g_tlm_state;
    out->flags = g_tlm_flags;
    out->fps = g_tlm_fps;
    out->obstacle_mm = g_tlm_obs_mm;
    out->fall_mm = g_tlm_fall_mm;
    /* 5Hz 遥测：原来 3 秒，主页显示时发现 WiFi 忙（图传/说话）会误报离线。
       5 秒更稳：丢 25 帧才算断，正常丢几帧不影响。 */
    out->online = (last != 0) && (out->age_ms < 5000);
    return out->online;
}

bool EspNowLink::SendExplore(int action, int speed_pct, int duration_s) {
    if (speed_pct < 20) speed_pct = 20;
    if (speed_pct > 100) speed_pct = 100;
    if (duration_s <= 0) duration_s = 120;
    if (duration_s > 3600) duration_s = 3600;

    std::lock_guard<std::mutex> lk(g_mu);
    if (!StartLocked()) return false;

    CarExplore e = {};
    e.magic = CAR_EXP_MAGIC;
    e.action = static_cast<uint8_t>(action != 0 ? 1 : 0);
    e.speed_pct = static_cast<uint8_t>(speed_pct);
    e.duration_s = static_cast<uint16_t>(duration_s);
    e.seq = g_exp_seq++;

    uint8_t crc = 0;
    for (int i = 0; i < 6; ++i) {
        crc ^= reinterpret_cast<const uint8_t*>(&e)[i];
    }
    e.crc = crc;

    /* 探索命令多发两轮：从机可能正好在扫描信道 */
    for (int i = 0; i < 4; ++i) {
        esp_err_t err = esp_now_send(kBroadcast, reinterpret_cast<const uint8_t*>(&e), sizeof(e));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "探索命令发送失败: %s", esp_err_to_name(err));
            return false;
        }
        if (i + 1 < 4) vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "探索命令: %s speed=%d%% duration=%ds",
             action ? "开始" : "停止", speed_pct, duration_s);
    return true;
}

bool EspNowLink::SendSpeed(int pct) {
    if (pct < 20) pct = 20;
    if (pct > 100) pct = 100;

    std::lock_guard<std::mutex> lk(g_mu);
    if (!StartLocked()) return false;

    CarSpeed s = {};
    s.magic = CAR_SPD_MAGIC;
    s.pct = static_cast<uint8_t>(pct);

    uint8_t crc = 0;
    for (int i = 0; i < 5; ++i) {
        crc ^= reinterpret_cast<const uint8_t*>(&s)[i];
    }
    s.crc = crc;

    /* 4 连发：从机可能在扫信道，单发容易错过（同运动指令的做法） */
    for (int i = 0; i < 4; ++i) {
        esp_err_t err = esp_now_send(kBroadcast, reinterpret_cast<const uint8_t*>(&s), sizeof(s));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "全局速度下发失败: %s", esp_err_to_name(err));
            return false;
        }
        if (i + 1 < 4) vTaskDelay(pdMS_TO_TICKS(15));
    }
    ESP_LOGI(TAG, "全局速度: %d%%", pct);
    return true;
}


