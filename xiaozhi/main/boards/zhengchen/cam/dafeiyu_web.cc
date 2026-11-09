#include "dafeiyu_web.h"

#include "dafeiyu_display.h"
#include "pet_themes.h"      // PET_THEME_N / pet_theme_name：/api/state 的主题名列表
#include "dafeiyu_storage.h"
#include "ws2812_led.h"     // 板载彩灯：/api/led
#include "wake_word_models.h"   // 唤醒词选择：/api/wakeword
#include "board.h"          // IP 自动定位要出网（GetNetwork()->CreateHttp）
#include "settings.h"       // 位置存 NVS（namespace "location"）

#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>     // 日志里直接打设备 IP
#include <esp_ota_ops.h>   // 进游戏机：切启动槽
#include <esp_partition.h> // 找 ota_1 分区
#include <esp_system.h>    // esp_restart
#include <freertos/FreeRTOS.h>
#include <freertos/task.h> // vTaskDelay
#include <nvs.h>
#include <cJSON.h>

/* LCD 颜色模式开关（实现在 zhengchen_cam_board.cc，2026-11-04 反色修复） */
extern void DafeiyuSetLcdColorMode(int invert, int rgb_swap);
extern int DafeiyuGetLcdInvert();
extern int DafeiyuGetLcdRgbSwap();

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define TAG "DafeiyuWeb"

namespace dafeiyu {

namespace {
constexpr const char* kNs = "petui";
constexpr const char* kKeyConfig = "cfg";
constexpr const char* kKeyVersion = "ver";

UiConfig g_config;
uint32_t g_version = 1;

void LoadConfig() {
    nvs_handle_t handle;
    if (nvs_open(kNs, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(g_config);
        if (nvs_get_blob(handle, kKeyConfig, &g_config, &len) != ESP_OK ||
            len != sizeof(g_config)) {
            g_config = UiConfig{};
        }
        /* 2026-11-02：大眼睛方案已删除。旧配置里若存着 1，必须归零 ——
           否则唤醒时既没有眼睛（不再创建）也没有呼吸球，只剩黑屏。 */
        if (g_config.wake_style == 1) {
            g_config.wake_style = 0;
        }
        uint32_t v = 0;
        if (nvs_get_u32(handle, kKeyVersion, &v) == ESP_OK && v > 0) {
            g_version = v;
        }
        nvs_close(handle);
    }
}
}  // namespace

uint32_t ConfigVersion() {
    return g_version;
}

UiConfig GetConfig() {
    return g_config;
}

void SaveConfig(const UiConfig& config) {
    g_config = config;
    g_version++;
    nvs_handle_t handle;
    if (nvs_open(kNs, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_blob(handle, kKeyConfig, &g_config, sizeof(g_config));
    nvs_set_u32(handle, kKeyVersion, g_version);
    nvs_commit(handle);
    nvs_close(handle);
}

namespace {

// ---------------------------------------------------------------- 控制页面
const char* kPage = R"HTML(<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>大肥鱼养成</title>
<style>
*{box-sizing:border-box}
body{margin:0;padding:16px;background:#12141c;color:#e8ecf5;font:15px/1.5 system-ui,-apple-system,"Microsoft YaHei",sans-serif}
h1{font-size:20px;margin:0 0 4px}
.card{background:#1b1f2b;border:1px solid #2a3348;border-radius:12px;padding:14px;margin:12px 0}
.row{display:flex;align-items:center;gap:10px;margin:8px 0}
.bar{flex:1;height:10px;background:#2a3348;border-radius:6px;overflow:hidden}
.bar>i{display:block;height:100%;border-radius:6px}
.lbl{width:64px;color:#8a93a8;font-size:13px}
.val{width:42px;text-align:right;font-variant-numeric:tabular-nums;font-size:13px}
button{background:#3d5afe;color:#fff;border:0;border-radius:8px;padding:9px 14px;font-size:14px;cursor:pointer}
button.ghost{background:#2a3348}
button:active{opacity:.75}
.chips{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px}
input[type=text],input[type=number]{background:#11141d;color:#e8ecf5;border:1px solid #2a3348;border-radius:8px;padding:9px 10px}
input[type=color]{width:46px;height:34px;border:0;background:none;padding:0}
.hint{color:#8a93a8;font-size:12px;margin-top:6px}
#preview{width:100%;max-width:320px;border-radius:8px;display:none;margin-top:10px}
.tag{display:inline-block;background:#2a3348;border-radius:6px;padding:2px 8px;font-size:12px;color:#8a93a8}
</style></head><body>
<h1>大肥鱼控制台</h1>
<div class="tag" id="meta">读取中…</div>
<div class="chips" style="margin-top:6px">
  <button class="ghost" id="refresh">刷新状态</button>
  <span class="tag">页面 v1106e</span>
</div>

<div class="card">
  <b>屏幕视图</b>
  <div class="chips">
    <button id="vTheme">主题</button>
    <button id="vClock">时钟</button>
    <button id="vWallpaper">壁纸</button>
  </div>
  <div class="row" style="margin-top:8px"><span class="lbl">肥鱼样式</span>
    <select id="vThemeStyle" style="flex:1"></select></div>
  <div class="row"><span class="lbl">时钟样式</span>
    <select id="vClockStyle" style="flex:1"></select></div>
  <div class="hint">两个下拉框常驻：选肥鱼样式切到对应主题，选时钟样式切到对应时钟。
  屏幕上也能切：双击音量键（时钟下切时钟样式）、长按 BOOT 开菜单。</div>
</div>

<div class="card">
  <b>自定义壁纸</b>
  <div class="row"><input type="file" id="file" accept="image/*" style="flex:1"></div>
  <canvas id="cv" width="320" height="240" style="display:none;width:100%;max-width:320px;border-radius:8px;margin-top:8px;touch-action:none;cursor:grab"></canvas>
  <div class="row" id="zoomRow" style="display:none"><span class="lbl">缩放</span>
    <input type="range" id="zoom" style="flex:1"></div>
  <img id="preview">
  <div class="chips"><button id="upload">上传为壁纸</button><button class="ghost" id="delwp">删除壁纸</button></div>
  <div class="hint">像手机设壁纸一样：<b>拖动图片</b>选位置，<b>滑块或双指捏合</b>缩放，
  预览就是屏幕最终显示的样子，满意后点上传。</div>
</div>

<div class="card">
  <b>位置</b>
  <div class="row"><input type="text" id="loc" placeholder="如：广东省 深圳市" style="flex:1"><button id="saveloc">保存</button></div>
  <div class="chips"><button class="ghost" id="autoloc">自动获取（IP 定位）</button></div>
  <div class="hint">保存后，语音问天气、当地时间时 AI 会先查这里知道你在哪。IP 定位是城市级精度，想更准就手动填。</div>
</div>

<div class="card">
  <b>板载彩灯</b>
  <div class="row"><span class="lbl">颜色</span><input type="color" id="ledc" value="#3d5afe">
    <span class="lbl">亮度</span><input type="number" id="ledb" value="50" min="0" max="100" style="width:70px"></div>
  <div class="row"><span class="lbl">效果</span>
    <select id="lede" style="flex:1;background:#11141d;color:#e8ecf5;border:1px solid #2a3348;border-radius:8px;padding:8px 10px">
      <option value="on">常亮</option>
      <option value="breathe">呼吸</option>
      <option value="blink">闪烁</option>
      <option value="rainbow">彩虹</option>
      <option value="music">律动</option>
      <option value="off">关闭</option>
    </select>
    <button id="ledset">应用</button>
  </div>
  <div class="hint">也可以直接对它说「把灯调成红色」「来个彩虹」。</div>
</div>

<div class="card">
  <b>唤醒词</b>
  <div class="row"><span class="lbl">方式</span>
    <select id="wwmode" style="flex:1;background:#11141d;color:#e8ecf5;border:1px solid #2a3348;border-radius:8px;padding:8px 10px"></select>
  </div>
  <div class="row" id="wwcustomrow" style="display:none">
    <input type="text" id="wwcmd" placeholder="拼音，如 xiao tu dou" style="flex:1">
    <input type="text" id="wwdisp" placeholder="中文，如 小土豆" style="width:120px">
  </div>
  <div class="row" id="wwthrow" style="display:none">
    <span class="lbl">灵敏度</span><input type="number" id="wwth" min="1" max="99" value="20" style="width:70px">
    <span class="lbl">越小越敏感</span>
  </div>
  <div class="chips"><button id="wwset">保存唤醒词</button></div>
  <div class="hint">模型在 assets 分区里、不在固件里，所以换词不用重新烧录，但改完必须重启设备才生效。<br>
  自定义词的<b>左边填拼音</b>（如 <code>xiao tu dou</code>，字母+空格），<b>右边填中文</b>（如 小土豆）。
  拼音栏里填中文会让语音库崩溃、设备反复重启，所以保存时会被拦下来。</div>
</div>

<div class="card">
  <b>游戏机</b>
  <div class="chips"><button id="gameboot">进游戏机（NES）</button></div>
  <div class="hint">重启后进入 GameBox NES 模拟器（需已烧录游戏固件）。
  在游戏机的「设置」里有「返回小智」，切换不会丢任何设置。</div>
</div>

<div class="card">
  <b>屏幕颜色（反色/偏色修复）</b>
  <div class="chips"><button id="invert">反色切换</button><button id="rgbswap">红蓝互换</button></div>
  <div class="hint">如果屏幕颜色发反或红蓝颠倒，点上面的按钮实时切换，自动保存。
  现在的状态：反色=<span id="lcdi">?</span> 红蓝互换=<span id="lcds">?</span></div>
</div>

<div class="card">
  <b>音乐可视化（2026-11-05）</b>
  <div class="row"><label style="flex:1"><input type="checkbox" id="spec"> 频谱条（播放音乐时居中显示）</label></div>
    <div class="row"><label style="flex:1"><input type="checkbox" id="dance"> 彩灯律动（灯随音乐/人声跳动，开启时覆盖灯效）</label></div>
  <div class="hint">提示：对它说「播放 XXX」点歌后，再说「把歌名报上来」它会调
  <code>self.music.now</code> 把歌名打到屏幕顶部。三个开关即时生效并自动保存。</div>
</div>

<script>
const $ = s => document.querySelector(s);
/* ── 屏幕视图（2026-11-06 用户要求）：主题/时钟/壁纸 + 样式选择 ── */
const kClockStyleNames = ['极光','暖橙','青翠','极简'];
function viewRender(j){
  const v = j.view || 'theme';
  const btns = [[$('#vTheme'),'theme'],[$('#vClock'),'clock'],[$('#vWallpaper'),'wallpaper']];
  btns.forEach(([b,t]) => b.style.background = (t===v) ? '#3d5afe' : '');
  const ts = $('#vThemeStyle');
  if (!ts.options.length) {
    (j.theme_names||[]).forEach((n,i)=>{ const o=document.createElement('option'); o.value=i; o.textContent=(i+1)+'. '+n; ts.appendChild(o); });
  }
  /* 正在被用户拖动的下拉不回写，避免选到一半被刷新打断 */
  if (v==='theme' && document.activeElement !== ts) ts.value = String(j.style||0);
  const cs = $('#vClockStyle');
  if (!cs.options.length) {
    kClockStyleNames.forEach((n,i)=>{ const o=document.createElement('option'); o.value=i; o.textContent=n; cs.appendChild(o); });
  }
  if (v==='clock' && document.activeElement !== cs) cs.value = String(j.style||0);
}
$('#vTheme').onclick = () => post('/api/view?target=theme').then(state);
$('#vClock').onclick = () => post('/api/view?target=clock').then(state);
$('#vWallpaper').onclick = () => post('/api/view?target=wallpaper').then(state);
$('#vThemeStyle').onchange = () => post('/api/view?target=theme&style=' + $('#vThemeStyle').value).then(state);
$('#vClockStyle').onchange = () => post('/api/view?target=clock&style=' + $('#vClockStyle').value).then(state);

async function state(){
  const r = await fetch('/api/state?t=' + Date.now()); const j = await r.json(); window.j = j;
  /* 养成系列已整体删除：状态行只保留还有意义的信息 */
  $('#meta').textContent = '壁纸 ' + (j.wallpaper ? '已设置' : '无');
  viewRender(j);
  /* 2026-11-05 音乐可视化开关回显 */
  if (j.spectrum !== undefined) $('#spec').checked = !!j.spectrum;
  if (j.subtitle !== undefined) $('#sub').checked = !!j.subtitle;
  if (j.dance !== undefined) $('#dance').checked = !!j.dance;
  if (j.loc !== undefined) { $('#loc').value = j.loc || ''; }
  if (j.led) { $('#ledc').value = j.led.color; $('#ledb').value = j.led.brightness; $('#lede').value = j.led.effect; }
}
async function post(u, body, type){
  const opt = {method:'POST'};
  if (body !== undefined){ opt.body = body; opt.headers = {'Content-Type': type || 'text/plain'}; }
  const r = await fetch(u, opt); return r.text();
}
$('#refresh').onclick = state;
/* 2026-11-05 音乐可视化三开关：即时生效 + 自动保存 */
$('#spec').onchange = () => post('/api/config?spec=' + ($('#spec').checked?1:0)).then(state);
$('#dance').onchange = () => post('/api/config?dance=' + ($('#dance').checked?1:0)).then(state);

$('#saveloc').onclick = () => {
  const v = $('#loc').value.trim();
  if (v) post('/api/location?set=' + encodeURIComponent(v)).then(r => { alert(r === 'ok' ? '已保存' : r); state(); });
};
$('#autoloc').onclick = async () => { alert(await post('/api/location?auto=1')); state(); };
$('#ledset').onclick = () => post('/api/led?color=' + encodeURIComponent($('#ledc').value) + '&b=' + $('#ledb').value + '&effect=' + $('#lede').value).then(state);

function wwToggle(){
  const custom = $('#wwmode').value === 'custom';
  $('#wwcustomrow').style.display = custom ? 'flex' : 'none';
  $('#wwthrow').style.display = custom ? 'flex' : 'none';
}
function wwRender(j){
  const sel = $('#wwmode'); sel.innerHTML = '';
  (j.models || []).forEach(m => {
    const o = document.createElement('option');
    o.value = m.id; o.textContent = m.name; sel.appendChild(o);
  });
  if (j.multinet) {
    const o = document.createElement('option');
    o.value = 'custom'; o.textContent = '自定义词（Multinet）'; sel.appendChild(o);
  }
  if (j.mode) sel.value = j.mode;
  $('#wwcmd').value = j.cmd || '';
  $('#wwdisp').value = j.display || '';
  $('#wwth').value = j.threshold || 20;
  wwToggle();
}
async function wwLoad(){
  const r = await fetch('/api/wakeword?t=' + Date.now());
  wwRender(await r.json());
}
$('#wwmode').onchange = wwToggle;
$('#wwset').onclick = async () => {
  const mode = $('#wwmode').value;
  let u = '/api/wakeword?mode=' + encodeURIComponent(mode);
  if (mode === 'custom') {
    u += '&cmd=' + encodeURIComponent($('#wwcmd').value.trim());
    u += '&display=' + encodeURIComponent($('#wwdisp').value.trim());
    u += '&th=' + $('#wwth').value;
  }
  const r = await post(u);
  alert(r);
};
wwLoad();

$('#gameboot').onclick = async () => {
  if (!confirm('重启进入游戏机（NES）？')) return;
  alert(await post('/api/gameboot'));
};

/* LCD 颜色模式（2026-11-04）：实时切换反色 / 红蓝互换 */
$('#invert').onclick = () => post('/api/config?invert=' + (j.lcd_invert ? 0 : 1)).then(state);
$('#rgbswap').onclick = () => post('/api/config?rgb_swap=' + (j.lcd_rgb_swap ? 0 : 1)).then(state);

/* ── 自定义壁纸：拖动 + 缩放裁剪（2026-11-06 用户明确要手机设壁纸那种交互）──
   选图后按 cover 倍率铺满，手指/鼠标拖动平移，滑块或双指捏合缩放，
   预览即所得（所见 320×240 就是上传到屏幕的样子）。 */
let wImg = null, wScale = 1, wMin = 1, wOx = 0, wOy = 0;
let wLast = null, wPinch = null;
const cv = $('#cv');
const wPts = new Map();
function wClamp(){
  if (!wImg) return;
  const w = wImg.width*wScale, h = wImg.height*wScale;
  const mx = Math.max(0,(w-320)/2), my = Math.max(0,(h-240)/2);
  wOx = Math.max(-mx, Math.min(mx, wOx));
  wOy = Math.max(-my, Math.min(my, wOy));
}
function wDraw(){
  const ctx = cv.getContext('2d');
  ctx.fillStyle = '#000'; ctx.fillRect(0,0,320,240);
  if (!wImg) return;
  const w = wImg.width*wScale, h = wImg.height*wScale;
  ctx.drawImage(wImg, (320-w)/2 + wOx, (240-h)/2 + wOy, w, h);
}
$('#file').onchange = e => {
  const f = e.target.files[0]; if (!f) return;
  const img = new Image(); img.onload = () => {
    wImg = img;
    wMin = Math.max(320/img.width, 240/img.height);   /* cover：最小也要铺满 */
    wScale = wMin; wOx = 0; wOy = 0;
    const z = $('#zoom');
    z.min = wMin; z.max = wMin*4; z.step = 0.01; z.value = wMin;
    cv.style.display = 'block';
    $('#zoomRow').style.display = 'flex';
    $('#preview').style.display = 'none';
    wDraw();
  };
  img.src = URL.createObjectURL(f);
};
cv.addEventListener('pointerdown', e => {
  wPts.set(e.pointerId, {x:e.clientX, y:e.clientY});
  wLast = {x:e.clientX, y:e.clientY};
  cv.setPointerCapture(e.pointerId);
});
cv.addEventListener('pointermove', e => {
  if (!wImg || !wPts.has(e.pointerId)) return;
  wPts.set(e.pointerId, {x:e.clientX, y:e.clientY});
  if (wPts.size === 2) {
    /* 双指捏合缩放 */
    const [a,b] = [...wPts.values()];
    const dist = Math.hypot(a.x-b.x, a.y-b.y);
    if (wPinch) {
      const z = $('#zoom');
      wScale = Math.max(+z.min, Math.min(+z.max, wScale * dist / wPinch));
      z.value = wScale;
      wClamp(); wDraw();
    }
    wPinch = dist;
    return;
  }
  if (!wLast) return;
  const r = cv.getBoundingClientRect();
  wOx += (e.clientX - wLast.x) * 320 / r.width;
  wOy += (e.clientY - wLast.y) * 240 / r.height;
  wLast = {x:e.clientX, y:e.clientY};
  wClamp(); wDraw();
});
const wUp = e => { wPts.delete(e.pointerId); if (wPts.size < 2) wPinch = null; if (!wPts.size) wLast = null; };
cv.addEventListener('pointerup', wUp);
cv.addEventListener('pointercancel', wUp);
$('#zoom').oninput = e => {
  if (!wImg) return;
  wScale = parseFloat(e.target.value);
  wClamp(); wDraw();
};
$('#upload').onclick = async () => {
  if (!wImg){ alert('先选一张图片'); return; }
  wClamp(); wDraw();   /* 收紧到合法范围再截取 */
  const d = cv.getContext('2d').getImageData(0,0,320,240).data;
  const buf = new Uint8Array(320*240*2);
  for (let i=0,j=0; i<d.length; i+=4, j+=2){
    const r=d[i], g=d[i+1], b=d[i+2];
    buf[j]   = ((g & 0x1C) << 3) | (b >> 3);      // RGB565 低字节
    buf[j+1] = (r & 0xF8) | (g >> 5);             // RGB565 高字节
  }
  alert(await post('/api/wallpaper', buf, 'application/octet-stream'));
  state();
};
$('#delwp').onclick = async () => { alert(await post('/api/wallpaper/delete')); state(); };
state();
</script></body></html>)HTML";

// ---------------------------------------------------------------- 处理函数
esp_err_t HandlePage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* 2026-11-06 用户报「下拉/4:3 裁剪没生效」：浏览器一直在用缓存的旧页面。
       页面每次固件升级都会变，必须禁缓存。 */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kPage, HTTPD_RESP_USE_STRLEN);
}

std::string QueryValue(const std::string& query, const char* key) {
    // 简单解析 ?a=1&b=2，值做 URL 解码（只需要处理 %23 这类）
    // 注意要先跳过 "?" 前面的路径，否则第一段会带上 "/api/xxx?key"
    size_t pos = query.find('?');
    if (pos == std::string::npos) {
        return "";
    }
    pos += 1;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) {
            amp = query.size();
        }
        std::string kv = query.substr(pos, amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) {
            std::string v = kv.substr(eq + 1);
            std::string out;
            for (size_t i = 0; i < v.size(); i++) {
                if (v[i] == '%' && i + 2 < v.size()) {
                    out.push_back(static_cast<char>(std::strtol(v.substr(i + 1, 2).c_str(), nullptr, 16)));
                    i += 2;
                } else {
                    out.push_back(v[i]);
                }
            }
            return out;
        }
        pos = amp + 1;
    }
    return "";
}

/* 前置声明：HandleState 要用，实现都在本文件后面 */
void JsonEscape(const char* in, char* out, size_t out_size);
std::string GetLocationText();

esp_err_t HandleState(httpd_req_t* req) {
    UiConfig c = GetConfig();
    /* ── 视图/样式字段（2026-11-06 网页「屏幕视图」卡）── */
    auto* disp = DafeiyuDisplay::Instance();
    const int view = disp ? disp->WebCurrentView() : 0;
    const char* view_s = (view == 100) ? "clock" : ((view == 101) ? "wallpaper" : "theme");
    const int style = (view == 100 && disp) ? disp->WebClockStyle(-1) : view;
    char names[224] = "[]";
    if (disp != nullptr) {
        size_t o = (size_t)snprintf(names, sizeof(names), "[");
        for (int i = 0; i < (int)PET_THEME_N && o < sizeof(names) - 24; i++) {
            o += (size_t)snprintf(names + o, sizeof(names) - o,
                                  "%s\"%s\"", (i ? "," : ""), disp->WebThemeName(i));
        }
        snprintf(names + o, sizeof(names) - o, "]");
    }
    // buf 要留够：加了 reset（中文）和 notify_text 之后 384 早就不够了，
    // 2026-11-03 又加了 loc（用户自由输入，要留转义余量）和 led
    char buf[1024];
    char loc_json[160];
    JsonEscape(GetLocationText().c_str(), loc_json, sizeof(loc_json));
    std::string led_json = "null";
    if (Ws2812Instance() != nullptr) {
        led_json = Ws2812Instance()->GetStateJson();
    }
    int n = snprintf(buf, sizeof(buf),
                     "{\"hunger\":%u,\"mood\":%u,\"bond\":%u,\"level\":%u,\"exp\":%u,"
                     "\"bg\":\"#%06X\",\"clock\":%d,\"wallpaper\":%d,\"wallpaper_on\":%d,"
                     "\"layout\":\"%s\",\"wake_style\":\"%s\",\"reset\":\"%s\",\"notify\":%u,\"notify_text\":\"%s\","
                     "\"auto\":%d,\"panic\":\"%s\",\"wake\":\"%s\",\"loc\":\"%s\",\"led\":%s,"
                     "\"lcd_invert\":%d,\"lcd_rgb_swap\":%d,"
                     "\"spectrum\":%d,\"subtitle\":%d,\"dance\":%d,"
                     "\"view\":\"%s\",\"style\":%d,\"theme_names\":%s}",
                     /* 养成玩法（PetGame/PetState）已随 dafeiyu_pet 删除。
                        按用户要求保留 /api/state 接口和控制页功能，
                        这几个养成字段改为返回固定值。 */
                     100u, 100u, 100u, 1u, 0u,
                     (unsigned)(c.bg_color & 0xFFFFFF), c.clock_enabled ? 1 : 0,
                     HasWallpaper() ? 1 : 0, c.wallpaper_enabled ? 1 : 0,
                     c.layout == Layout::kInfo ? "info" : "desktop",
                     c.wake_style == 1 ? "eyes" : (c.wake_style == 2 ? "prim" : "orb"),
                     DafeiyuDisplay::LastResetReason(),
                     (unsigned)DafeiyuDisplay::NotifyCount(),
                     DafeiyuDisplay::LastNotifyText(),
                     DafeiyuDisplay::AutoEmoteEnabled() ? 1 : 0,
                     DafeiyuDisplay::LastPanicStep(),
                     DafeiyuDisplay::WakeDebug(),
                     loc_json,
                     led_json.c_str(),
                     DafeiyuGetLcdInvert(),
                     DafeiyuGetLcdRgbSwap(),
                     c.spectrum_enabled ? 1 : 0,
                     c.subtitle_enabled ? 1 : 0,
                     c.led_dance_enabled ? 1 : 0,
                     view_s, style, names);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* 网页切换屏幕视图（2026-11-06 用户要求）：
   /api/view                     查询当前 {view, style}
   /api/view?target=theme        回大肥鱼待机
   /api/view?target=theme&style=n  切到第 n 套肥鱼主题
   /api/view?target=clock        进翻页时钟
   /api/view?target=clock&style=n  进时钟并切到第 n 套样式（已在时钟下只切样式）
   /api/view?target=wallpaper    进自定义壁纸 */
esp_err_t HandleView(httpd_req_t* req) {
    auto* disp = DafeiyuDisplay::Instance();
    if (disp == nullptr) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "display not ready");
    }
    const std::string q = req->uri;
    const std::string target = QueryValue(q, "target");
    const std::string style_s = QueryValue(q, "style");
    if (target.empty()) {
        const int v = disp->WebCurrentView();
        char b[64];
        const int n = snprintf(b, sizeof(b), "{\"view\":\"%s\",\"style\":%d}",
                               (v == 100) ? "clock" : ((v == 101) ? "wallpaper" : "theme"),
                               (v == 100) ? disp->WebClockStyle(-1) : v);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, b, n);
    }
    bool ok = false;
    if (target == "theme") {
        ok = disp->WebSetView(style_s.empty() ? 0 : atoi(style_s.c_str()));
    } else if (target == "clock") {
        ok = disp->WebSetView(100);
        if (ok && !style_s.empty()) {
            disp->WebClockStyle(atoi(style_s.c_str()));
        }
    } else if (target == "wallpaper") {
        ok = disp->WebSetView(101);
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, ok ? "ok" : "fail");
}

/** 用户输入的文本进 JSON 前转义（位置是自由输入，可能带引号/反斜杠） */
void JsonEscape(const char* in, char* out, size_t out_size) {
    size_t o = 0;
    for (const char* p = in; *p != '\0' && o + 7 < out_size; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '"' || ch == '\\') {
            out[o++] = '\\';
            out[o++] = (char)ch;
        } else if (ch < 0x20) {
            o += (size_t)snprintf(out + o, out_size - o, "\\u%04x", ch);
        } else {
            out[o++] = (char)ch;
        }
    }
    out[o] = '\0';
}

/* ── 位置（2026-11-03）───────────────────────────────────────────────────
   设备没有 GPS，定位只有两条路：
     1. 网页手动填（最准，用户自己知道在哪）
     2. 公网 IP 反查（ip-api.com 免费接口，城市级精度，无需 key）
   存在 NVS "location"/"text"，语音侧由 MCP 工具 self.location.get 读给大模型。
   ⚠️ IP 定位只在用户点按钮时才查：免费接口限 45 次/分钟，不能开机就查。 */
std::string GetLocationText() {
    Settings s("location", false);
    return s.GetString("text", "");
}

std::string LocateByIp() {
    std::string out;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http->Open("GET", "http://ip-api.com/json/?lang=zh-CN&fields=status,message,regionName,city")) {
        ESP_LOGE(TAG, "定位: 连接 ip-api.com 失败");
        return out;
    }
    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "定位: HTTP %d", http->GetStatusCode());
        http->Close();
        return out;
    }
    std::string body = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "定位返回: %s", body.c_str());

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return out;
    }
    cJSON* st = cJSON_GetObjectItem(root, "status");
    if (cJSON_IsString(st) && strcmp(st->valuestring, "success") == 0) {
        cJSON* city = cJSON_GetObjectItem(root, "city");
        cJSON* region = cJSON_GetObjectItem(root, "regionName");
        std::string c = cJSON_IsString(city) ? city->valuestring : "";
        std::string r = cJSON_IsString(region) ? region->valuestring : "";
        if (!c.empty()) {
            out = c;
        }
        if (!r.empty() && r != c) {
            out = out.empty() ? r : (r + " " + out);
        }
    }
    cJSON_Delete(root);
    return out;
}

esp_err_t HandleLocationGet(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    std::string loc = GetLocationText();
    return httpd_resp_sendstr(req, loc.empty() ? "未设置" : loc.c_str());
}

esp_err_t HandleLocationSet(httpd_req_t* req) {
    // POST /api/location?set=广东省深圳市   手动保存
    // POST /api/location?auto=1             用公网 IP 自动定位并保存
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    std::string set = QueryValue(req->uri, "set");
    std::string automatic = QueryValue(req->uri, "auto");
    if (!automatic.empty() && automatic != "0") {
        std::string loc = LocateByIp();
        if (loc.empty()) {
            return httpd_resp_sendstr(req, "自动定位失败（网络不通或接口限流，稍后再试）");
        }
        Settings s("location", true);
        s.SetString("text", loc);
        ESP_LOGI(TAG, "IP 定位已保存: %s", loc.c_str());
        return httpd_resp_sendstr(req, loc.c_str());
    }
    if (!set.empty()) {
        Settings s("location", true);
        s.SetString("text", set);
        ESP_LOGI(TAG, "位置已保存: %s", set.c_str());
        return httpd_resp_sendstr(req, "ok");
    }
    return httpd_resp_sendstr(req, "用法：POST /api/location?set=城市名  或  ?auto=1");
}

/* ── 板载彩灯（2026-11-03）────────────────────────────────────────────── */
esp_err_t HandleLedGet(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    auto* led = Ws2812Instance();
    if (led == nullptr) {
        return httpd_resp_sendstr(req, "null");
    }
    return httpd_resp_sendstr(req, led->GetStateJson().c_str());
}

esp_err_t HandleLedSet(httpd_req_t* req) {
    // POST /api/led?color=%23ff0000&b=50&effect=breathe（color 里 # 要转成 %23）
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    auto* led = Ws2812Instance();
    if (led == nullptr) {
        return httpd_resp_sendstr(req, "彩灯未初始化");
    }
    std::string color = QueryValue(req->uri, "color");
    std::string b = QueryValue(req->uri, "b");
    std::string effect = QueryValue(req->uri, "effect");
    if (color.empty() && b.empty() && effect.empty()) {
        return httpd_resp_sendstr(req, "用法：POST /api/led?color=%23ff0000&b=50&effect=breathe");
    }
    int level = b.empty() ? -1 : atoi(b.c_str());
    if (!led->Set(color, level, effect.empty() ? std::string("on") : effect)) {
        return httpd_resp_sendstr(req, "不认识的颜色或效果");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, led->GetStateJson().c_str());
}

/* ── 唤醒词（2026-11-03）────────────────────────────────────────────────
   模型在 assets 分区的 srmodels.bin 里，**不在固件里**，所以换词不用重烧固件；
   但 AFE 初始化后不能再换模型（AudioService 会拒绝），因此这里是「存设置 + 重启生效」。
   两种模式：
     · mode=wn9_xxx  内置词（WakeNet），镜像里打包了几个就能选几个
     · mode=custom   自定义词（MultiNet），词条是拼音，如 "xiao tu dou" */
esp_err_t HandleWakeWordGet(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    char esc[256];

    /* 没选过时回「实际在用的那个」= 出厂默认词，否则下拉框显示的和引擎不一致 */
    std::string mode = wakeword::GetMode();
    if (mode.empty()) {
        mode = wakeword::FactoryDefaultWakenet();
        if (mode.empty() && !wakeword::AvailableWakenets().empty()) {
            mode = wakeword::AvailableWakenets().front();
        }
    }
    std::string out = "{";
    JsonEscape(mode.c_str(), esc, sizeof(esc));
    out += "\"mode\":\"" + std::string(esc) + "\"";

    /* 没设过自定义词就回镜像里的默认值（打包脚本写的 xiao tu dou / 小土豆） */
    std::string cmd = wakeword::GetCustomText();
    std::string disp = wakeword::GetCustomDisplay();
    if (cmd.empty() || !wakeword::IsValidCommand(cmd)) {
        cmd = "xiao tu dou";   // 存的不是拼音（比如误填了中文）就别回显，免得再存一次
    }
    if (disp.empty()) {
        disp = "小土豆";
    }
    JsonEscape(cmd.c_str(), esc, sizeof(esc));
    out += ",\"cmd\":\"" + std::string(esc) + "\"";
    JsonEscape(disp.c_str(), esc, sizeof(esc));
    out += ",\"display\":\"" + std::string(esc) + "\"";
    out += ",\"threshold\":" + std::to_string(wakeword::GetThreshold());
    out += ",\"multinet\":";
    out += wakeword::HasMultinet() ? "true" : "false";

    out += ",\"models\":[";
    bool first = true;
    for (const auto& id : wakeword::AvailableWakenets()) {
        if (!first) {
            out += ",";
        }
        first = false;
        char id_esc[80];
        char name_esc[160];
        JsonEscape(id.c_str(), id_esc, sizeof(id_esc));
        JsonEscape(wakeword::DisplayName(id).c_str(), name_esc, sizeof(name_esc));
        out += std::string("{\"id\":\"") + id_esc + "\",\"name\":\"" + name_esc + "\"}";
    }
    out += "]}";
    return httpd_resp_sendstr(req, out.c_str());
}

esp_err_t HandleWakeWordSet(httpd_req_t* req) {
    // POST /api/wakeword?mode=wn9_hiesp
    // POST /api/wakeword?mode=custom&cmd=xiao%20tu%20dou&display=小土豆&th=20
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    std::string mode = QueryValue(req->uri, "mode");
    std::string cmd = QueryValue(req->uri, "cmd");
    std::string disp = QueryValue(req->uri, "display");
    std::string th = QueryValue(req->uri, "th");
    if (mode.empty() && cmd.empty() && disp.empty() && th.empty()) {
        return httpd_resp_sendstr(
            req, "用法：?mode=wn9_hiesp 或 ?mode=custom&cmd=xiao tu dou&display=小土豆&th=20");
    }
    if (!cmd.empty() && !wakeword::IsValidCommand(cmd)) {
        // 中文命令词会让 esp-sr 的 phrase_init_from_phoneme_cn 空指针崩溃（实测无限重启）
        return httpd_resp_sendstr(
            req, "唤醒词只能填拼音（字母+空格），例如 xiao tu dou；中文请填到「中文」那一栏");
    }
    if (!mode.empty()) {
        wakeword::SetMode(mode);
    }
    if (!cmd.empty()) {
        wakeword::SetCustomText(cmd);
    }
    if (!disp.empty()) {
        wakeword::SetCustomDisplay(disp);
    }
    if (!th.empty()) {
        wakeword::SetThreshold(atoi(th.c_str()));
    }
    ESP_LOGI(TAG, "唤醒词设置: mode=%s cmd=%s th=%s", mode.c_str(), cmd.c_str(), th.c_str());
    return httpd_resp_sendstr(req, "已保存，重启后生效");
}

/* 换主题（调试/网页用）：/api/theme?set=8
   只置请求标志，真正的切主题在 LVGL tick 里做（和双击音量键同一路径）。 */
esp_err_t HandleThemeSet(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    const std::string v = QueryValue(req->uri, "set");
    if (v.empty()) {
        return httpd_resp_sendstr(req, "用法：/api/theme?set=0~N");
    }
    DafeiyuDisplay* disp = DafeiyuDisplay::Instance();
    if (disp == nullptr) {
        return httpd_resp_sendstr(req, "display 还没初始化");
    }
    const int id = atoi(v.c_str());
    disp->SetThemeById(id);
    char buf[64];
    snprintf(buf, sizeof(buf), "已请求主题 %d", id);
    return httpd_resp_sendstr(req, buf);
}

/* 进游戏机（2026-11-04 双固件）：把启动槽切到 ota_1（GameBox-NES）并重启。
   otadata 只决定「下一次」从哪个槽启动，GameBox 里切回时再写回 ota_0。
   WiFi / NVS / assets 都不碰，设置不会丢。 */
esp_err_t HandleRebootToGame(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    const esp_partition_t* game =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    if (game == nullptr) {
        return httpd_resp_sendstr(req, "找不到 ota_1 分区（分区表没升级？）");
    }
    if (game->size < 0x8000) {
        return httpd_resp_sendstr(req, "ota_1 槽太小，不是游戏固件槽");
    }
    const esp_partition_t* cur = esp_ota_get_boot_partition();
    esp_err_t err = esp_ota_set_boot_partition(game);
    if (err != ESP_OK) {
        return httpd_resp_sendstr(req, "设置启动槽失败");
    }
    ESP_LOGI(TAG, "重启进游戏机: ota_1 @0x%lx (%u KB)，当前槽 %s",
             (unsigned long)game->address, (unsigned)(game->size / 1024),
             cur ? cur->label : "?");
    httpd_resp_sendstr(req, "已切换，设备正在重启进游戏机…");
    vTaskDelay(pdMS_TO_TICKS(1000));   // 给 HTTP 响应留时间发出去
    esp_restart();
    return ESP_OK;   // 不会执行到
}

/** 读 POST 正文（UTF-8，喂食名 / RGB565 二进制都走这里） */
bool ReadBody(httpd_req_t* req, std::string* out) {
    if (req->content_len <= 0) {
        return false;
    }
    out->resize(req->content_len);
    size_t got = 0;
    while (got < out->size()) {
        int r = httpd_req_recv(req, &(*out)[got], out->size() - got);
        if (r <= 0) {
            return false;
        }
        got += r;
    }
    return true;
}

/** 动作自动变换开关：GET /api/auto?on=1 开，?on=0 关；不带参数则只查询不改 */
esp_err_t HandleAutoEmote(httpd_req_t* req) {
    char q[64] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q) - 1);

    bool on = DafeiyuDisplay::AutoEmoteEnabled();   // 默认保持原值
    const char* p = strstr(q, "on=");
    if (p != nullptr) {
        on = (p[3] == '1' || p[3] == 't' || p[3] == 'T' || p[3] == 'y' || p[3] == 'Y');
        DafeiyuDisplay::SetAutoEmoteEnabled(on);
        ESP_LOGI(TAG, "auto emote -> %d", on ? 1 : 0);
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, on ? "auto=1" : "auto=0");
}

esp_err_t HandleConfig(httpd_req_t* req) {
    UiConfig c = GetConfig();
    std::string q = req->uri;
    std::string bg = QueryValue(q, "bg");          // #191f2e
    std::string clock = QueryValue(q, "clock");
    std::string wp = QueryValue(q, "wallpaper");
    std::string layout = QueryValue(q, "layout");   // desktop | info
    std::string wake_style = QueryValue(q, "wake_style");   // orb | eyes | prim
    if (!bg.empty()) {
        c.bg_color = static_cast<uint32_t>(std::strtoul(bg.c_str() + (bg[0] == '#' ? 1 : 0), nullptr, 16));
    }
    if (!clock.empty()) {
        c.clock_enabled = (clock != "0");
    }
    if (!wp.empty()) {
        c.wallpaper_enabled = (wp != "0");
    }
    if (!layout.empty()) {
        c.layout = (layout == "info" || layout == "1") ? Layout::kInfo : Layout::kDesktop;
    }
    if (!wake_style.empty()) {
        if (wake_style == "eyes" || wake_style == "1") {
            c.wake_style = 1;
        } else if (wake_style == "prim" || wake_style == "2") {
            c.wake_style = 2;
        } else {
            c.wake_style = 0;
        }
    }
    /* 2026-11-04 LCD 颜色模式：反色 / 红蓝互换，实刷+持久化。
       留空表示不改。 */
    std::string invert = QueryValue(q, "invert");
    std::string rgb_swap = QueryValue(q, "rgb_swap");
    if (!invert.empty() || !rgb_swap.empty()) {
        int inv = invert.empty() ? DafeiyuGetLcdInvert() : (invert != "0" ? 1 : 0);
        int swp = rgb_swap.empty() ? DafeiyuGetLcdRgbSwap() : (rgb_swap != "0" ? 1 : 0);
        DafeiyuSetLcdColorMode(inv, swp);
    }
    /* 2026-11-05 音乐可视化三开关：spec=频谱条 sub=TTS字幕 dance=彩灯律动。
       dance 直接联动彩灯（同 lcd 颜色开关的即时生效做法），存档随 SaveConfig。 */
    std::string spec = QueryValue(q, "spec");
    std::string sub = QueryValue(q, "sub");
    std::string dance = QueryValue(q, "dance");
    if (!spec.empty()) {
        c.spectrum_enabled = (spec != "0");
    }
    if (!sub.empty()) {
        c.subtitle_enabled = (sub != "0");
    }
    if (!dance.empty()) {
        c.led_dance_enabled = (dance != "0");
        if (Ws2812Instance() != nullptr) {
            Ws2812Instance()->SetDanceMode(c.led_dance_enabled);
        }
    }
    SaveConfig(c);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t HandleWallpaperUpload(httpd_req_t* req) {
    std::string body;
    if (!ReadBody(req, &body) || body.size() != kWallpaperBytes) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "需要 320x240 RGB565，153600 字节");
    }
    if (!SaveWallpaper(reinterpret_cast<const uint8_t*>(body.data()), body.size())) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "写入失败");
    }
    SaveConfig(GetConfig());  // 版本号 +1，让显示层立刻重读
    /* 2026-11-06 补一刀：config 链路在 wallpaper_enabled 关着时不会加载
       壁纸 —— 直接热刷新，保证传完立刻能看见（壁纸主题下尤其需要）。 */
    if (DafeiyuDisplay::Instance() != nullptr) {
        DafeiyuDisplay::Instance()->RefreshWallpaper();
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "壁纸已更新");
}

esp_err_t HandleWallpaperDelete(httpd_req_t* req) {
    DeleteWallpaper();
    SaveConfig(GetConfig());
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "已删除");
}

esp_err_t HandleWallpaperGet(httpd_req_t* req) {
    static uint8_t* cache = nullptr;
    if (cache == nullptr) {
        cache = static_cast<uint8_t*>(heap_caps_malloc(kWallpaperBytes, MALLOC_CAP_SPIRAM));
    }
    if (cache == nullptr || !LoadWallpaper(cache, kWallpaperBytes)) {
        return httpd_resp_send_404(req);
    }
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, reinterpret_cast<const char*>(cache), kWallpaperBytes);
}

// 验收用：跳过待机随机触发，直接点播某个动作片段，肉眼确认每个 Sho 到底长什么样。
// 注意：非一次性片段（idle/wait/speak/think）播了会停在那儿，要点别的动作才回来。
esp_err_t HandleAct(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    DafeiyuDisplay* disp = DafeiyuDisplay::Instance();
    if (disp == nullptr) {
        return httpd_resp_sendstr(req, "display 还没初始化");
    }
    const std::string name = QueryValue(req->uri, "name");   // /api/act?name=dance
    char buf[320];
    if (name.empty()) {
        snprintf(buf, sizeof(buf), "用法：/api/act?name=xxx\n可选动作：%s",
                 DafeiyuDisplay::ClipListForDebug());
        return httpd_resp_sendstr(req, buf);
    }
    if (!disp->PlayClipByName(name.c_str())) {
        snprintf(buf, sizeof(buf), "不认识「%s」\n可选动作：%s", name.c_str(),
                 DafeiyuDisplay::ClipListForDebug());
        return httpd_resp_sendstr(req, buf);
    }
    snprintf(buf, sizeof(buf), "ok: %s", name.c_str());
    return httpd_resp_sendstr(req, buf);
}

}  // namespace

/* 控制页的 httpd 句柄。为什么要有全局句柄（2026-11-03）：
   配网模式下官方配网页先占了 80 端口，我们 12 秒后去起必然失败；等配网成功、
   官方 httpd_stop 让出端口后，得能再起一次，也得能在下次进配网时让出去。 */
static httpd_handle_t s_server = nullptr;

bool WebServerRunning() {
    return s_server != nullptr;
}

void StopWebServer() {
    if (s_server == nullptr) {
        return;
    }
    httpd_stop(s_server);
    s_server = nullptr;
    ESP_LOGI(TAG, "控制页已停止（让出 80 端口给官方配网页）");
}

bool StartWebServer() {
    if (s_server != nullptr) {
        return true;   // 已经在跑，重试调用直接返回，别重复占用端口
    }
    LoadConfig();
    if (!MountStorage()) {
        ESP_LOGW(TAG, "storage unavailable, wallpaper disabled");
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    // 内存这台机子上是硬约束（实测内部 SRAM 一度只剩 127 字节，httpd 直接
    // select() error 12 = ENOMEM 并在日志里刷 "Closing invalid socket"）。
    // 默认 max_open_sockets=7，每个 socket 都要吃 LWIP 的内部分配 —— 而这
    // 台设备就一个人偶尔开个网页，2 个够用了。省下来的给 TLS/MQTT 用。
    // 砍到 2 试过：内存是省了一点，但 keep-alive 会占住不放，槽位耗尽后
    // 后续请求全部挂起（表现就是网页轮询时好时坏）。4 个 + 2 秒保活够用。
    config.max_open_sockets = 4;
    config.stack_size = 6144;
    config.max_uri_handlers = 19;
    config.lru_purge_enable = true;  // 没槽位时回收最久没用的连接
    config.keep_alive_enable = true;
    config.keep_alive_idle = 2;
    /* ⚠️ 2026-11-06 修复「壁纸要传好几次才成功」：壁纸 150KB，手机 WiFi
       慢时 5 秒收不完 → recv 超时 ReadBody 失败 → 用户重传。放宽到 30 秒。 */
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        // 常见原因：还在配网模式，80 端口被官方配网页占着。上层会定时重试。
        ESP_LOGW(TAG, "http server start failed（80 端口被占用？配网模式下正常，稍后重试）");
        return false;
    }

    const httpd_uri_t routes[] = {
        {"/", HTTP_GET, HandlePage, nullptr},
        {"/api/state", HTTP_GET, HandleState, nullptr},
        {"/api/view", HTTP_GET, HandleView, nullptr},
        {"/api/act", HTTP_GET, HandleAct, nullptr},
        {"/api/auto", HTTP_GET, HandleAutoEmote, nullptr},
        {"/api/config", HTTP_POST, HandleConfig, nullptr},
        {"/api/wallpaper", HTTP_POST, HandleWallpaperUpload, nullptr},
        {"/api/wallpaper", HTTP_GET, HandleWallpaperGet, nullptr},
        {"/api/wallpaper/delete", HTTP_POST, HandleWallpaperDelete, nullptr},
        {"/api/location", HTTP_GET, HandleLocationGet, nullptr},
        {"/api/location", HTTP_POST, HandleLocationSet, nullptr},
        {"/api/led", HTTP_GET, HandleLedGet, nullptr},
        {"/api/led", HTTP_POST, HandleLedSet, nullptr},
        {"/api/wakeword", HTTP_GET, HandleWakeWordGet, nullptr},
        {"/api/wakeword", HTTP_POST, HandleWakeWordSet, nullptr},
        {"/api/theme", HTTP_GET, HandleThemeSet, nullptr},
        {"/api/gameboot", HTTP_POST, HandleRebootToGame, nullptr},
    };
    for (const auto& r : routes) {
        httpd_register_uri_handler(server, &r);
    }
    s_server = server;

    // 把可直接访问的地址打进日志，省得到处找 IP
    esp_netif_t* sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {};
    if (sta_netif != nullptr && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0) {
        ESP_LOGI(TAG, "控制页已就绪 -> http://%d.%d.%d.%d/",
                 (int)(ip_info.ip.addr & 0xff), (int)((ip_info.ip.addr >> 8) & 0xff),
                 (int)((ip_info.ip.addr >> 16) & 0xff), (int)((ip_info.ip.addr >> 24) & 0xff));
    } else {
        ESP_LOGI(TAG, "控制页已就绪（还没拿到 IP，联网后访问 http://<设备IP>/ 或 http://Xiaozhi-xxxx.local/）");
    }
    return true;
}

}  // namespace dafeiyu
