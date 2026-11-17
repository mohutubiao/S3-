#include "dafeiyu_web.h"

#include "cam_stream.h"      // 81 端口图传（MJPEG）：网页点「图传」时才启动
#include "application.h"     // 图传模式要停小智：SetDeviceState / AudioService
#include "audio/audio_service.h"   // EnableWakeWordDetection / EnableVoiceProcessing
#include "dafeiyu_display.h"
#include "pet_themes.h"      // PET_THEME_N / pet_theme_name：/api/state 的主题名列表
#include "dafeiyu_storage.h"
#include "ws2812_led.h"     // 板载彩灯：/api/led
#include "esp_now_link.h"   // 小车模式遥控：/api/car（ESP-NOW 发给 C3 从机）
#include "wake_word_models.h"   // 唤醒词选择：/api/wakeword
#include "board.h"          // IP 自动定位要出网（GetNetwork()->CreateHttp）
#include "settings.h"       // 位置存 NVS（namespace "location"）
#include "wifi_manager.h"   // 2026-11-13：80 被配网 AP 占用时停配置 AP 再重试

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

/* 2026-11-10 用户定案：LCD 颜色参数在板级修死（invert=1 + BGR=1，POR 实测），
   不再提供网页开关 —— 网页调出来的错值存进 NVS 曾反复造成红蓝反色。 */

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

/* ── 2026-11-15 待机省电 ────────────────────────────────────────────────
   设备 idle 超过 N 分钟就把屏幕亮度降到最暗（执行在板级定时器里）。
   存独立 NVS 命名空间，**故意不放进 UiConfig blob**：加字段会改变 blob
   长度，老用户的背景色/壁纸/字幕开关会被整体判废回默认值。
   0 = 关闭该功能。 */
namespace {
constexpr const char* kStandbyNs = "screen";
int g_standby_min = -1;   // -1 = 还没从 NVS 读过
}

int StandbyMinutes() {
    if (g_standby_min < 0) {
        Settings s(kStandbyNs, false);
        g_standby_min = s.GetInt("standby_min", 3);
        if (g_standby_min < 0) g_standby_min = 0;
        if (g_standby_min > 120) g_standby_min = 120;
    }
    return g_standby_min;
}

void SetStandbyMinutes(int minutes) {
    if (minutes < 0) minutes = 0;
    if (minutes > 120) minutes = 120;
    g_standby_min = minutes;
    Settings s(kStandbyNs, true);
    s.SetInt("standby_min", minutes);
    /* ⚠️ SetInt 只标脏，必须 Commit，否则重启后设置丢失 */
    s.Commit();
    ESP_LOGI(TAG, "待机省电：%d 分钟（0=关闭）", minutes);
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
/* 2026-11-14：「已是开机默认」时保存按钮置灰，别让人以为还能点 */
button:disabled{background:#2a3348;color:#6b7488;cursor:default}
.chips{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px}
input[type=text],input[type=number]{background:#11141d;color:#e8ecf5;border:1px solid #2a3348;border-radius:8px;padding:9px 10px}
input[type=color]{width:46px;height:34px;border:0;background:none;padding:0}
.hint{color:#8a93a8;font-size:12px;margin-top:6px}
#preview{width:100%;max-width:320px;border-radius:8px;display:none;margin-top:10px}
.tag{display:inline-block;background:#2a3348;border-radius:6px;padding:2px 8px;font-size:12px;color:#8a93a8}
/* ── 2026-11-11 大按钮 + 分页 ── */
.nav{display:flex;gap:8px;margin:12px 0}
.nav button{flex:1;background:#232a3a;padding:12px 6px;font-size:14px}
.nav button.on{background:#3d5afe}
.page{display:none}
.page.on{display:block}
.biggrid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin:12px 0}
.biggrid a,.biggrid button{display:flex;flex-direction:column;align-items:center;justify-content:center;
  gap:6px;background:#1b1f2b;border:1px solid #2a3348;border-radius:16px;padding:26px 10px;
  color:#e8ecf5;text-decoration:none;font-size:15px}
.biggrid .ico{font-size:34px;line-height:1}
.biggrid .sub{font-size:12px;color:#8a93a8}
#pad{position:relative;width:100%;max-width:280px;aspect-ratio:1/1;margin:10px auto;border-radius:50%;
  background:radial-gradient(circle at 50% 50%,#232a3a,#171b26);border:1px solid #2a3348;touch-action:none}
#knob{position:absolute;left:50%;top:50%;width:64px;height:64px;margin:-32px 0 0 -32px;border-radius:50%;
  background:#3d5afe;box-shadow:0 4px 14px rgba(61,90,254,.5)}
.dirpad{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;max-width:280px;margin:0 auto}
.dirpad button{padding:14px 0}
.dirpad .sp{visibility:hidden}
</style></head><body>
<h1>控制台</h1>
<div class="tag" id="meta">读取中…</div>
<div class="chips" style="margin-top:6px">
  <button class="ghost" id="refresh">刷新状态</button>
</div>

<!-- 2026-11-15 顶部只留「主页」：控制/设置入口在主页大按钮里已经有了，
     重复摆一份会让人点错。控制页/设置页靠这个主页键返回。 -->
<div class="nav">
  <button data-page="home" class="on">🏠 主页</button>
</div>

<!-- ── 主页：大按钮 ── -->
<div class="page on" id="pg-home">
  <!-- 2026-11-12 从机连接状态 + 实时距离/实际阈值，直接放主页 -->
  <div class="card" style="margin-bottom:10px">
    <div class="row" style="align-items:center">
      <span id="homeDot" style="font-size:18px;color:#888">●</span>
      <span id="homeLink" style="flex:1;font-weight:700">从机检测中…</span>
    </div>
    <div class="hint" id="homeLive">读取中…</div>
  </div>

  <div class="biggrid">
    <button data-page="ctrl"><span class="ico">🎮</span>遥控控制<span class="sub">摇杆 / 方向键</span></button>
    <button id="homeCam"><span class="ico">📷</span>图传遥控<span class="sub">视频 + 摇杆</span></button>
    <button data-page="setup"><span class="ico">⚙️</span>设置<span class="sub">屏幕 / 灯 / 唤醒词</span></button>
    <button id="homeExitCar"><span class="ico">🚪</span>退出小车模式<span class="sub">回正常模式</span></button>
  </div>
  <div class="hint">图传页需要在设备上进入小车模式后使用；摇杆直接走 ESP-NOW 发给 C3 从机。</div>
</div>

<!-- ── 控制页：摇杆 + 方向键 + 电机设置 ── -->
<div class="page" id="pg-ctrl">
  <div class="card">
    <b>摇杆</b>
    <div id="pad"><div id="knob"></div></div>
    <div class="row"><span class="lbl">steer</span><span class="val" id="jS">0</span>
      <span class="lbl">thr</span><span class="val" id="jT">0</span></div>
    <div class="hint">按住拖动即走，松手立即停车（40ms 一帧，尾部补发 0）。</div>
  </div>
  <div class="card">
    <b>方向键</b>
    <div class="dirpad">
      <button class="sp"></button><button data-act="forward">▲</button><button class="sp"></button>
      <button data-act="left">◀</button><button data-act="stop" style="background:#b3261e">■</button><button data-act="right">▶</button>
      <button class="sp"></button><button data-act="backward">▼</button><button class="sp"></button>
    </div>
  </div>
  <div class="card">
    <b>自主探索</b>
    <div class="row"><span class="lbl">时长</span>
      <select id="expdur" style="flex:1;background:#11141d;color:#e8ecf5;border:1px solid #2a3348;border-radius:8px;padding:8px 10px">
        <option value="60">1 分钟</option>
        <option value="180" selected>3 分钟</option>
        <option value="300">5 分钟</option>
        <option value="600">10 分钟</option>
      </select></div>
    <div class="chips" style="margin-top:8px">
      <button id="expstart">开始探索</button>
      <button class="ghost" id="expstop">停止</button>
    </div>
    <div class="hint" id="expmsg">遇障/临边自动换方向继续走，不会停。速度用下面「电机设置」的全局滑块。
    探索中按住从机 BOOT 键可立即打断。</div>
  </div>
  <div class="card">
    <b>电机设置（本机网页）</b>
    <div class="row"><span class="lbl">速度</span>
      <input type="range" id="spd" min="30" max="100" step="5" value="80" style="flex:1">
      <span class="val" id="spdV">80%</span></div>
    <div class="row"><label style="flex:1"><input type="checkbox" id="swp"> 左右轮交换（swap_lr）</label></div>
    <div class="hint">速度为全局缩放；交换用于接线反了的情况（等价于 steer 取反）。
    单轮反相（inv_l / inv_r）<b>没法在主机做</b> —— 从机的差速混控里 steer 只走一次 expo 曲线，
    用 steer 去扛单轮反相会让速度失真，必须改从机固件。存在浏览器里，换手机/清缓存会丢。</div>
  </div>
</div>

<!-- ── 设置页：原有全部卡片 ── -->
<div class="page" id="pg-setup">

<div class="card">
  <b>语音打断</b>
  <div class="row"><label style="flex:1"><input type="checkbox" id="vi"> 允许说话打断 AI</label></div>
  <div class="hint">关闭后：AI 正在说话时喊唤醒词不会停，等它说完再响应。
  也可以直接说「关闭语音打断」/「打开语音打断」。<br>
  <b>这个开关同时控制设备端 AEC（回声消除）</b>：打开 = AEC 开，关掉 = AEC 一起关。
  打断必须靠 AEC 压掉喇叭回声，AEC 关着时喊了没反应。双击 BOOT 也是切这同一个开关。</div>
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
  <div class="hint" id="viewNow" style="color:#9fb0cc">当前：—　开机默认：—</div>
  <div class="chips" style="margin-top:8px">
    <button id="vSave">保存为开机默认</button>
  </div>
  <div class="hint">上面点按钮/下拉只是<b>预览</b>：立刻上屏，但重启后会恢复成
  「开机默认」。满意后点<b>保存为开机默认</b>才会真正记住。
  屏幕上双击音量键、长按 BOOT 菜单切主题仍然是即切即存，不受这里影响。</div>
</div>

<div class="card">
  <b>待机省电</b>
  <div class="row"><span class="lbl">待机</span>
    <input type="number" id="standby" min="0" max="120" step="1" value="3" style="width:90px">
    <span style="color:#8a93a8">分钟</span>
    <button id="standbySet">保存</button></div>
  <div class="hint" id="standbyMsg">待机（不在对话）超过这么多分钟，屏幕自动降到最暗省电，
  一唤醒或按任意键立刻恢复。填 <b>0</b> = 关闭。默认 3 分钟，改完立即生效、不用重启。</div>
</div>

<div class="card">
  <b>屏幕方向（2026-11-13）</b>
  <div class="chips">
    <button id="lcdLand">横屏 320×240</button>
    <button class="ghost" id="lcdPort">竖屏 240×320</button>
  </div>
  <div class="hint" id="lcdMsg">主题模式下可自由切换。切换会重启设备（两套分辨率必须重建
  LVGL），几秒后本页自动回来。小车模式固定竖屏，想切先退出小车模式。</div>
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
  <b>距离保护（GY-530）</b>
  <div class="row"><label style="flex:1"><input type="checkbox" id="disten"> 开启防碰撞 / 防跌落</label></div>
  <div class="row"><span class="lbl">碰撞距离</span>
    <input type="number" id="distobs" min="3" max="200" step="1" value="6" style="width:80px">
    <span class="lbl">厘米（小于等于就停车）</span></div>
  <div class="row"><span class="lbl">跌落距离</span>
    <input type="number" id="distfall" min="8" max="800" step="1" value="45" style="width:80px">
    <span class="lbl">厘米（大于等于就停车）</span></div>
  <div class="chips"><button id="distset">保存并下发</button></div>
  <div class="hint" id="distlive" style="color:#8ad;font-weight:600">读取中…</div>
  <div class="hint" id="distmsg">激光测距装在从机上（SCL=GPIO2 / SDA=GPIO10），
  保存后通过 ESP-NOW 立即下发，并存在从机 NVS 里，断电不丢。</div>
</div>

<div class="card">
  <b>小车模式</b>
  <div class="chips"><button id="carexit">退出小车模式</button></div>
  <div class="hint">小车模式（竖屏大眼睛界面）出问题无法语音退出时的保底通道：
  点击后重启回正常模式。切换需几秒，属正常现象。</div>
</div>

<div class="card">
  <b>音乐可视化（2026-11-05）</b>
  <div class="row"><label style="flex:1"><input type="checkbox" id="spec"> 频谱条（播放音乐时居中显示）</label></div>
    <div class="row"><label style="flex:1"><input type="checkbox" id="dance"> 彩灯律动（灯随音乐/人声跳动，开启时覆盖灯效）</label></div>
  <div class="hint">提示：对它说「播放 XXX」点歌后，再说「把歌名报上来」它会调
  <code>self.music.now</code> 把歌名打到屏幕顶部。三个开关即时生效并自动保存。</div>
</div>

</div><!-- /pg-setup -->

<script>
const $ = s => document.querySelector(s);

/* ── 2026-11-11 分页：主页大按钮 / 控制 / 设置 ── */
function showPage(name){
  document.querySelectorAll('.page').forEach(p => p.classList.toggle('on', p.id === 'pg-' + name));
  document.querySelectorAll('.nav button').forEach(b => b.classList.toggle('on', b.dataset.page === name));
  window.scrollTo(0, 0);
}
document.querySelectorAll('[data-page]').forEach(b => { b.onclick = () => showPage(b.dataset.page); });

/* ── 2026-11-11 电机设置：全局速度缩放 + 左右轮交换 ──
   ⚠️ 单轮反相（从机 inv_l / inv_r）在主机侧做不了：从机的差速混控里 steer 只走
   一次 expo 曲线，拿 steer 去扛单轮反相会让两侧速度一起失真，必须改从机固件。
   这里只提供「整体快慢」和「左右轮交换」（等价 steer 取反，接线接反时用）。 */
function carSpd(){ const v = parseInt(localStorage.getItem('carSpd'), 10); return isNaN(v) ? 80 : v; }
function carSwp(){ return localStorage.getItem('carSwp') === '1'; }
function carApply(){
  $('#spd').value = carSpd();
  $('#spdV').textContent = carSpd() + '%';
  $('#swp').checked = carSwp();
  /* 2026-11-12：页面每次打开都同步一次全局速度，从机重启后不会丢。
     ESP-NOW 没起来时静默失败，不影响页面。 */
  post('/api/speed?pct=' + carSpd());
}
$('#spd').oninput = () => { $('#spdV').textContent = $('#spd').value + '%'; };
$('#spd').onchange = () => {
  localStorage.setItem('carSpd', $('#spd').value);
  /* 2026-11-12：滑块改为全局速度，直接下发从机（遥控 + 探索都生效） */
  post('/api/speed?pct=' + $('#spd').value);
};
$('#swp').onchange = () => { localStorage.setItem('carSwp', $('#swp').checked ? '1' : '0'); };
carApply();

/* ── 距离保护（GY-530）：阈值存在从机，主机网页只负责读/下发 ──
   GET /api/dist 返回当前设置 + 最近一次拦截；带 obs/fall/en 参数就是下发。
   单位统一厘米（从机内部毫米）。 */
async function distState(){
  try{
    const r = await fetch('/api/dist?t=' + Date.now());
    const j = await r.json();
    $('#disten').checked = !!j.enabled;
    /* 正在输入的数字不要被轮询覆盖（否则改到一半被重置） */
    if (document.activeElement !== $('#distobs')) $('#distobs').value = j.obstacle_cm;
    if (document.activeElement !== $('#distfall')) $('#distfall').value = j.fall_cm;

    /* 实时距离 + 从机在线状态：调阈值前先看这个 */
    if (j.linked){
      $('#distlive').textContent = '从机在线 · 当前距离 ' + j.distance_cm + ' 厘米 · '
        + '实际阈值 碰撞 ' + j.obstacle_live_cm + ' / 跌落 ' + j.fall_live_cm + ' 厘米 · '
        + (j.sensor_ok ? ('传感器正常 ' + j.fps + ' fps') : '传感器未就绪')
        + ' · 状态 ' + j.state;
      $('#distlive').style.color = '#8ad';
    } else {
      $('#distlive').textContent = '从机离线（3 秒没收到遥测）';
      $('#distlive').style.color = '#f77';
    }

    if (j.last_block){
      $('#distmsg').textContent = '最近一次拦截：' + j.last_block + '，距离 '
        + j.last_block_cm + ' 厘米，' + Math.round((j.last_block_age_ms || 0) / 1000)
        + ' 秒前，本次开机共拦 ' + j.block_count + ' 次。';
    } else {
      $('#distmsg').textContent = '本次开机还没被拦过。';
    }
  }catch(e){}
}
$('#distset').onclick = async () => {
  const q = '/api/dist?obs=' + encodeURIComponent($('#distobs').value)
          + '&fall=' + encodeURIComponent($('#distfall').value)
          + '&en=' + ($('#disten').checked ? 1 : 0);
  const r = await post(q);
  try{
    const j = JSON.parse(r);
    $('#distmsg').textContent = j.success ? '已下发：碰撞 ' + j.obstacle_cm
      + ' 厘米，跌落 ' + j.fall_cm + ' 厘米' : (j.message || '下发失败');
  }catch(e){ $('#distmsg').textContent = r; }
};
distState();
setInterval(distState, 5000);

/* ── 自主探索（网页直发，绕过语音）：语音调 MCP 工具时好时坏，
   实测 AI 会「口头答应但不发指令」，这里直接打 ESP-NOW 最可靠 */
async function expState(){
  try{
    const j = await (await fetch('/api/explore?t=' + Date.now())).json();
    $('#expstart').textContent = j.running ? '探索中…' : '开始探索';
    $('#expstart').style.background = j.running ? '#2ecc71' : '';
    if (j.running) {
      $('#expmsg').textContent = '正在探索 · 当前距离 ' + j.distance_cm
        + ' 厘米 · 遇障自动换方向，不会停';
      $('#expmsg').style.color = '#2ecc71';
    } else if (j.linked) {
      $('#expmsg').textContent = '从机在线，点「开始探索」';
      $('#expmsg').style.color = '';
    } else {
      $('#expmsg').textContent = '从机离线';
      $('#expmsg').style.color = '#f77';
    }
  }catch(e){}
}
$('#expstart').onclick = async () => {
  /* spd 固定 100：全局速度由从机统一缩放，这里再乘一次会双重缩放 */
  const r = await post('/api/explore?action=start&dur=' + $('#expdur').value + '&spd=100');
  $('#expmsg').textContent = r;
  expState();
};
$('#expstop').onclick = async () => {
  const r = await post('/api/explore?action=stop');
  $('#expmsg').textContent = r;
  expState();
};
expState();
setInterval(expState, 2000);

/* ── 主页：从机连接状态 + 实时距离 + 实际生效阈值 ──
   1.5 秒刷新，且只在主页可见时才发请求（图传/控制页不额外占 httpd）。 */
async function homeLink(){
  const page = document.getElementById('pg-home');
  if (!page || !page.classList.contains('on')) return;
  try{
    const j = await (await fetch('/api/dist?t=' + Date.now())).json();
    const dot = $('#homeDot'), txt = $('#homeLink'), live = $('#homeLive');
    if (j.linked){
      dot.style.color = '#2ecc71';
      txt.textContent = '从机已连接';
      txt.style.color = '#2ecc71';
      live.textContent = '距离 ' + j.distance_cm + ' 厘米 · 实际碰撞阈值 '
        + j.obstacle_live_cm + ' 厘米 · 实际跌落阈值 ' + j.fall_live_cm + ' 厘米 · '
        + (j.sensor_ok ? ('传感器正常 ' + j.fps + ' fps') : '传感器未就绪')
        + (j.state && j.state !== 'clear' ? (' · ' + j.state) : '');
    } else {
      dot.style.color = '#e74c3c';
      txt.textContent = '从机未连接';
      txt.style.color = '#e74c3c';
      live.textContent = '从机没上电，或没和主机对上频（5 秒内没收到遥测）';
    }
  }catch(e){}
}
homeLink();
/* 2026-11-13：1s → 500ms。从机遥测是 5Hz（200ms），网页这边才是延迟大头：
   1s 轮询时最坏要 1.2 秒才看到新读数。500ms 后最坏约 0.7 秒。
   不再提高从机发射频率 —— 空口要留给音频 UDP。 */
setInterval(homeLink, 500);

/* ── 语音打断开关（NVS voice/interrupt）── */
async function viState(){
  try{
    const j = await (await fetch('/api/voice?t=' + Date.now())).json();
    $('#vi').checked = !!j.interrupt;
  }catch(e){}
}
$('#vi').onchange = async () => {
  await post('/api/voice?interrupt=' + ($('#vi').checked ? 1 : 0));
};
viState();

/* 摇杆/方向键统一出口：/api/car（ESP-NOW 发给 C3 从机）。
   ms=500：从机 500ms 收不到新指令就自己停 —— 手机掉线/切后台不会飞车，
   所以按住不动时靠 200ms 心跳续命（见下面的 keep 定时器）。 */
let carLast = 0, carTimer = null, carPendS = 0, carPendT = 0;
function carSendNow(s, t){
  /* 2026-11-12：全局速度改由从机统一缩放（/api/speed 下发），
     这里不能再乘系数，否则 30% × 80% 会双重缩放。 */
  let S = s, T = t;
  if (carSwp()) S = -S;
  post('/api/car?steer=' + S + '&throttle=' + T + '&ms=500');
}
function carSend(s, t){
  carPendS = s; carPendT = t;
  const now = Date.now();
  if (now - carLast >= 40){ carLast = now; carSendNow(s, t); return; }
  if (!carTimer){
    carTimer = setTimeout(() => { carTimer = null; carLast = Date.now(); carSendNow(carPendS, carPendT); }, 40);
  }
}
function carHalt(){
  if (carTimer){ clearTimeout(carTimer); carTimer = null; }
  carLast = 0;
  carSendNow(0, 0);
}

/* ── 摇杆 ── */
const pad = $('#pad'), knob = $('#knob');
let jDrag = false, jS = 0, jT = 0, jKeep = null;
function jKnob(dx, dy){ knob.style.left = 'calc(50% + ' + dx + 'px)'; knob.style.top = 'calc(50% + ' + dy + 'px)'; }
function jMove(cx, cy){
  const r = pad.getBoundingClientRect();
  const max = Math.max(1, r.width / 2 - knob.offsetWidth / 2);
  let dx = cx - (r.left + r.width / 2), dy = cy - (r.top + r.height / 2);
  const d = Math.sqrt(dx * dx + dy * dy);
  if (d > max){ dx = dx / d * max; dy = dy / d * max; }
  jKnob(dx, dy);
  jS = Math.round(dx / max * 100); jT = Math.round(-dy / max * 100);
  $('#jS').textContent = jS; $('#jT').textContent = jT;
  carSend(jS, jT);
}
function jStop(){
  if (!jDrag) return;
  jDrag = false; jS = 0; jT = 0;
  if (jKeep){ clearInterval(jKeep); jKeep = null; }
  jKnob(0, 0); $('#jS').textContent = 0; $('#jT').textContent = 0;
  carHalt();
}
pad.addEventListener('pointerdown', e => {
  e.preventDefault(); jDrag = true;
  pad.setPointerCapture(e.pointerId);
  if (!jKeep) jKeep = setInterval(() => { if (jDrag) carSend(jS, jT); }, 200);
  jMove(e.clientX, e.clientY);
});
pad.addEventListener('pointermove', e => { if (jDrag) jMove(e.clientX, e.clientY); });
pad.addEventListener('pointerup', jStop);
pad.addEventListener('pointercancel', jStop);
document.addEventListener('visibilitychange', () => { if (document.hidden) jStop(); });

/* ── 方向键（按住一直走，松手停） ── */
const kDirVec = {forward:[0,80], backward:[0,-80], left:[-80,0], right:[80,0]};
document.querySelectorAll('.dirpad [data-act]').forEach(b => {
  const act = b.dataset.act;
  b.addEventListener('pointerdown', e => {
    e.preventDefault();
    if (act === 'stop'){ jStop(); carHalt(); return; }
    const v = kDirVec[act];
    if (!v) return;
    carSend(v[0], v[1]);
    if (!b._hold) b._hold = setInterval(() => carSend(v[0], v[1]), 200);
  });
  const up = () => { if (b._hold){ clearInterval(b._hold); b._hold = null; } carHalt(); };
  b.addEventListener('pointerup', up);
  b.addEventListener('pointercancel', up);
  b.addEventListener('pointerleave', up);
});

/* ── 主页按钮 ──
   图传是按需的：先让设备进「图传模式」（停小智 + 屏上出表情 + 起 81 口服务器），
   成功后才跳过去。81 口是独立 httpd：ESP-IDF 的 httpd 是单任务，/stream 一直
   不返回会把同服务器的摇杆请求堵死，所以必须和 80 口分开。
   速度和交换用 query 带过去：两个端口是不同 origin，localStorage 不互通。 */
$('#homeCam').onclick = async () => {
  const b = $('#homeCam');
  b.disabled = true;
  const tip = b.querySelector('.sub');
  if (tip) tip.textContent = '正在启动图传…';
  const r = (await post('/api/camstart')).trim();
  b.disabled = false;
  if (r !== 'ok') {
    if (tip) tip.textContent = '视频 + 摇杆';
    alert(r || '图传启动失败');
    return;
  }
  /* 页面走 80 口（/cam）：80 口永远在，页面一定能打开；视频/摇杆各走各的端口 */
  location.href = '/cam?spd=' + carSpd() + '&swp=' + (carSwp() ? 1 : 0);
};
$('#homeExitCar').onclick = () => $('#carexit').click();
/* ── 屏幕视图（2026-11-06 用户要求）：主题/时钟/壁纸 + 样式选择 ── */
const kClockStyleNames = ['极光','暖橙','青翠','极简'];
/* 视图名：view 是类型，style 在主题下是主题 id、在时钟下是样式序号 */
function viewLabel(v, style){
  if (v === 'clock') return '时钟 · ' + (kClockStyleNames[style] || ('样式' + style));
  if (v === 'wallpaper') return '自定义壁纸';
  const n = (window.j && window.j.theme_names) ? window.j.theme_names[style] : '';
  return '大肥鱼 · ' + ((style|0) + 1) + (n ? '. ' + n : '');
}
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

  /* 当前 vs 开机默认：一样就把保存按钮置灰，省得反复点 */
  const did = (j.default_id === undefined) ? 0 : j.default_id;
  const dv = (did === 100) ? 'clock' : ((did === 101) ? 'wallpaper' : 'theme');
  const ds = (did === 100) ? (j.default_style || 0) : did;
  $('#viewNow').textContent = '当前：' + viewLabel(v, j.style||0) +
                              '　开机默认：' + viewLabel(dv, ds);
  const same = (v === dv) && ((j.style||0) === ds);
  const sb = $('#vSave');
  sb.disabled = same;
  sb.textContent = same ? '已是开机默认' : '保存为开机默认';
}
/* ⚠️ 切换是「预览」：HTTP 只置请求，tick 下一帧才上屏。
   立刻 state() 会读回旧值（看着像没反应），所以等 400ms 再刷新。 */
const viewGo = (u) => () => post(u).then((t) => {
  if (t && t.indexOf('"ok":false') >= 0) {
    alert('壁纸还没上传，先在下面「自定义壁纸」里传一张');
  }
  setTimeout(state, 400);
});
$('#vTheme').onclick = viewGo('/api/view?target=theme');
$('#vClock').onclick = viewGo('/api/view?target=clock');
$('#vWallpaper').onclick = viewGo('/api/view?target=wallpaper');
$('#vThemeStyle').onchange = () => post('/api/view?target=theme&style=' +
    $('#vThemeStyle').value).then(() => setTimeout(state, 400));
$('#vClockStyle').onchange = () => post('/api/view?target=clock&style=' +
    $('#vClockStyle').value).then(() => setTimeout(state, 400));
$('#vSave').onclick = () => post('/api/view/save').then(() => setTimeout(state, 400));

async function state(){
  const r = await fetch('/api/state?t=' + Date.now()); const j = await r.json(); window.j = j;
  /* 养成系列已整体删除：状态行只保留还有意义的信息 */
  $('#meta').textContent = '壁纸 ' + (j.wallpaper ? '已设置' : '无');
  viewRender(j);
  /* 2026-11-05 音乐可视化开关回显 */
  if (j.spectrum !== undefined) $('#spec').checked = !!j.spectrum;
  if (j.subtitle !== undefined && $('#sub')) $('#sub').checked = !!j.subtitle;
  if (j.dance !== undefined) $('#dance').checked = !!j.dance;
  if (j.standby !== undefined && $('#standby')) $('#standby').value = j.standby;
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
/* 2026-11-15 待机省电：板级每秒轮询，改完立即生效（不用重启） */
$('#standbySet').onclick = async () => {
  const m = parseInt($('#standby').value || '0', 10);
  const v = isNaN(m) ? 0 : Math.max(0, Math.min(120, m));
  const r = await post('/api/config?standby=' + v);
  $('#standbyMsg').textContent = (r === 'ok')
    ? (v > 0 ? ('已保存：待机 ' + v + ' 分钟后屏幕降到最暗') : '已关闭待机省电')
    : ('保存失败：' + r);
  state();
};

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

$('#carexit').onclick = async () => {
  if (!confirm('退出小车模式并重启回正常模式？')) return;
  alert(await post('/api/carmode'));
};

/* ── 屏幕方向（2026-11-13）── */
$('#lcdLand').onclick = async () => {
  if (!confirm('切换为横屏 320×240 并重启？')) return;
  $('#lcdMsg').textContent = await post('/api/lcd?mode=1');
};
$('#lcdPort').onclick = async () => {
  if (!confirm('切换为竖屏 240×320 并重启？')) return;
  $('#lcdMsg').textContent = await post('/api/lcd?mode=0');
};

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
                     "\"spectrum\":%d,\"subtitle\":%d,\"dance\":%d,\"standby\":%d,"
                     "\"view\":\"%s\",\"style\":%d,\"default_id\":%d,\"theme_names\":%s}",
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
                     c.spectrum_enabled ? 1 : 0,
                     c.subtitle_enabled ? 1 : 0,
                     c.led_dance_enabled ? 1 : 0,
                     StandbyMinutes(),
                     view_s, style, (disp != nullptr) ? disp->WebDefaultView() : 0, names);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* 网页切换屏幕视图（2026-11-06 用户要求，2026-11-14 重做语义）：
   GET  /api/view                    查询当前 {view, style, default_id}
   POST /api/view?target=theme       回「上次那套」大肥鱼（不再硬编码 0）
   POST /api/view?target=theme&style=n  预览第 n 套肥鱼
   POST /api/view?target=clock[&style=n]  预览翻页时钟（可顺带切样式）
   POST /api/view?target=wallpaper   预览自定义壁纸
   POST /api/view/save               把当前视图存成开机默认

   ⚠️ 关键语义（2026-11-14 用户定稿）：切换一律是**预览** —— 立即上屏但
   **不写 NVS**，重启后仍是上次保存的那套；要点「保存为开机默认」才记住。
   设备端双击音量键 / 菜单切主题不受影响，仍是即切即存。

   ⚠️ 原来这里只注册了 HTTP_GET，而网页用 POST 调 → 405，按钮从来没生效过。
   现在 GET/POST 都注册（GET 保留给 curl 调试）。 */
esp_err_t HandleView(httpd_req_t* req) {
    auto* disp = DafeiyuDisplay::Instance();
    if (disp == nullptr) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "display not ready");
    }
    const std::string target = QueryValue(req->uri, "target");
    const std::string style_s = QueryValue(req->uri, "style");
    if (target.empty()) {
        const int v = disp->WebCurrentView();
        const int dv = disp->WebDefaultView();
        char b[160];
        const int n = snprintf(b, sizeof(b),
                               "{\"view\":\"%s\",\"style\":%d,\"default_id\":%d,"
                               "\"default_view\":\"%s\",\"default_style\":%d}",
                               (v == 100) ? "clock" : ((v == 101) ? "wallpaper" : "theme"),
                               (v == 100) ? disp->WebClockStyle(-1) : v, dv,
                               (dv == 100) ? "clock" : ((dv == 101) ? "wallpaper" : "theme"),
                               (dv == 100) ? disp->WebClockStyle(-1) : dv);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, b, n);
    }

    int id = -1;
    if (target == "theme") {
        /* 不带 style = 「回肥鱼」→ 回到上次用的那套，而不是永远第 1 套待机 */
        id = style_s.empty() ? disp->WebLastPetTheme() : atoi(style_s.c_str());
    } else if (target == "clock") {
        id = 100;
    } else if (target == "wallpaper") {
        id = 101;
    }
    if (id < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "usage: target=theme|clock|wallpaper");
    }
    bool ok = disp->WebSetView(id);
    if (ok && target == "clock" && !style_s.empty()) {
        /* 预览：样式也不写 NVS，等 /api/view/save 一起记 */
        disp->WebClockStyle(atoi(style_s.c_str()), false);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true,\"preview\":true}"
                                      : "{\"ok\":false}");
}

/* 把当前视图（含刚预览完还没被 tick 消费的目标）写成开机默认 */
esp_err_t HandleViewSave(httpd_req_t* req) {
    auto* disp = DafeiyuDisplay::Instance();
    if (disp == nullptr) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "display not ready");
    }
    const bool ok = disp->WebSaveView();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true,\"saved\":true}"
                                      : "{\"ok\":false}");
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

/* 2026-11-10 HandleRebootToGame（进游戏机）已随 GameBox 双固件删除。 */

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
    /* 2026-11-10 LCD 颜色开关已删：参数板级修死，不再收 invert/rgb_swap。 */
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
    /* 2026-11-15 待机省电：standby=分钟数（0=关闭）。存独立 NVS，
       板级定时器每秒轮询，改完立即生效，不用重启。 */
    std::string standby = QueryValue(q, "standby");
    if (!standby.empty()) {
        SetStandbyMinutes(atoi(standby.c_str()));
    }
    SaveConfig(c);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "ok");
}

/* ── 小车模式遥控（2026-11-11）──────────────────────────────────────────
   GET  /api/car?steer=0&throttle=60&ms=400
        steer    -100..100（>0 右转）
        throttle -100..100（>0 前进）
        ms       自动停时长，省略 = 0（一直跑到下一条）；
                 网页摇杆建议带 300~500，松手/掉线后从机自己停。
   返回 "ok"，失败返回错误文本。

   守卫：只有 car_mode=1 才允许发。切换模式必然重启，所以静态缓存安全，
   避免 10Hz 摇杆把 NVS 读出热点。 */
esp_err_t HandleCarDrive(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    /* 2026-11-11：图传页在 81 端口（独立 httpd），摇杆要从那个页面打回 80 口，
       属于跨 origin 请求，没有这个头浏览器会拦掉响应。MJPEG 那边只发不收，
       这里只加 ACAO 即可（GET 无自定义头，不触发预检）。 */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    static int car_mode_cached = -1;
    if (car_mode_cached < 0) {
        Settings ls("lcd_display", false);
        car_mode_cached = ls.GetInt("car_mode", 0);
    }
    if (car_mode_cached == 0) {
        return httpd_resp_sendstr(req, "当前不是小车模式，请先进入小车模式");
    }

    std::string s_steer = QueryValue(req->uri, "steer");
    std::string s_thr = QueryValue(req->uri, "throttle");
    if (s_thr.empty()) s_thr = QueryValue(req->uri, "speed");   /* 兼容叫法 */
    if (s_steer.empty() && s_thr.empty()) {
        return httpd_resp_sendstr(req, "用法：/api/car?steer=0&throttle=60&ms=400");
    }

    int steer = s_steer.empty() ? 0 : atoi(s_steer.c_str());
    int throttle = s_thr.empty() ? 0 : atoi(s_thr.c_str());
    std::string s_ms = QueryValue(req->uri, "ms");
    int ms = s_ms.empty() ? 0 : atoi(s_ms.c_str());
    if (ms < 0) ms = 0;
    if (ms > 5000) ms = 5000;

    if (!EspNowLink::Get().Send(steer, throttle, static_cast<uint16_t>(ms))) {
        return httpd_resp_sendstr(req, "ESP-NOW 发送失败（WiFi 未就绪？）");
    }

    /* 10Hz 摇杆会刷屏，只在节流后打一条便于取证 */
    static uint32_t cnt = 0;
    if ((++cnt % 20) == 1) {
        ESP_LOGI(TAG, "小车遥控: steer=%d throttle=%d ms=%d", steer, throttle, ms);
    }
    return httpd_resp_sendstr(req, "ok");
}

/* ── 距离保护设置（2026-11-12）────────────────────────────────────────
   GET  /api/dist                              读设置 + 最近一次拦截
   GET  /api/dist?obs=6&fall=45&en=1           下发 + 存 NVS（单位厘米）
   传感器在 C3 从机，阈值只能通过 ESP-NOW 下发；从机自己也存 NVS。
   带参数时才下发，避免网页轮询把阈值反复重发。 */
esp_err_t HandleDistCfg(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    Settings s("car_guard", true);
    int obs_mm = s.GetInt("obs_mm", 60);
    int fall_mm = s.GetInt("fall_mm", 450);
    bool en = s.GetInt("enabled", 1) != 0;

    const std::string s_obs = QueryValue(req->uri, "obs");
    const std::string s_fall = QueryValue(req->uri, "fall");
    const std::string s_en = QueryValue(req->uri, "en");

    bool sent = true;
    if (!s_obs.empty() || !s_fall.empty() || !s_en.empty()) {
        if (!s_obs.empty()) obs_mm = atoi(s_obs.c_str()) * 10;
        if (!s_fall.empty()) fall_mm = atoi(s_fall.c_str()) * 10;
        if (!s_en.empty()) en = atoi(s_en.c_str()) != 0;

        sent = EspNowLink::Get().SendConfig(obs_mm, fall_mm, en);
        if (sent) {
            s.SetInt("obs_mm", obs_mm);
            s.SetInt("fall_mm", fall_mm);
            s.SetInt("enabled", en ? 1 : 0);
            s.Commit();
            ESP_LOGI(TAG, "网页设置距离保护: 障碍<=%dmm 跌落>=%dmm enabled=%d",
                     obs_mm, fall_mm, (int)en);
        } else {
            ESP_LOGW(TAG, "网页设置距离保护失败：ESP-NOW 未启动");
        }
    }

    BlockInfo blk;
    const bool has_blk = EspNowLink::Get().LastBlock(&blk);
    LinkStatus st;
    const bool linked = EspNowLink::Get().Status(&st);

    /* 2026-11-12：网页要能看到「当前距离」，否则用户不知道该把阈值设多少 */
    const char* state_name = "unknown";
    if (linked) {
        switch (st.state) {
            case 2: state_name = "clear"; break;
            case 3: state_name = "obstacle"; break;
            case 4: state_name = "fall"; break;
            case 1: state_name = "no_sensor"; break;
            case 0: state_name = "disabled"; break;
            default: break;
        }
    }

    char blk_txt[192];
    if (has_blk) {
        snprintf(blk_txt, sizeof(blk_txt),
                 "\"last_block\":\"%s\",\"last_block_cm\":%.1f,"
                 "\"last_block_age_ms\":%u,\"block_count\":%d",
                 blk.type == 2 ? "跌落（悬空）" : "碰撞（障碍）", blk.mm / 10.0,
                 (unsigned)blk.age_ms, blk.count);
    } else {
        snprintf(blk_txt, sizeof(blk_txt), "\"last_block\":null,\"block_count\":0");
    }

    char buf[768];
    snprintf(buf, sizeof(buf),
             "{\"success\":%s,\"enabled\":%s,\"obstacle_cm\":%.1f,\"fall_cm\":%.1f,"
             "\"linked\":%s,\"link_age_ms\":%u,\"distance_cm\":%.1f,"
             "\"obstacle_live_cm\":%.1f,\"fall_live_cm\":%.1f,"
             "\"sensor_ok\":%s,\"fps\":%u,\"state\":\"%s\",%s,\"message\":\"%s\"}",
             sent ? "true" : "false", en ? "true" : "false",
             obs_mm / 10.0, fall_mm / 10.0,
             linked ? "true" : "false", (unsigned)st.age_ms,
             linked ? st.mm / 10.0 : 0.0,
             linked ? st.obstacle_mm / 10.0 : 0.0,
             linked ? st.fall_mm / 10.0 : 0.0,
             (linked && (st.flags & TLM_FLAG_SENSOR)) ? "true" : "false",
             (unsigned)(linked ? st.fps : 0), state_name, blk_txt,
             sent ? "ok" : "ESP-NOW 没启动，未下发");
    return httpd_resp_sendstr(req, buf);
}

/* ── 全局速度（2026-11-12）──
   GET /api/speed?pct=80
   从机在 Motor2WD::Drive() 入口统一缩放，遥控和探索共用这一个值。
   所以主机侧不能再自己乘系数，否则会变成「30% × 80%」双重缩放。 */
esp_err_t HandleSpeedSet(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    std::string s = QueryValue(req->uri, "pct");
    int pct = s.empty() ? 0 : atoi(s.c_str());
    if (pct <= 0) {
        return httpd_resp_sendstr(req, "用法：/api/speed?pct=80");
    }
    if (pct < 20) pct = 20;
    if (pct > 100) pct = 100;
    if (!EspNowLink::Get().SendSpeed(pct)) {
        return httpd_resp_sendstr(req, "ESP-NOW 发送失败（WiFi 未就绪？）");
    }
    /* 2026-11-14：主控记住滑块值。从机那侧不存 NVS（重启回 100），
       开机时板级会用这个值补发一次，避免「滑块 30、车跑 100」。 */
    {
        Settings s("lcd_display", true);
        s.SetInt("speed_pct", pct);
        s.Commit();
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "全局速度 %d%%", pct);
    return httpd_resp_sendstr(req, buf);
}

/* ── 内存账单（2026-11-13 用户质问「到底谁占的」）──
   GET /api/mem
   之前只打 free/minimal 两个数，看不出结构。这里直接给内部 SRAM / PSRAM 的
   总量、空闲、最大连续块，以及各任务栈水位 —— 判断是底座占满还是碎片化。 */
esp_err_t HandleMemInfo(httpd_req_t* req) {
    multi_heap_info_t sram;
    multi_heap_info_t psram;
    heap_caps_get_info(&sram, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM);

    /* multi_heap_info_t 没有 total_bytes：总量 = 已分配 + 空闲 */
    const unsigned sram_total = (unsigned)(sram.total_allocated_bytes + sram.total_free_bytes);
    const unsigned psram_total = (unsigned)(psram.total_allocated_bytes + psram.total_free_bytes);

    char buf[640];
    int n = snprintf(buf, sizeof(buf),
        "{\"sram\":{\"total\":%u,\"used\":%u,\"free\":%u,\"largest\":%u,\"min_free\":%u,"
        "\"blocks\":%u,\"free_blocks\":%u,\"alloc_blocks\":%u},"
        "\"psram\":{\"total\":%u,\"used\":%u,\"free\":%u,\"largest\":%u,\"alloc_blocks\":%u}}",
        sram_total, (unsigned)sram.total_allocated_bytes,
        (unsigned)sram.total_free_bytes, (unsigned)sram.largest_free_block,
        (unsigned)sram.minimum_free_bytes,
        (unsigned)sram.total_blocks, (unsigned)sram.free_blocks,
        (unsigned)sram.allocated_blocks,
        psram_total, (unsigned)psram.total_allocated_bytes,
        (unsigned)psram.total_free_bytes, (unsigned)psram.largest_free_block,
        (unsigned)psram.allocated_blocks);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, n);
}

/* ── 探索模式（2026-11-12）──
   GET /api/explore?action=start&dur=180&spd=80
   GET /api/explore?action=stop
   GET /api/explore?action=status
   为什么要这个接口：语音触发依赖服务端 AI 调 self.car.explore，实测
   经常"嘴上答应但没真发指令"，从机日志里连 [探索] 开始 都没有。
   网页按钮直连 ESP-NOW，是唯一可靠的通路。 */
esp_err_t HandleExploreCfg(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    std::string s_action = QueryValue(req->uri, "action");
    if (s_action.empty()) s_action = "status";
    int dur = atoi(QueryValue(req->uri, "dur").c_str());
    int spd = atoi(QueryValue(req->uri, "spd").c_str());
    if (dur <= 0) dur = 180;
    if (dur > 3600) dur = 3600;
    /* 100 = 不额外缩放，真正速度由从机的全局速度（/api/speed）决定 */
    if (spd <= 0) spd = 100;

    auto& link = EspNowLink::Get();
    char buf[224];

    if (s_action == "status") {
        LinkStatus st;
        const bool linked = link.Status(&st);
        const bool running = linked && ((st.flags & TLM_FLAG_EXPLORE) != 0);
        snprintf(buf, sizeof(buf),
                 "{\"success\":true,\"running\":%s,\"linked\":%s,\"distance_cm\":%.1f,"
                 "\"state\":%d,\"message\":\"%s\"}",
                 running ? "true" : "false", linked ? "true" : "false",
                 linked ? st.mm / 10.0 : 0.0, linked ? st.state : 0,
                 running ? "正在探索" : (linked ? "没有在探索" : "从机离线"));
        return httpd_resp_sendstr(req, buf);
    }

    const int act = (s_action == "start") ? 1 : 0;
    if (!link.SendExplore(act, spd, dur)) {
        snprintf(buf, sizeof(buf),
                 "{\"success\":false,\"message\":\"指令没发出去，ESP-NOW 没启动\"}");
        return httpd_resp_sendstr(req, buf);
    }
    snprintf(buf, sizeof(buf),
             "{\"success\":true,\"message\":\"%s（%d 秒，速度 %d%%）\"}",
             act ? "已开始探索" : "已停止探索", dur, spd);
    ESP_LOGI(TAG, "网页探索: %s dur=%d spd=%d", act ? "开始" : "停止", dur, spd);
    return httpd_resp_sendstr(req, buf);
}

/* ── 语音打断开关（2026-11-12）──
   GET /api/voice               读当前值
   GET /api/voice?interrupt=0   关闭（AI 说话时忽略唤醒词）
   GET /api/voice?interrupt=1   开启（默认，喊一声就打断） */
esp_err_t HandleVoiceCfg(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    auto& app = Application::GetInstance();
    const std::string s = QueryValue(req->uri, "interrupt");
    if (!s.empty()) {
        app.SetVoiceInterruptEnabled(atoi(s.c_str()) != 0);
    }

    char buf[96];
    snprintf(buf, sizeof(buf), "{\"success\":true,\"interrupt\":%s}",
             app.IsVoiceInterruptEnabled() ? "true" : "false");
    return httpd_resp_sendstr(req, buf);
}

/* 2026-11-10 退出小车模式（保底通道）：写 NVS 后重启。
   不依赖语音/LLM —— 小车模式界面出问题时网页点一下就回来。 */
esp_err_t HandleCarModeExit(httpd_req_t* req) {
    Settings s("lcd_display", true);
    s.SetInt("car_mode", 0);
    s.SetInt("ui_mode", 0);   /* 2026-11-15 与 car_mode 同步，否则重启后仍按旧 ui_mode */
    /* 2026-11-13：还原进小车前方向（主题竖屏进小车再退出要保持竖屏） */
    s.SetInt("lcd_mode", s.GetInt("car_prev_lcd", 1) != 0 ? 1 : 0);
    /* ⚠️ 2026-11-10：必须显式落盘再重启，否则写入随掉电丢失。 */
    s.Commit();
    ESP_LOGW(TAG, "网页：退出小车模式，已落盘，重启生效");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "已退出小车模式，设备重启中…");
    vTaskDelay(pdMS_TO_TICKS(1000));   // 给 HTTP 响应留时间发出去
    esp_restart();
    return ESP_OK;   // 不会执行到
}

/* ── 屏幕方向（2026-11-13 用户要求：主题模式下也能切横竖屏）──
   GET /api/lcd            读当前方向
   GET /api/lcd?mode=1     横屏 320x240
   GET /api/lcd?mode=0     竖屏 240x320
   ⚠️ 两套分辨率必须重建 LVGL 和显示对象，运行期切不了 —— 只能写 NVS 后重启
   （与小车模式同款流程）。小车模式固定竖屏，这里会拒绝。 */
esp_err_t HandleLcdMode(httpd_req_t* req) {
    std::string s = QueryValue(req->uri, "mode");
    if (s.empty()) {
        Settings cfg("lcd_display", true);
        const bool land = cfg.GetInt("lcd_mode", 1) != 0;
        const bool car = cfg.GetInt("car_mode", 0) != 0;
        char buf[80];
        snprintf(buf, sizeof(buf), "{\"lcd_mode\":%d,\"car_mode\":%d,\"landscape\":%s}",
                 land ? 1 : 0, car ? 1 : 0, land ? "true" : "false");
        httpd_resp_set_type(req, "application/json; charset=utf-8");
        return httpd_resp_sendstr(req, buf);
    }

    Settings cfg("lcd_display", true);
    if (cfg.GetInt("car_mode", 0) != 0) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "小车模式固定竖屏，请先退出小车模式");
    }

    const int mode = atoi(s.c_str());
    cfg.SetInt("lcd_mode", mode != 0 ? 1 : 0);
    cfg.Commit();   /* ⚠️ 必须先落盘再重启，否则写入丢失 */
    ESP_LOGW(TAG, "网页：切到%s，已落盘，重启生效", mode != 0 ? "横屏" : "竖屏");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, mode != 0 ? "已切换为横屏，设备重启中…" : "已切换为竖屏，设备重启中…");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;   // 不会执行到
}

/* ── 图传模式开关（2026-11-12 用户定案）───────────────────────────────
   图传**不再常驻**。用户点网页「图传遥控」才进这个模式：
     · 小智停用：回待机 + 关唤醒词 + 关语音处理（不再听、不再抢麦克风）
     · 屏幕强制显示表情（原本待机会出时钟）
     · 起 81 口图传服务器（它要 6KB 内部 RAM，常驻会把 mbedTLS 挤死 ——
       实测「esp-aes: Failed to allocate memory / SSL send failed」连不上服务器）
   点图传页的「返回」→ /api/camstop：停流停服务器，小智原样还给用户。 */
/* 图传遥控页（2026-11-12 挪到 80 口）。
   81 口是图传服务器，只有进了图传模式才存在；页面放在 80 口，
   保证「点按钮 → 页面一定能打开」，视频再走 81。 */
esp_err_t HandleCamPage(httpd_req_t* req) {
    /* 页面一加载就踢掉上一条推流：81 口是单任务，旧流不走新流就永远排不上队
       （用户实测「第一次有画面，第二次没画面」就是这么来的）。 */
    KickCamStream();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, CamPageHtml(), HTTPD_RESP_USE_STRLEN);
}

esp_err_t HandleCamStart(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    Settings ls("lcd_display", false);
    if (ls.GetInt("car_mode", 0) == 0) {
        return httpd_resp_sendstr(req, "当前不是小车模式，图传只在车模式下可用");
    }
    /* 已经在图传模式里（页面刷新 / 「重启图传」按钮）就别再走一遍停小智 + 800ms 等待，
       但要踢掉旧推流，否则单任务服务器上旧流会把新流堵死。 */
    if (CamStreamRunning()) {
        KickCamStream();
        return httpd_resp_sendstr(req, "ok");
    }

    auto& app = Application::GetInstance();
    /* 先回待机：说话/聆听中直接推流会抢音频资源，而且 idle 的
       「开唤醒词」逻辑会把我们后面关掉的又打开（状态事件滞后）。 */
    if (app.GetDeviceState() != kDeviceStateIdle) {
        app.SetDeviceState(kDeviceStateIdle);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    auto& audio = app.GetAudioService();
    audio.EnableVoiceProcessing(false);
    audio.EnableWakeWordDetection(false);

    /* ⚠️ 2026-11-12 第二次修「偶尔报内存不足」：
       真正吃内存的不是唤醒词/语音处理，而是**在线的 WSS/TLS 会话**
       （刚说完话时那十几 KB 还挂着，空闲态 32KB 会掉到 6KB 以下）。
       小智本来就要停用，所以干脆把音频通道关掉，把 TLS 缓冲还回来再起服务器。
       注意用 CloseAudioChannelForSuspend（只关通道、保留 protocol_），
       不能用 ResetProtocol —— 那个会把 protocol_ 置空，设备之后叫不醒，只能重启。 */
    app.CloseAudioChannelForSuspend();
    vTaskDelay(pdMS_TO_TICKS(800));   /* 等主任务真正断开会话 */

    /* 2026-11-12：图传期间关掉 WiFi 省电。
       默认待机是 LOW_POWER（modem sleep），新 TCP 连接偶发会被丢掉 ——
       用户点进图传偶尔"网页连不上"就是这个。顺带把遥控延迟也压下来。 */
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    auto* disp = DafeiyuDisplay::Instance();
    if (disp != nullptr) {
        disp->SetCarFaceOnly(true);
    }

    if (!StartCamStreamServer()) {
        /* 起不来必须把小智还回去，不能把设备卡成哑巴 */
        audio.EnableWakeWordDetection(true);
        if (disp != nullptr) {
            disp->SetCarFaceOnly(false);
        }
        return httpd_resp_sendstr(req, "图传服务器启动失败（内部内存不足或 81 口被占，详见串口日志）");
    }
    ESP_LOGW(TAG, "图传模式：小智已停用（会话/唤醒词/语音处理关闭），81 口已起");
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t HandleCamStop(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    CamModeExit();
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

/* 退出图传模式的公共实现（2026-11-12）：
   三个入口都走这里 —— ① 页面点「⌂ 返回」的 /api/camstop；
   ② cam_stream 的空闲看门狗（页面被关掉、90 秒没请求）；
   ③ BOOT 键单击（2026-11-14 用户要求，板级丢独立任务调用）。
   第二个入口是用户踩出来的：关页面没点返回 → 小智一直停用 → 表现为"叫不应/连不上服务器"。
   ⚠️ 必须留在匿名 namespace **外面**：板级要按 dafeiyu::CamModeExit 取地址，
   放匿名 space 里就成了另一个实体，头文件声明会和它构成重载（编译报 ambiguous）。 */
void CamModeExit() {
    StopCamStreamServer();
    /* 图传期间的 PERFORMANCE 保持不回退。
       2026-11-15：实测本机 RTT 锯齿 17→110ms、网关 1~9ms，modem sleep 导致的
       TCP 队头阻塞就是音频断档的来源；用户优先流畅度 → 常驻 PS_NONE。 */
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    auto* disp = DafeiyuDisplay::Instance();
    if (disp != nullptr) {
        disp->SetCarFaceOnly(false);
    }
    /* 恢复小智放 application 主任务里做：EnableWakeWordDetection 内部要等唤醒词任务
       起来，别堵在 httpd 任务（或 esp_timer 任务）上。
       配置照 idle 分支来：只开唤醒词，语音处理保持关闭。
       ⚠️ 不要 EnableVoiceProcessing(true)：两个引擎同时抢麦克风，唤醒会失灵。 */
    Application::GetInstance().Schedule([]() {
        auto& app = Application::GetInstance();
        app.SetDeviceState(kDeviceStateIdle);
        auto& audio = app.GetAudioService();
        audio.EnableVoiceProcessing(false);
        audio.EnableWakeWordDetection(true);
        ESP_LOGW(TAG, "图传模式：小智已恢复");
    });
    ESP_LOGW(TAG, "图传模式：已退出");
}

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
    /* ⚠️ 2026-11-13 实测教训：这个函数在开机第 12 秒被调用，那一刻是内存
       最低谷（free 2.8KB / 最大块 2.3KB）。httpd 在内存不足时 socket() 会失败，
       而且失败后 ctrl socket(32768) 会被弄脏，之后每次重试都报 112，
       网页永久打不开（只能重启）。所以内存不够时干脆不尝试，交给 3 秒重试，
       等启动高峰过去（实测 29 秒后最大块回升到 8KB）再起。 */
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest < 6000) {
        ESP_LOGD(TAG, "内部 SRAM 最大块只有 %u，暂缓启动控制页",
                 (unsigned)largest);
        return false;
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
    /* ⚠️ 2026-11-12：24 → 32。路由已经 29 条了，超过上限的会被
       httpd_register_uri_handler 静默丢弃（返回 HANDLERS_FULL 但没人看），
       表现就是新接口一律 404（/api/speed 就这么消失的）。 */
    /* 2026-11-13 再放宽到 40：/api/lcd、/api/speed、/api/mem 都是新加的，
       32 又不够了。上限不够时 httpd_register_uri_handler 会静默失败 → 404。 */
    config.max_uri_handlers = 40;
    config.lru_purge_enable = true;  // 没槽位时回收最久没用的连接
    config.keep_alive_enable = true;
    config.keep_alive_idle = 2;
    /* ⚠️ 2026-11-06 修复「壁纸要传好几次才成功」：壁纸 150KB，手机 WiFi
       慢时 5 秒收不完 → recv 超时 ReadBody 失败 → 用户重传。放宽到 30 秒。 */
    config.recv_wait_timeout = 30;
    /* 2026-11-14：send 从 30 秒缩到 5 秒。
       httpd 是单任务，发送是同步阻塞的 —— 只要有一个客户端请求了首页却
       没读完响应（刷新中断、手机切后台、我这边脚本连续请求），整台服务器
       就会被占住整整 30 秒，期间首页和所有接口全部超时。
       用户报「http://IP/ 进不去」，等 35 秒又自己好了，就是这个超时。
       局域网发 30KB 首页实测 0.16 秒，5 秒足够；慢客户端断开比整站卡死好。
       ⚠️ recv 必须保持 30 秒：那是 150KB 壁纸上传用的，缩了就会收不全。 */
    config.send_wait_timeout = 5;
    httpd_handle_t server = nullptr;
    esp_err_t start_err = httpd_start(&server, &config);
    if (start_err != ESP_OK) {
        /* ⚠️ 2026-11-13 实测：设备已经连上路由器（ARP 能看到、能 ping 通），
           但配网 AP 的 web 服务没退干净，一直占着 80 →
           控制页每 3 秒重试都报 listen(112) EADDRINUSE，网页永远打不开。
           之前只干等重试，等于永远起不来。这里主动把配置 AP 停掉再重试。 */
        auto& wm = WifiManager::GetInstance();
        if (wm.IsConfigMode()) {
            /* ⚠️ 2026-11-17 修复「开机反复进配网、反复播报 AP」：
               以前这里只要 IsConfigMode() 就 StopConfigAp()，但配网期间
               配网页**本来就该占着 80**（手机要靠它配置）。控制页每 3 秒
               重试一次，每次都把配网 AP 拆掉 → 板级收到 ConfigModeExit →
               没 SSID → 1.5 秒后又重进配网 → 再播报一次，实测 3 秒一轮
               无限循环（串口 40 秒内 Access Point started 13 次）。
               正确判据是「是否已经真的连上路由器」：
                 · 没连上 = 正在配网，控制页无条件让路，绝不拆配网；
                 · 已连上 = 配网残留没退干净（2026-11-13 那种情况），才停它。 */
            if (!wm.IsConnected()) {
                ESP_LOGD(TAG, "配网进行中，控制页让路（等配网结束后再起）");
                return false;
            }
            ESP_LOGW(TAG, "已联网但配网 AP 仍占 80，停掉配置 AP 后重试");
            wm.StopConfigAp();
            vTaskDelay(pdMS_TO_TICKS(600));
            start_err = httpd_start(&server, &config);
        }
    }
    if (start_err != ESP_OK) {
        ESP_LOGW(TAG, "http server start failed（80 端口仍被占用，稍后重试）");
        return false;
    }

    const httpd_uri_t routes[] = {
        {"/", HTTP_GET, HandlePage, nullptr},
        {"/api/state", HTTP_GET, HandleState, nullptr},
        {"/api/view", HTTP_GET, HandleView, nullptr},
        /* 2026-11-14：网页一直用 POST 调，这里原来只注册 GET → 405，按钮全废。
           GET 保留给 curl 调试，POST 给页面。 */
        {"/api/view", HTTP_POST, HandleView, nullptr},
        {"/api/view/save", HTTP_GET, HandleViewSave, nullptr},
        {"/api/view/save", HTTP_POST, HandleViewSave, nullptr},
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
        {"/api/carmode", HTTP_POST, HandleCarModeExit, nullptr},   /* 2026-11-10 退出小车模式保底通道 */
        {"/api/lcd", HTTP_GET, HandleLcdMode, nullptr},            /* 2026-11-13 横竖屏切换（主题模式） */
        {"/api/lcd", HTTP_POST, HandleLcdMode, nullptr},
        {"/api/car", HTTP_GET, HandleCarDrive, nullptr},           /* 2026-11-11 小车遥控（ESP-NOW） */
        {"/api/car", HTTP_POST, HandleCarDrive, nullptr},
        {"/api/dist", HTTP_GET, HandleDistCfg, nullptr},           /* 2026-11-12 距离保护阈值（GY-530） */
        {"/api/dist", HTTP_POST, HandleDistCfg, nullptr},
        {"/api/voice", HTTP_GET, HandleVoiceCfg, nullptr},         /* 2026-11-12 语音打断开关 */
        {"/api/voice", HTTP_POST, HandleVoiceCfg, nullptr},
        {"/api/explore", HTTP_GET, HandleExploreCfg, nullptr},     /* 2026-11-12 探索模式（网页直发） */
        {"/api/explore", HTTP_POST, HandleExploreCfg, nullptr},
        {"/api/speed", HTTP_GET, HandleSpeedSet, nullptr},         /* 2026-11-12 全局速度（下发给从机） */
        {"/api/speed", HTTP_POST, HandleSpeedSet, nullptr},
        {"/api/mem", HTTP_GET, HandleMemInfo, nullptr},            /* 2026-11-13 内存账单 */
        {"/api/camstart", HTTP_POST, HandleCamStart, nullptr},     /* 2026-11-12 进图传模式（按需） */
        {"/api/camstop", HTTP_POST, HandleCamStop, nullptr},       /* 2026-11-12 退图传模式 */
        {"/cam", HTTP_GET, HandleCamPage, nullptr},                /* 2026-11-12 图传页放 80 口，永远能打开 */
    };
    for (const auto& r : routes) {
        /* 必须检查：超过 max_uri_handlers 时这里返回 HANDLERS_FULL，
           以前忽略返回值 → 新接口静默 404，极难查。 */
        esp_err_t err = httpd_register_uri_handler(server, &r);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "路由注册失败 %s: %s", r.uri, esp_err_to_name(err));
        }
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
    /* ⚠️ 2026-11-12：图传（81 口）**不在这里起**。它要 6KB 内部 RAM，
       常驻会把 mbedTLS 挤死（实测 esp-aes 分配失败 → WS 发不出去 → 连不上服务器）。
       改为用户点「图传遥控」时走 /api/camstart 按需启动，退出即关（见 HandleCamStart）。
       这里只挂上"页面关了自动退出"的回调。 */
    SetCamIdleCallback(CamModeExit);
    return true;
}

}  // namespace dafeiyu
