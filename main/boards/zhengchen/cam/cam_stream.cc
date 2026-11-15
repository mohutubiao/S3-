#include "cam_stream.h"

#include "settings.h"

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpg/image_to_jpeg.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "CamStream"

namespace dafeiyu {
namespace {

/* ── 2026-11-11 图传 ─────────────────────────────────────────────────────
   用户要求：图传 10fps，且设备屏上的表情必须继续显示（「保持脸」）。

   「保持脸」天然成立：这里只调 esp_camera_fb_get()，**绝不**调
   Esp32Camera::Capture() —— 后者会 SetPreviewImage() 把表情藏起来 5 秒。
   所以推流期间屏幕照常跑表情/时钟。

   10fps 靠两端配合：
     · 相机在小车模式下按 QVGA(320x240) 取流（见 zhengchen_cam_board.cc）
     · 这里每帧把「编码耗时」从 100ms 周期里扣掉，余下补睡
   S3 没有硬件 JPEG 编码器（那是 P4 的），QVGA 软编约 40~80ms，10fps 是上限附近。 */
constexpr int kTargetFps = 10;
constexpr uint32_t kFrameMs = 1000 / kTargetFps;
constexpr int kJpegQuality = 62;   /* 够看清路况，单帧基本在 10KB 上下 */
constexpr const char* kPartHeader = "--camframe\r\nContent-Type: image/jpeg\r\n\r\n";

httpd_handle_t s_server = nullptr;
volatile int s_stop = 0;
volatile int s_active = 0;   /* 有 /stream 在跑：停服务器前必须先等它退出 */
volatile int s_kick = 0;     /* 踢掉当前这条 /stream（新页面进来时用） */
/* 2026-11-15：上次 Stop 没能把推流任务等退出 → 句柄保留、置本标志，
   下次 Start 时再清理。绝不能像以前那样直接把 s_server 置空 ——
   句柄一丢就再也 httpd_stop 不了，81 口被僵尸任务占死只能重启设备。 */
volatile int s_pending_stop = 0;

/* ── 空闲看门狗（2026-11-12 用户反馈「忘了点返回 → 叫不应」）── */
constexpr int64_t kIdleExitMs = 120 * 1000;  /* 120 秒没有任何请求就自动退出图传模式 */
volatile int64_t s_last_req_ms = 0;
CamIdleCallback s_idle_cb = nullptr;
esp_timer_handle_t s_idle_timer = nullptr;

void TouchClient() { s_last_req_ms = esp_timer_get_time() / 1000; }

void IdleCheck(void*) {
    if (s_server == nullptr || s_idle_cb == nullptr) {
        return;
    }
    const int64_t now = esp_timer_get_time() / 1000;
    if (now - s_last_req_ms < kIdleExitMs) {
        return;
    }
    ESP_LOGW(TAG, "图传 %d 秒无任何请求（页面已关？），自动退出图传模式",
             (int)(kIdleExitMs / 1000));
    s_idle_cb();
}

/* RGB565 字节序交换缓冲（PSRAM）。相机吐大端、编码器要小端，
   不换就是满屏色块花屏（和 Esp32Camera::Capture 里的 bswap 同一个道理）。 */
uint8_t* s_swap = nullptr;
size_t s_swap_size = 0;

bool EnsureSwap(size_t need) {
    if (s_swap != nullptr && s_swap_size >= need) {
        return true;
    }
    if (s_swap != nullptr) {
        heap_caps_free(s_swap);
        s_swap = nullptr;
        s_swap_size = 0;
    }
    s_swap = static_cast<uint8_t*>(heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_swap == nullptr) {
        ESP_LOGE(TAG, "换序缓冲分配失败（%u 字节）", (unsigned)need);
        return false;
    }
    s_swap_size = need;
    return true;
}

/* 32 位批量交换（一次 2 像素），比逐像素 bswap16 快一倍 */
uint8_t* SwapRgb565(const camera_fb_t* fb) {
    if (!EnsureSwap(fb->len)) {
        return nullptr;
    }
    const uint32_t* s32 = reinterpret_cast<const uint32_t*>(fb->buf);
    uint32_t* d32 = reinterpret_cast<uint32_t*>(s_swap);
    const size_t n32 = fb->len / 4;
    for (size_t i = 0; i < n32; i++) {
        const uint32_t v = s32[i];
        d32[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    }
    const size_t px = fb->len / 2;
    for (size_t i = n32 * 2; i < px; i++) {
        reinterpret_cast<uint16_t*>(s_swap)[i] =
            __builtin_bswap16(reinterpret_cast<const uint16_t*>(fb->buf)[i]);
    }
    return s_swap;
}

/* 把一帧原样（JPEG）或编码后（RGB565/YUYV/GRAY）作为 multipart 的一部分发出去 */
bool SendPart(httpd_req_t* req, camera_fb_t* fb) {
    if (fb->format == PIXFORMAT_JPEG) {
        char hdr[96];
        const int n = snprintf(hdr, sizeof(hdr),
                               "--camframe\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                               (unsigned)fb->len);
        if (httpd_resp_send_chunk(req, hdr, n) != ESP_OK) {
            return false;
        }
        return httpd_resp_send_chunk(req, reinterpret_cast<const char*>(fb->buf), fb->len) == ESP_OK;
    }

    v4l2_pix_fmt_t fmt = 0;
    switch (fb->format) {
        case PIXFORMAT_RGB565:    fmt = V4L2_PIX_FMT_RGB565; break;
        case PIXFORMAT_YUV422:    fmt = V4L2_PIX_FMT_YUYV;   break;
        case PIXFORMAT_GRAYSCALE: fmt = V4L2_PIX_FMT_GREY;   break;
        default:
            ESP_LOGW(TAG, "不支持的像素格式 %d", (int)fb->format);
            return false;
    }

    uint8_t* src = fb->buf;
    if (fb->format == PIXFORMAT_RGB565) {
        src = SwapRgb565(fb);
        if (src == nullptr) {
            return false;
        }
    }

    struct Ctx {
        httpd_req_t* req;
        bool ok;
    } ctx{req, true};

    const bool ok = image_to_jpeg_cb(
        src, fb->len, fb->width, fb->height, fmt, static_cast<uint8_t>(kJpegQuality),
        [](void* arg, size_t index, const void* data, size_t len) -> size_t {
            auto* c = static_cast<Ctx*>(arg);
            if (!c->ok) {
                return len;
            }
            if (index == 0) {
                if (httpd_resp_send_chunk(c->req, kPartHeader, strlen(kPartHeader)) != ESP_OK) {
                    c->ok = false;
                    return len;
                }
            }
            if (data != nullptr && len > 0) {
                if (httpd_resp_send_chunk(c->req, static_cast<const char*>(data), len) != ESP_OK) {
                    c->ok = false;
                }
            }
            return len;
        },
        &ctx);
    return ok && ctx.ok;
}

/* 对端还活着吗？浏览器关页面/切后台只断 TCP，send() 在半关闭下不报错，
   不探测就会一直推流 → socket 泄漏 → 再进图传黑屏（C3 那套踩过）。 */
bool PeerGone(httpd_req_t* req) {
    const int sfd = httpd_req_to_sockfd(req);
    if (sfd < 0) {
        return false;
    }
    char c;
    const int r = recv(sfd, &c, 1, MSG_DONTWAIT);
    if (r == 0) {
        return true;
    }
    if (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
        return true;
    }
    return false;
}

esp_err_t StreamHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=camframe");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    s_stop = 0;
    s_kick = 0;
    s_active = 1;
    TouchClient();
    int frames = 0;
    int fails = 0;
    ESP_LOGI(TAG, "图传开始（目标 %d fps）", kTargetFps);

    while (!s_stop && !s_kick) {
        const int64_t t0 = esp_timer_get_time();
        TouchClient();   /* 活着就一直是"有人在看"，别被自己的看门狗掐了 */

        if (PeerGone(req)) {
            ESP_LOGI(TAG, "对端断开，停止推流（已发 %d 帧）", frames);
            break;
        }

        /* 取最新帧：连拿两次丢掉旧帧，保证实时性（发旧帧画面会越走越滞后） */
        camera_fb_t* fb = nullptr;
        for (int i = 0; i < 2; i++) {
            camera_fb_t* f = esp_camera_fb_get();
            if (f == nullptr) {
                break;
            }
            if (fb != nullptr) {
                esp_camera_fb_return(fb);
            }
            fb = f;
        }
        if (fb == nullptr) {
            /* 取不到帧的两种正常情形：
               ① 刚进页面，相机还没 ready；② 语音侧正在拍照（Esp32Camera 抓着帧
                 不放，fb_count=2 时可能一个都拿不到）。都先等，别急着断流 ——
                 断流后浏览器不会自己重连，用户就以为「图传死了」。
               连续 2 秒都不行才算真坏。 */
            vTaskDelay(pdMS_TO_TICKS(100));
            /* ⚠️ 2026-11-14 修致命 bug（原写法 `frames == 0 || ++fails < 20`）：
               frames==0 时短路，fails 永远不增加 —— 摄像头一出不了帧
               （初始化失败 / cam_hal EV-EOF-OVF DMA 溢出），这个循环就永不退出，
               而且每 100ms TouchClient() 续命，连 90 秒空闲看门狗都掐不掉。
               后果不止「图传没画面」：81 口是单任务，被永久占用后用户点「返回」，
               StopCamStreamServer 里的 httpd_stop 会死等这台任务 → 80 口控制页
               整个失联，表现就是「车不动了 + 所有接口连接被关闭」。
               现在没帧也计数，5 秒拿不到就退出，保证 s_active 能清掉。 */
            if (++fails >= 50) {   /* 50 × 100ms = 5 秒 */
                ESP_LOGW(TAG, "5 秒拿不到帧，结束推流（已发 %d 帧）", frames);
                break;
            }
            continue;
        }
        fails = 0;

        const bool ok = SendPart(req, fb);
        esp_camera_fb_return(fb);
        if (!ok) {
            ESP_LOGW(TAG, "发送失败，结束推流（已发 %d 帧）", frames);
            break;
        }
        if (httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
            break;
        }
        frames++;

        /* 10fps 节拍：编码已经花掉的时间从周期里扣掉 */
        const int64_t used_ms = (esp_timer_get_time() - t0) / 1000;
        if (used_ms < static_cast<int64_t>(kFrameMs)) {
            vTaskDelay(pdMS_TO_TICKS(kFrameMs - used_ms));
        }
    }

    s_active = 0;
    s_stop = 0;
    httpd_resp_send_chunk(req, nullptr, 0);
    /* 栈水位（字节）：编码链最深用到哪。下次再遇到「启动失败/重启」时，
       这条日志能直接告诉我们 6144 到底够不够，不用再猜。 */
    ESP_LOGI(TAG, "图传结束，共 %d 帧，栈余量 %u 字节",
             frames, (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    return ESP_OK;
}

/* 单帧 JPEG：Safari 对 multipart/x-mixed-replace 只渲染第一帧，逐帧轮询兜底。
   编码进一块 PSRAM 缓冲再整块发（一帧 ~10KB，不值得为它牺牲内部 SRAM）。 */
esp_err_t FrameHandler(httpd_req_t* req) {
    TouchClient();
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera busy");
    }

    bool ok = false;
    if (fb->format == PIXFORMAT_JPEG) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
        ok = httpd_resp_send(req, reinterpret_cast<const char*>(fb->buf), fb->len) == ESP_OK;
        esp_camera_fb_return(fb);
        return ok ? ESP_OK : ESP_FAIL;
    }

    static uint8_t* buf = nullptr;
    static size_t cap = 0;
    if (cap < 64 * 1024) {
        if (buf != nullptr) {
            heap_caps_free(buf);
        }
        buf = static_cast<uint8_t*>(heap_caps_malloc(64 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        cap = (buf != nullptr) ? 64 * 1024 : 0;
    }
    if (buf == nullptr) {
        esp_camera_fb_return(fb);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }

    v4l2_pix_fmt_t fmt = 0;
    switch (fb->format) {
        case PIXFORMAT_RGB565:    fmt = V4L2_PIX_FMT_RGB565; break;
        case PIXFORMAT_YUV422:    fmt = V4L2_PIX_FMT_YUYV;   break;
        case PIXFORMAT_GRAYSCALE: fmt = V4L2_PIX_FMT_GREY;   break;
        default: break;
    }
    uint8_t* src = fb->buf;
    bool swap_ok = true;
    if (fb->format == PIXFORMAT_RGB565) {
        src = SwapRgb565(fb);
        swap_ok = (src != nullptr);
    }
    if (fmt == 0 || !swap_ok) {
        esp_camera_fb_return(fb);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "bad format");
    }

    struct Ctx {
        uint8_t* buf;
        size_t cap;
        size_t len;
        bool ok;
    } ctx{buf, cap, 0, true};

    ok = image_to_jpeg_cb(
        src, fb->len, fb->width, fb->height, fmt, static_cast<uint8_t>(kJpegQuality),
        [](void* arg, size_t, const void* data, size_t len) -> size_t {
            auto* c = static_cast<Ctx*>(arg);
            if (data != nullptr && len > 0) {
                if (!c->ok || c->len + len > c->cap) {
                    c->ok = false;
                    return len;
                }
                memcpy(c->buf + c->len, data, len);
                c->len += len;
            }
            return len;
        },
        &ctx);
    esp_camera_fb_return(fb);

    if (!ok || !ctx.ok || ctx.len == 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "encode failed");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, reinterpret_cast<const char*>(ctx.buf), ctx.len);
}

/* ── 图传遥控页 ─────────────────────────────────────────────────────────
   页面在 :81，摇杆必须打回 :80 的 /api/car（本服务器的任务被 /stream 占着）。
   跨端口 = 跨 origin，所以 /api/car 那边加了 Access-Control-Allow-Origin。
   速度和左右交换用 query 从主页带过来（localStorage 不跨 origin），本页再改就存本页。 */
const char* kPage = R"HTML(<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>图传遥控</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;user-select:none;-webkit-user-select:none}
body{margin:0;padding:8px;background:#0d0f14;color:#e8ecf5;font:14px/1.4 system-ui,-apple-system,"Microsoft YaHei",sans-serif;
  min-height:100vh;display:flex;flex-direction:column;align-items:center}
.cam{width:100%;max-width:520px;background:#000;border:1px solid #2a3348;border-radius:12px;overflow:hidden}
.cam img{display:block;width:100%;height:min(52vh,320px);object-fit:contain}
#tip{color:#fbbf24;font-size:12px;min-height:16px;margin:6px 0;text-align:center}
#pad{position:relative;width:min(60vw,230px);aspect-ratio:1/1;margin:4px auto;border-radius:50%;
  background:radial-gradient(circle,#232a3a,#141821);border:2px solid #2a3348;touch-action:none}
#knob{position:absolute;left:50%;top:50%;width:64px;height:64px;margin:-32px 0 0 -32px;border-radius:50%;
  background:linear-gradient(135deg,#4ade80,#22c55e);box-shadow:0 4px 12px rgba(0,0,0,.5)}
.row{display:flex;gap:8px;align-items:center;width:100%;max-width:520px;margin:5px 0}
button{padding:13px 6px;border:0;border-radius:10px;background:#2a3348;color:#fff;font-size:14px;flex:1}
#stop{background:#b3261e}
#spd{flex:1}
.lbl{font-size:12px;color:#8a93a8}
</style></head><body>
<div class="cam"><img id="v" alt=""></div>
<div id="tip">连接图传中…</div>
<div id="pad"><div id="knob"></div></div>
<div class="row">
  <button id="stop">■ 停车</button>
  <button id="camT">📷 关闭画面</button>
  <button id="poll">逐帧</button>
  <button id="back">⌂ 返回</button>
</div>
<div class="row"><span class="lbl">速度</span>
  <input type="range" id="spd" min="30" max="100" step="5">
  <span class="lbl" id="spdV" style="width:42px;text-align:right">80%</span></div>
<div class="row"><label class="lbl"><input type="checkbox" id="swp"> 左右轮交换</label></div>
<script>
const $ = s => document.querySelector(s);
const q = new URLSearchParams(location.search);
const qn = (k, d) => { const v = parseInt(q.get(k), 10); return isNaN(v) ? d : v; };

/* 速度 / 交换：本页独立保存（:81 和 :80 是不同 origin） */
let spd = parseInt(localStorage.getItem('c81spd'), 10);
if (isNaN(spd)) spd = qn('spd', 80);
let swp = localStorage.getItem('c81swp');
swp = (swp === null) ? (q.get('swp') === '1') : (swp === '1');
function uiSpeed(){
  $('#spd').value = spd; $('#spdV').textContent = spd + '%'; $('#swp').checked = swp;
}
$('#spd').oninput = () => { spd = +$('#spd').value; $('#spdV').textContent = spd + '%'; };
$('#spd').onchange = () => localStorage.setItem('c81spd', spd);
$('#swp').onchange = () => { swp = $('#swp').checked; localStorage.setItem('c81swp', swp ? '1' : '0'); };
uiSpeed();

/* 摇杆：打到 :80。ms=500 → 从机 500ms 没新指令就自己停，手机掉线不会飞车；
   按住不动靠 200ms 心跳续命（否则站着不动 0.5 秒车就停了）。 */
/* 页面本身由 80 口提供（见 dafeiyu_web.cc 的 /cam）—— 80 口永远在，
   所以本页一定能打开；视频/帧/心跳走 81 口。这样即使图传服务器没起来，
   页面也能显示出来并自己点「重启图传」，不会再出现整页打不开的情况。 */
const ROOT = 'http://' + location.hostname;
const CAM = 'http://' + location.hostname + ':81';
const API = ROOT + '/api/car?';
/* 2026-11-14：页面一打开、以及滑块每次改动，都把本页速度写成从机的全局速度。
   从机重启后这个值是 0，不推这一下，摇杆推到底也不动（用户实测就是这个）。
   覆盖上面那个只存 localStorage 的 onchange。 */
function pushSpeed(){ fetch(ROOT + '/api/speed?pct=' + spd).catch(() => {}); }
pushSpeed();
$('#spd').onchange = () => { localStorage.setItem('c81spd', spd); pushSpeed(); };
let lastAt = 0, pendS = 0, pendT = 0, timer = null;
function sendNow(s, t){
  /* 2026-11-14 修「图传遥控推杆车不动」：
     全局速度由从机在 Drive() 入口统一缩放（主机 /api/speed 下发），这里
     不能再乘 spd/100 —— 那是双重缩放。更致命的是本页从来没下发过
     /api/speed，而从机重启后全局速度默认是 0，所以摇杆推到底也不动。
     现在速度改由 pushSpeed() 单独下发，见下面 ROOT 定义之后。 */
  let S = s, T = t;
  if (swp) S = -S;
  fetch(API + 'steer=' + S + '&throttle=' + T + '&ms=500').catch(() => {});
}
function send(s, t){
  pendS = s; pendT = t;
  const now = Date.now();
  if (now - lastAt >= 40){ lastAt = now; sendNow(s, t); return; }
  if (!timer) timer = setTimeout(() => { timer = null; lastAt = Date.now(); sendNow(pendS, pendT); }, 40);
}
function halt(){
  if (timer){ clearTimeout(timer); timer = null; }
  lastAt = 0;
  sendNow(0, 0);
}

const pad = $('#pad'), knob = $('#knob');
let drag = false, jS = 0, jT = 0, keep = null;
function knobAt(dx, dy){
  knob.style.left = 'calc(50% + ' + dx + 'px)';
  knob.style.top = 'calc(50% + ' + dy + 'px)';
}
function move(cx, cy){
  const r = pad.getBoundingClientRect();
  const max = Math.max(1, r.width / 2 - knob.offsetWidth / 2);
  let dx = cx - (r.left + r.width / 2), dy = cy - (r.top + r.height / 2);
  const d = Math.sqrt(dx * dx + dy * dy);
  if (d > max){ dx = dx / d * max; dy = dy / d * max; }
  knobAt(dx, dy);
  jS = Math.round(dx / max * 100); jT = Math.round(-dy / max * 100);
  send(jS, jT);
}
function stopJoy(){
  if (!drag) return;
  drag = false; jS = 0; jT = 0;
  if (keep){ clearInterval(keep); keep = null; }
  knobAt(0, 0);
  halt();
}
pad.addEventListener('pointerdown', e => {
  e.preventDefault(); drag = true;
  pad.setPointerCapture(e.pointerId);
  if (!keep) keep = setInterval(() => { if (drag) send(jS, jT); }, 200);
  move(e.clientX, e.clientY);
});
pad.addEventListener('pointermove', e => { if (drag) move(e.clientX, e.clientY); });
pad.addEventListener('pointerup', stopJoy);
pad.addEventListener('pointercancel', stopJoy);
document.addEventListener('visibilitychange', () => { if (document.hidden) stopJoy(); });

/* 画面：默认 MJPEG；Safari 只显示第一帧时点「逐帧」改 10fps 轮询 */
const v = $('#v');
let camOn = true, pollTimer = null;
function setTip(s){ $('#tip').textContent = s || ''; }
v.onload = () => setTip('');
v.onerror = () => {
  if (!camOn) return;
  /* 连接失败时把按钮切成「重启图传」：一次点击就会重新 camstart 再拉流，
     不用先点关闭再点打开（用户就是这么被绕晕的）。 */
  camOn = false;
  $('#camT').textContent = '📷 重启图传';
  setTip('画面断开：点「重启图传」');
};

/* 确保设备端的图传服务器起来了（页面在 80 口，所以这一步永远能打到） */
async function ensureServer(){
  try {
    const r = await fetch(ROOT + '/api/camstart', {method: 'POST'});
    const t = (await r.text()).trim();
    if (t !== 'ok') { setTip(t || '图传启动失败'); return false; }
    return true;
  } catch (e) { setTip('连不上设备（WiFi？）'); return false; }
}
async function startStream(){
  if (pollTimer){ clearInterval(pollTimer); pollTimer = null; }
  camOn = true;
  $('#camT').textContent = '📷 关闭画面';
  setTip('启动图传…');
  v.removeAttribute('src');                       /* 先断掉旧连接，别让浏览器复用 */
  if (!(await ensureServer())) { camOn = false; $('#camT').textContent = '📷 重启图传'; return; }
  await new Promise(r => setTimeout(r, 250));     /* 等设备端把旧推流退干净（单任务） */
  v.src = CAM + '/stream?t=' + Date.now();
  setTip('连接画面…');
}
function stopCam(){
  camOn = false;
  if (pollTimer){ clearInterval(pollTimer); pollTimer = null; }
  v.removeAttribute('src');
  $('#camT').textContent = '📷 打开画面';
  setTip('');
}
$('#camT').onclick = () => { camOn ? stopCam() : startStream(); };
$('#poll').onclick = () => {
  if (pollTimer){ clearInterval(pollTimer); pollTimer = null; setTip('逐帧已关，点「逐帧」外的按钮可回连续画面'); return; }
  stopCam(); camOn = true;
  $('#camT').textContent = '📷 关闭画面';
  pollTimer = setInterval(() => { v.src = CAM + '/frame?t=' + Date.now(); }, 100);
  setTip('逐帧模式（10fps）');
};
$('#stop').onclick = () => { stopJoy(); halt(); };
/* 退出图传模式：先断流（服务器才能立刻停），再叫设备关掉 81 口、把小智还回去，
   最后跳回控制页。顺序不能反 —— 服务器一停页面就白了，用户看不到任何反馈。 */
$('#back').onclick = async () => {
  halt();
  stopCam();
  setTip('正在退出图传…');
  $('#back').disabled = true;
  try { await fetch(ROOT + '/api/camstop', {method: 'POST'}); } catch (e) {}
  location.href = ROOT + '/';
};
window.addEventListener('pagehide', () => { halt(); });
/* 心跳：每 30 秒告诉设备「页面还在」，不然设备 120 秒没收到请求就自动退出图传
   （用户点「关闭画面」只停视频，不该算离开；直接关页签才是真离开） */
setInterval(() => { fetch(CAM + '/api/keepalive').catch(() => {}); }, 30000);
startStream();
</script></body></html>)HTML";

esp_err_t PageHandler(httpd_req_t* req) {
    TouchClient();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kPage, HTTPD_RESP_USE_STRLEN);
}

/* 心跳：页面每 20 秒打一次。用户点「关闭画面」只停视频、不算离开，
   靠这个把图传模式留住，不至于 90 秒就被看门狗掐掉。 */
esp_err_t KeepAliveHandler(httpd_req_t* req) {
    TouchClient();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, "ok");
}

/* 关 Nagle：每帧 JPEG 一次写，开着会被 delayed ACK 拖成几十上百毫秒一帧 */
esp_err_t NoDelay(httpd_handle_t /*hd*/, int sockfd) {
    int nodelay = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    return ESP_OK;
}

}  // namespace

const char* CamPageHtml() { return kPage; }

void KickCamStream() {
    if (s_active) {
        ESP_LOGW(TAG, "踢掉旧推流（新页面/重连进来）");
        s_kick = 1;
    }
}

bool CamStreamRunning() {
    /* pending_stop：服务器句柄还在但上次没停干净，实际不可用 ——
       对上层必须报 false，否则 /api/camstart 会走「已在运行就踢流」
       的捷径，永远不触发清理，81 口就一直占着。 */
    return s_server != nullptr && !s_pending_stop;
}

void SetCamIdleCallback(CamIdleCallback cb) {
    s_idle_cb = cb;
}

bool StartCamStreamServer() {
    if (s_server != nullptr && !s_pending_stop) {
        return true;
    }
    /* 2026-11-15：上次 Stop 没把推流任务等退出，句柄被保留下来了。
       这里再给一次清理机会 —— 推流任务通常早已自己退出，此时
       httpd_stop 能干净收掉，81 口就能重新 bind。
       这就是「点重启图传能自愈」的关键（以前句柄被丢，永远自愈不了）。 */
    if (s_server != nullptr && s_pending_stop) {
        ESP_LOGW(TAG, "上次图传没停干净，先清理旧的 81 口服务器");
        s_stop = 1;
        for (int i = 0; i < 40 && s_active != 0; i++) {   /* 最多再等 2 秒 */
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (s_active != 0) {
            ESP_LOGE(TAG, "81 口仍被上次的推流占着（%d），只能重启设备", (int)s_active);
            return false;
        }
        httpd_stop(s_server);
        s_server = nullptr;
        s_pending_stop = 0;
        s_stop = 0;
        ESP_LOGW(TAG, "旧 81 口服务器已清理，继续启动新服务器");
    }
    /* 只有小车模式才需要图传：正常模式下多起一台 httpd（16KB 栈）不值当，
       内部 SRAM 本来就紧（实测低谷只剩 40KB 上下）。 */
    Settings ls("lcd_display", false);
    if (ls.GetInt("car_mode", 0) == 0) {
        ESP_LOGI(TAG, "非小车模式，图传服务器不启动");
        return false;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 81;
    cfg.ctrl_port = 32769;        /* 与主 httpd 的 32768 错开 */
    cfg.max_open_sockets = 3;     /* 1 条 /stream + 逐帧轮询 + 余量 */
    cfg.max_uri_handlers = 8;
    /* ⚠️ 2026-11-12 实测教训：这里原来是 16384（照抄 C3 那套带 TLS 的图传）。
       本服务器是**明文** HTTP，根本用不到 TLS 的大栈，却照样把 16KB 内部 RAM 拿走，
       车模式下稳态只剩 6547 字节 → mbedTLS 申请 AES 上下文失败
       →「esp-aes: Failed to allocate memory / SSL send failed errno=119」
       → WS 发不出去，表现为「设备连不上服务器」。
       改成 6144（和主控制页一致），把 10KB 还给 TLS。 */
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 10;
    cfg.send_wait_timeout = 3;    /* 丢包时别让整条流卡 30 秒 */
    cfg.task_priority = 4;        /* 低于主 httpd：摇杆请求要能抢占 */
    cfg.core_id = 1;              /* 网络任务全部避开核 0（音频/唤醒词） */
    cfg.open_fn = NoDelay;

    /* ⚠️ 2026-11-12 用户实测：点图传报「内部内存不足」，但空闲时明明还有 28KB。
       原因是内部 SRAM 是浮动的 —— 刚聊完天时 WS+TLS 会话没断、音频管线还在跑，
       那一刻可能只剩几 KB，6KB 的栈就申请不到。
       改成按档位退让：6144 → 5120 → 4608。每档都打剩余量和 esp_err_t，
       下次再失败就能一眼看出是「没内存」还是「端口被占」，不用猜。 */
    const size_t kStackLadder[] = {6144, 5120, 4608};
    esp_err_t err = ESP_FAIL;
    for (size_t st : kStackLadder) {
        cfg.stack_size = st;
        const uint32_t free_int = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const uint32_t free_blk = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        httpd_handle_t hd = nullptr;
        err = httpd_start(&hd, &cfg);
        if (err == ESP_OK) {
            s_server = hd;
            ESP_LOGI(TAG, "图传服务器已启动，栈 %u（启动前内部SRAM：空闲 %u / 最大块 %u）",
                     (unsigned)st, free_int, free_blk);
            break;
        }
        ESP_LOGW(TAG, "图传启动失败 stack=%u err=%s 内部SRAM 空闲 %u / 最大块 %u",
                 (unsigned)st, esp_err_to_name(err), free_int, free_blk);
        if (err != ESP_ERR_HTTPD_TASK && err != ESP_ERR_HTTPD_ALLOC_MEM) {
            break;   /* 不是内存问题（例如 81 口被占），降栈也没用 */
        }
    }
    if (s_server == nullptr) {
        ESP_LOGE(TAG, "图传服务器起不来，最后错误 %s", esp_err_to_name(err));
        return false;
    }

    const httpd_uri_t routes[] = {
        {"/", HTTP_GET, PageHandler, nullptr},
        {"/stream", HTTP_GET, StreamHandler, nullptr},
        {"/frame", HTTP_GET, FrameHandler, nullptr},
        {"/api/keepalive", HTTP_GET, KeepAliveHandler, nullptr},
    };
    for (const auto& r : routes) {
        httpd_register_uri_handler(s_server, &r);
    }

    /* 看门狗：15 秒查一次，90 秒没请求就回调上层退出图传模式 */
    TouchClient();
    if (s_idle_timer == nullptr) {
        const esp_timer_create_args_t targs = {
            .callback = &IdleCheck,
            .arg = nullptr,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "cam_idle",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&targs, &s_idle_timer);
    }
    if (s_idle_timer != nullptr) {
        esp_timer_stop(s_idle_timer);
        esp_timer_start_periodic(s_idle_timer, 15 * 1000 * 1000);
    }
    ESP_LOGI(TAG, "图传已就绪 -> http://<设备IP>:81/ （内部SRAM剩余 %u 字节）",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

void StopCamStreamServer() {
    if (s_server == nullptr) {
        return;
    }
    /* ⚠️ 顺序铁律：httpd_stop() 是靠往 ctrl 端口发消息、然后**死等**任务退出
       （httpd_main.c 里 while(hd_td.status != THREAD_STOPPED) sleep(100)）。
       而 httpd 是单任务，正在跑 /stream 的 handler 根本没法回头处理那条消息
       → 直接 httpd_stop 会把调用者（配网流程）永久卡死。
       所以先置停止位、等 handler 自己退干净，再停服务器。 */
    if (s_idle_timer != nullptr) {
        esp_timer_stop(s_idle_timer);   /* 先停看门狗，别在停服务器过程中又回调一次 */
    }
    s_stop = 1;
    /* 等 9 秒：推流循环最坏情况 5 秒（拿不到帧的自愈超时）+ send 3 秒 = 8 秒。
       原来是 6 秒，覆盖不住最坏情况 → 经常走到「放弃」分支留下僵尸。 */
    for (int i = 0; i < 180 && s_active != 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    /* ⚠️ 任务还没退就绝不能 httpd_stop —— 它是死等任务退出的，
       而本函数是被 80 口 handler 调的，卡在这里等于把整个控制页一起弄死
       （用户实测「点返回后车不动、所有接口连不上」就是这么来的）。
       2026-11-15 修正：**保留句柄**并置 s_pending_stop，下次 Start 再清理。
       以前在这里 s_server = nullptr，句柄一丢就永远 httpd_stop 不了，
       81 口被僵尸任务占死、点「重启图传」也没用，只能重启设备。 */
    if (s_active != 0) {
        ESP_LOGE(TAG, "推流任务 9 秒内没退出，保留句柄待下次 Start 清理（不 httpd_stop，避免卡死调用者）");
        s_pending_stop = 1;
        s_stop = 0;
        return;
    }
    httpd_stop(s_server);
    s_server = nullptr;
    s_pending_stop = 0;
    ESP_LOGI(TAG, "图传服务器已停止");
}

}  // namespace dafeiyu
