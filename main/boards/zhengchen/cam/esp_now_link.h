#pragma once

#include <cstdint>

/* 2026-11-11 小车模式 ESP-NOW 主机端（S3 发，C3 从机收）
 *
 * 与从机 C:\Users\mohut\xiaoche-c3-slave\src\main.cpp 的 car_cmd_t
 * 必须逐字节一致。
 *
 * ⚠️ 两端都**不要**加 __attribute__((packed))**：
 *    从机是默认对齐（10 字节数据补齐到 12），S3 端若 packed 成 10 字节，
 *    对端 `len != sizeof(car_cmd_t)` 会直接丢包，表现就是「C3 一直对频不上」，
 *    而且两边都不报错，极难查。
 */
struct CarCmd {
    uint32_t magic;      /* 0x52414331，防串包 */
    int8_t   steer;      /* -100..100，>0 右转 */
    int8_t   throttle;   /* -100..100，>0 前进 */
    uint16_t ms;         /* 自动停时长 ms（0 = 一直跑到下一条） */
    uint8_t  seq;        /* 包序号 */
    uint8_t  crc;        /* 前 7 字节异或 */
};

static_assert(sizeof(CarCmd) == 12,
              "CarCmd 布局必须与 C3 从机 car_cmd_t 一致（默认对齐 = 12 字节）");

/* ── 2026-11-12 距离保护配置（主机 → 从机）──
   GY-530（VL53L0X）接在从机上（SCL=GPIO0 / SDA=GPIO10），阈值在主机设，
   通过这个包下发；从机存 NVS，重启后自己恢复。
   ⚠️ 与 car_cmd_t 都是 12 字节，从机靠 magic 分流，两者必须逐字节一致：
      C:\Users\mohut\xiaoche-c3-slave\src\main.cpp 的 car_cfg_t */
struct CarCfg {
    uint32_t magic;        /* 0x31474643 = "CFG1" */
    uint16_t obstacle_mm;  /* ≤ 此距离 = 障碍（碰撞） */
    uint16_t fall_mm;      /* ≥ 此距离 = 悬空（跌落） */
    uint8_t  flags;        /* bit0 = 距离保护开关 */
    uint8_t  seq;
    uint8_t  crc;          /* 前 10 字节异或 */
    uint8_t  reserved;
};

static_assert(sizeof(CarCfg) == 12,
              "CarCfg 布局必须与 C3 从机 car_cfg_t 一致（默认对齐 = 12 字节）");

/* ── 2026-11-12 遥测（从机 → 主机，5Hz）──
   与从机 car_tlm_t 逐字节一致（20 字节）。 */
#define CAR_TLM_MAGIC 0x314D4C54u   /* "TLM1" */
#define TLM_FLAG_ENABLED  0x01
#define TLM_FLAG_SENSOR   0x02
#define TLM_FLAG_EXPLORE  0x04
struct CarTelemetry {
    uint32_t magic;
    uint16_t mm;          /* 当前距离；8191 = 无有效目标 */
    uint8_t state;
    uint8_t flags;
    uint8_t fps;
    uint8_t seq;
    uint8_t crc;          /* crc 之前 10 字节异或（不含 crc 自身） */
    uint8_t reserved;
    uint32_t up_ms;
    uint16_t obstacle_mm;   /* 从机实际生效阈值（可能被地面基线夹紧改过） */
    uint16_t fall_mm;
};
static_assert(sizeof(CarTelemetry) == 20,
              "CarTelemetry 必须与从机 car_tlm_t 一致（20 字节）");

/* ── 2026-11-12 探索模式命令（主机 → 从机）── */
#define CAR_EXP_MAGIC 0x31505845u   /* "EXP1" */
struct CarExplore {
    uint32_t magic;
    uint8_t action;      /* 0 = 停止 1 = 开始 */
    uint8_t speed_pct;
    uint16_t duration_s;
    uint8_t seq;
    uint8_t crc;         /* 前 6 字节异或 */
    uint8_t reserved[6];
};
static_assert(sizeof(CarExplore) == 16,
              "CarExplore 必须与从机 car_exp_t 一致（16 字节）");

/* ── 2026-11-12 全局速度（主机 → 从机，网页滑块）──
   与从机 car_spd_t 逐字节一致（12 字节，与 CarCmd/CarCfg 同长，靠 magic 分流）。
   从机在 Motor2WD::Drive() 入口统一缩放 steer/throttle，遥控和探索共用。 */
#define CAR_SPD_MAGIC 0x31504453u   /* "SDP1" */
struct CarSpeed {
    uint32_t magic;
    uint8_t  pct;        /* 20~100 */
    uint8_t  crc;        /* 前 5 字节异或（magic + pct） */
    uint8_t  reserved[6];
};
static_assert(sizeof(CarSpeed) == 12,
              "CarSpeed 必须与从机 car_spd_t 一致（12 字节）");

/* 2026-11-12：从机上报的最近一次「拦截事件」。
   用来自查/告诉大模型「是什么拦住了车」。从机进入危险状态时发一次，
   不是每帧都发，所以这里只保存最后一次。 */
struct BlockInfo {
    int type;          /* 1 = 碰撞（障碍） 2 = 跌落（悬空） 0 = 没发生过 */
    uint16_t mm;       /* 触发时的距离读数 */
    uint32_t age_ms;   /* 距现在多少毫秒 */
    int count;         /* 本次开机累计拦截次数 */
};

/* 从机实时状态（5Hz 遥测）：当前距离 + 保护状态 + 在线判断 */
struct LinkStatus {
    bool online;        /* 3 秒内收到过遥测 */
    uint32_t age_ms;    /* 距最后一条遥测多久 */
    uint16_t mm;        /* 当前距离，8191/0 = 无有效目标 */
    int state;          /* DistanceGuard::State */
    uint8_t flags;      /* TLM_FLAG_* */
    uint8_t fps;        /* 传感器帧率 */
    uint16_t obstacle_mm;   /* 从机实际生效的碰撞阈值 */
    uint16_t fall_mm;       /* 从机实际生效的跌落阈值 */
};

class EspNowLink {
public:
    static EspNowLink& Get();

    /* 幂等；WiFi 未初始化时返回 false（调用方可稍后重试） */
    bool Start();
    void Stop();
    bool Running() const;

    /* steer/throttle 超界自动限幅；ms=0 表示持续 */
    bool Send(int steer, int throttle, uint16_t ms);

    /* 2026-11-12：把距离保护阈值下发到从机（立即生效 + 从机存 NVS）。
       obstacle_mm ≤ 此距离判碰撞；fall_mm ≥ 此距离判跌落。
       内部自动夹紧（fall ≥ obstacle+50），返回 false = ESP-NOW 没起来。 */
    bool SendConfig(int obstacle_mm, int fall_mm, bool enabled);

    /* 取最近一次拦截事件；返回 false = 本次开机还没拦过（或从机没上报）。
       ⚠️ 从机是广播上报、不重发，丢一条就少一条，不是关键路径。 */
    bool LastBlock(BlockInfo* out) const;

    /* 从机是否在线 + 当前距离。online=false 时其余字段无意义。 */
    bool Status(LinkStatus* out) const;

    /* 探索模式：action 1=开始 0=停止；speed_pct 20~100；duration_s 0=默认 120
       ⚠️ 不需要停心跳：从机在探索中会忽略 0/0 心跳（见从机 main.cpp） */
    bool SendExplore(int action, int speed_pct, int duration_s);

    /* 2026-11-12 全局速度：网页滑块变化时下发，从机统一缩放遥控和探索的速度。
       返回 false = ESP-NOW 没起来。 */
    bool SendSpeed(int pct);

private:
    EspNowLink() = default;
    bool StartLocked();
};
