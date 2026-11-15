/**
 * orb.h —— 低开销「呼吸圆球」渲染核心
 * 目标平台：ESP32-S3 + 240x320 SPI 屏（ST7789 / GC9A01 等），RGB565
 *
 * 设计要点（都是为了压性能）：
 *  1. 只刷新球体脏矩形，背景不参与每帧重绘 —— SPI 传输量降 70%+
 *  2. 轮廓用 32~40 边形近似，扫描线填充无除法（预计算边增量 Q16）
 *  3. 径向渐变降级为垂直渐变查表（20 级 RGB565），体积感几乎无损
 *  4. 全部使用整数运算，sin 走 256 项查表 + 线性插值
 *  5. 渲染通过 span 回调输出，不绑定任何 GUI 库（LVGL / 裸屏都能接）
 *
 * 典型开销（R=62，开启 glow，20fps）：
 *    CPU      ~2.5 ms/帧
 *    SPI      ~7 ms/帧（脏区 186x186）
 *    静态 RAM  < 2 KB（不含显存/canvas buf）
 */
#ifndef ORB_H
#define ORB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 画质 / 性能 权衡开关 ==================== */

/** 轮廓多边形边数。
    2026-11-02：32 → 48 → 64。用户要"更圆"，64 边形在 220px 画布上
    弦高误差 < 0.4px，肉眼看就是正圆。多算的只是几十个顶点，可忽略。 */
#ifndef ORB_SEG
#define ORB_SEG             64
#endif

/** 是否绘制外发光。关闭后脏区缩小约 55%，帧率可翻倍 */
#ifndef ORB_ENABLE_GLOW
#define ORB_ENABLE_GLOW     1
#endif

/** 外发光半径 = R * ORB_GLOW_SCALE / 10。15 较柔和，12 更省 */
#ifndef ORB_GLOW_SCALE
#define ORB_GLOW_SCALE      15
#endif

/** 是否启用 256 项 sin 查表（关闭则用 sinf，慢 5~8 倍） */
#ifndef ORB_USE_TRIG_LUT
#define ORB_USE_TRIG_LUT    1
#endif

/**
 * 球体径向明暗带数：3=细腻暗角，2=省一次环带填充（差异很小）
 */
#ifndef ORB_LAYERS
#define ORB_LAYERS          3
#endif

/* ==================== 状态 ==================== */

typedef enum {
    ORB_IDLE      = 0,   /* 待机：慢呼吸 */
    ORB_LISTENING = 1,   /* 聆听：随麦克风起伏 */
    ORB_SPEAKING  = 2,   /* 说话：剧烈翻涌 */
    ORB_THINKING  = 3,   /* 思考：平稳 + 旋转弧 */
    ORB_STATE_MAX
} orb_state_t;

/* 参数索引 */
enum {
    P_BREATH = 0,   /* 呼吸幅度 */
    P_SPEED,        /* 呼吸速度 */
    P_WOB,          /* 表面形变 */
    P_WOBS,         /* 流动速度 */
    P_SPREAD,       /* 层间距 */
    P_GLOW,         /* 光晕强度 */
    P_REACT,        /* 音频驱动强度 */
    P_N
};

typedef struct {
    float p[P_N];
    uint8_t pal[3][3];   /* 3 个 RGB888 锚点：亮 / 中 / 暗 */
} orb_cfg_t;

/* ==================== 渲染后端 ==================== */

typedef struct orb_s orb_t;

/**
 * 扫描线回调：把 y 行 [x0, x1) 区间填成 color。
 * alpha: 0..32（5 位）。alpha >= 32 时可直接覆盖写入，不必混合。
 * 调用方需要自己做裁剪（x0/x1 已保证在 clip 内）。
 */
typedef void (*orb_span_cb)(orb_t *o, int y, int x0, int x1, uint16_t color, uint8_t alpha);

struct orb_s {
    /* ---- 用户配置 ---- */
    int16_t cx, cy;          /* 圆心（屏幕坐标） */
    int16_t r_base;          /* 基础半径（像素） */
    orb_span_cb span;        /* 绘制回调 */
    void *user;              /* 回调用户数据 */
    int16_t clip_x0, clip_y0, clip_x1, clip_y1;  /* 裁剪框（含边界） */

    /* ---- 运行状态 ---- */
    int      state;
    float    p[P_N];         /* 插值后的参数 */
    float    pal[3][3];      /* 插值后的调色板 */
    float    level;          /* 0..1 音频总能量 */
    float    band[4];        /* 0..1 四个频段能量（低→高） */
    uint32_t t_ms;           /* 内部时间 */
    float    spin;           /* 思考态旋转相位 */
    int      auto_audio;     /* 1=无麦克风时用合成包络；调 orb_feed_audio 后自动置 0 */

    /* ---- 每帧缓存 ---- */
    int16_t ux[ORB_SEG];     /* 单位轮廓 x（Q12） */
    int16_t uy[ORB_SEG];     /* 单位轮廓 y（Q12） */
    int16_t lx[ORB_SEG];     /* 当前层像素坐标 */
    int16_t ly[ORB_SEG];

    /* ---- 输出 ---- */
    int16_t dx0, dy0, dx1, dy1;   /* 本帧脏矩形（含边界） */
    int16_t r_out;                /* 本帧含形变的最大外接半径 */
    uint32_t stat_px;             /* 本帧填充像素数（性能统计用） */
    uint32_t stat_blend;          /* 其中需要 alpha 混合的像素数 */
    uint16_t dmax_q8;             /* 本帧轮廓形变峰值（Q8，1.0=256），算脏框用 */
};

/* ==================== API ==================== */

/** 初始化。cx,cy 为圆心，r_base 为基础半径 */
void orb_init(orb_t *o, int cx, int cy, int r_base);

/** 绑定绘制回调与裁剪框（通常裁剪到屏幕或 canvas 边界） */
void orb_set_backend(orb_t *o, orb_span_cb span, void *user);
void orb_set_clip(orb_t *o, int x0, int y0, int x1, int y1);

/** 切换状态（参数会平滑过渡过去） */
void orb_set_state(orb_t *o, orb_state_t s);

/** 无麦克风时用内置合成包络驱动（效果与网页版一致） */
void orb_set_auto_audio(orb_t *o, int enable);

/** 喂入真实音频特征。level 0..1，band[4] 各 0..1 */
void orb_feed_audio(orb_t *o, float level, const float band[4]);

/** 推进时间与参数插值。dt_ms 建议 33~50（20~30fps） */
void orb_update(orb_t *o, uint32_t dt_ms);

/** 绘制一帧（会调用 span 回调）。调用前需先 orb_update */
void orb_draw(orb_t *o);

/** 取得本帧脏矩形（含边界），供局部刷新使用 */
void orb_dirty(const orb_t *o, int *x0, int *y0, int *x1, int *y1);

/** 用背景色铺满脏矩形（每帧绘制前调用一次，alpha=32 表示不透明覆盖） */
void orb_clear(orb_t *o, uint16_t bg);

/** 覆盖某状态的默认参数（可选）。cfg 为 NULL 则恢复默认 */
void orb_override_state(orb_state_t s, const orb_cfg_t *cfg);

/* 工具：RGB888 -> RGB565 */
static inline uint16_t orb_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#ifdef __cplusplus
}
#endif
#endif /* ORB_H */
