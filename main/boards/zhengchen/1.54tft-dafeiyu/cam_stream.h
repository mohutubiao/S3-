#pragma once

namespace dafeiyu {

/* ── 图传（MJPEG）独立服务器：端口 81，仅小车模式启动 ──────────────────
 *
 * 为什么必须独立 httpd：
 *   ESP-IDF 的 httpd 只有**一个**工作任务，handler 不返回就把整台服务器堵死。
 *   /stream 是一条活到用户关页面的长连接，挂在 80 口上会让 /api/car（摇杆）、
 *   /api/state 全部排队 → 画面在动、车不动。所以图传自己起一台。
 *
 * 内容：
 *   GET /            图传遥控页（视频 + 摇杆 + 速度）
 *   GET /stream      multipart/x-mixed-replace，固定 10fps
 *   GET /frame       单帧 JPEG（Safari 只渲染 MJPEG 第一帧时的兜底）
 *
 * 屏幕：图传全程**不碰** Capture()，所以设备屏上的表情/时钟照常显示（用户要求「保持脸」）。
 */
bool StartCamStreamServer();
void StopCamStreamServer();
bool CamStreamRunning();

/* ── 空闲自动退出（2026-11-12）──────────────────────────────────────────
 * 用户关掉网页却没点「返回」时，图传模式会一直开着 —— 小智被停用，
 * 表现为「叫不应 / 像连不上服务器」。所以这里做看门狗：
 * 90 秒内 81 口一个请求都没有（页面每 20 秒有心跳），就回调上层退出图传模式。 */
typedef void (*CamIdleCallback)();
void SetCamIdleCallback(CamIdleCallback cb);

/* 图传遥控页的 HTML（2026-11-12）。
   由 **80 口**（dafeiyu_web.cc 的 /cam）提供，不再只在 81 口 —— 80 口永远在，
   页面一定能打开；视频/帧/心跳才走 81。这样图传服务器没起来时页面也不会白屏，
   还能自己点「重启图传」。 */
const char* CamPageHtml();

/* 踢掉正在跑的 /stream（2026-11-12）。
   为什么需要：81 口是**单任务** httpd，一条 /stream 会一直占着这个任务。
   用户第二次进图传时，上一页的连接可能还挂在浏览器里（返回缓存会保留 <img> 连接），
   新页面的 /stream 就永远排不上队 →「第一次有画面，第二次没画面，重启图传也没用」。
   页面加载 / 「重启图传」时调一次，旧流会在 100ms 内自己退出。 */
void KickCamStream();

}  // namespace dafeiyu
