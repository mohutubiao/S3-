#include "settings_web_server.h"

#include <WebServer.h>
#include <WiFi.h>

#include "device_identity.h"
#include "espnow_host.h"         /* 网页手柄配对/状态 */
#include "rom_web_server.h"
#include "wallpaper_web_server.h"
#include "wifi_provisioning.h"   /* 网页配网：保存凭证 + 重连 */

namespace {
WebServer server(80);
SettingsWebServerStatus status;
SettingsWebJsonProvider jsonProvider = nullptr;
SettingsWebApplyHandler applyHandler = nullptr;
bool routesRegistered = false;
bool startedFallbackAp = false;

/* 网页配网的延迟重连：先回 HTTP 响应，800ms 后再断网重连，否则手机会收不到回包 */
String pendingWifiSsid;
String pendingWifiPass;
uint32_t pendingWifiAtMs = 0;

/* 网页遥控虚拟按键：位定义同 serial_controller.h 的 DIJI_BTN_* */
uint8_t virtualButtonMask = 0;
uint32_t virtualButtonUntilMs = 0;

const char kSettingsHtml[] PROGMEM = R"SETTINGS_HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>GameBox 设置</title><style>
*{box-sizing:border-box}body{margin:0;background:#07111f;color:#dcecff;font-family:system-ui,-apple-system,sans-serif}.wrap{max-width:760px;margin:auto;padding:18px}h1{color:#48d7ff;font-size:24px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:14px}.card{display:block;background:#102033;border:1px solid #294760;border-radius:14px;padding:16px;color:inherit;text-decoration:none}.link:hover{border-color:#48d7ff}.link h2{color:#48d7ff}.setting{display:grid;grid-template-columns:1fr 130px;align-items:center;gap:12px;padding:10px 0;border-top:1px solid #294052}select,input{width:100%;font:inherit;background:#07111f;color:#fff;border:1px solid #39799d;border-radius:8px;padding:8px}.btn{font:inherit;background:#0d6efd;color:#fff;border:0;border-radius:8px;padding:10px 14px;margin:6px 6px 0 0;cursor:pointer}.btn.alt{background:#28465f}.btn:active{transform:scale(.96)}.pad{display:flex;flex-wrap:wrap}.muted{color:#8fa9bd;font-size:13px}#status{min-height:24px;color:#62efb1}#wifiState{color:#62efb1}@media(max-width:480px){.setting{grid-template-columns:1fr}.wrap{padding:10px}}
</style></head><body><main class="wrap"><h1>GameBox 设置中心</h1><div class="grid">
<section class="card"><h2>设备设置</h2><div class="setting"><label>音量</label><input id="volume" type="range" min="0" max="100" step="10"></div><div class="setting"><label id="displayLabel">显示模式</label><select id="display"><option id="displayFull" value="1">全屏</option><option id="displayNative" value="0">原始比例</option></select></div><div class="setting"><label>屏幕旋转</label><select id="rotation"><option value="0">默认方向</option><option value="1">旋转 180°</option></select></div><div class="setting" id="invertRow"><label>屏幕反色</label><select id="invert"><option value="1">开启</option><option value="0">关闭</option></select></div><div class="setting"><label>开机主页</label><select id="boot"><option value="1">壁纸时钟</option><option value="0">游戏列表</option></select></div><div class="setting"><label>倒带时长</label><select id="rewind"><option value="0">关闭</option><option value="5">5 秒</option><option value="10">10 秒</option><option value="20">20 秒</option></select></div><div class="setting"><label>自动关机</label><select id="autooff"><option value="0">关闭</option><option value="5">5 分钟</option><option value="10">10 分钟</option><option value="20">20 分钟</option></select></div><div id="status"></div></section>
<section class="card"><h2>网页遥控</h2><p class="muted">点下面的键＝按掌机上对应的键。没有手柄时用它选底部那排（返回/设置/商店/开始/电源）。</p><div class="pad"><button class="btn" data-btn="up">↑ 上</button><button class="btn" data-btn="down">↓ 下</button><button class="btn" data-btn="left">← 左</button><button class="btn" data-btn="right">→ 右</button></div><div class="pad"><button class="btn" data-btn="a">A 确认</button><button class="btn" data-btn="b">B 返回</button><button class="btn alt" data-btn="select">SELECT 切底排</button><button class="btn alt" data-btn="start">START</button></div><div id="inputMsg" class="muted" style="margin-top:8px"></div></section>
<section class="card"><h2>游戏手柄</h2><div class="setting"><label>P1 状态</label><span id="pad1" class="muted">读取中…</span></div><div class="setting"><label>P2 状态</label><span id="pad2" class="muted">读取中…</span></div><div class="setting"><label>实时按键</label><span id="padBtn" class="muted">—</span></div><div><button class="btn" id="pair1">配对 P1</button><button class="btn" id="pair2">配对 P2</button><button class="btn alt" id="padRefresh">刷新</button></div><div id="padMsg" class="muted" style="margin-top:8px">配对时：先点按钮，再在手柄上同时长按 SELECT + START。</div></section>
<section class="card"><h2>WiFi 配网</h2><div class="setting"><label>当前连接</label><span id="wifiState" class="muted">读取中…</span></div><div class="setting"><label>选择附近网络</label><select id="wifiList"><option value="">点「扫描」获取列表</option></select></div><div class="setting"><label>WiFi 名称</label><input id="wifiSsid" placeholder="选择上方网络或手动输入" autocomplete="off"></div><div class="setting"><label>WiFi 密码</label><input id="wifiPass" type="password" placeholder="开放网络留空" autocomplete="new-password"></div><div><button class="btn" id="wifiSave">保存并连接</button><button class="btn alt" id="wifiScan">扫描网络</button></div><div id="wifiMsg" class="muted" style="margin-top:8px"></div></section>
<a class="card link" href="/wallpaper"><h2>壁纸管理</h2><p>上传、裁切、选择和删除 SD 卡壁纸。</p></a><a class="card link" href="/roms"><h2>ROM 管理</h2><p>批量上传、浏览和删除 SD 卡中的 NES ROM。</p></a></div><p class="muted">此网页在 GameBox 联网后自动开启；连不上 WiFi 时开热点 GameBox-Settings-xxxx，手机连它访问 192.168.4.1 即可配网。</p></main><script>
const ids=['volume','display','rotation','invert','boot','rewind','autooff'],statusEl=document.getElementById('status');let loading=true;
async function load(){try{const r=await fetch('/api/device-settings');const j=await r.json();if(!r.ok||!j.ok)throw Error(j.message||'读取失败');ids.forEach(k=>document.getElementById(k).value=String(j[k]));document.getElementById('invertRow').style.display=j.st7789?'grid':'none';if(j.original_s3){document.getElementById('displayLabel').textContent='游戏分辨率';document.getElementById('displayFull').textContent='320×240 全屏';document.getElementById('displayNative').textContent='256×240 原生'}loading=false}catch(e){statusEl.textContent=e.message}}
async function save(key,value){if(loading)return;try{statusEl.textContent='正在保存...';const r=await fetch('/api/device-settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'key='+encodeURIComponent(key)+'&value='+encodeURIComponent(value)});const j=await r.json();if(!r.ok||!j.ok)throw Error(j.message||'保存失败');statusEl.textContent=j.message}catch(e){statusEl.textContent=e.message}}
ids.filter(k=>k!=='volume').forEach(k=>document.getElementById(k).onchange=e=>save(k,e.target.value));let volumeTimer;document.getElementById('volume').oninput=e=>{clearTimeout(volumeTimer);volumeTimer=setTimeout(()=>save('volume',e.target.value),180)};load();
const wifiMsg=document.getElementById('wifiMsg'),wifiState=document.getElementById('wifiState'),wifiList=document.getElementById('wifiList');
async function wifiStatus(){try{const j=await (await fetch('/api/wifi-status')).json();wifiState.textContent=j.connected?('已连接 '+j.ssid+'  '+j.ip):(j.ap?('未联网，热点 '+j.ap+' 已开'):'未连接')}catch(e){wifiState.textContent='读取失败'}}
async function wifiScan(){wifiMsg.textContent='扫描中（约 3 秒）...';try{const j=await (await fetch('/api/wifi-scan')).json();if(!j.ok)throw Error(j.message||'扫描失败');wifiList.innerHTML='<option value="">请选择（或手动输入）</option>'+j.networks.map(n=>'<option value="'+n.ssid.replace(/"/g,'&quot;')+'">'+n.ssid+'  ('+n.rssi+'dBm)'+(n.saved?' ✓已存':'')+'</option>').join('');wifiMsg.textContent='发现 '+j.networks.length+' 个网络'}catch(e){wifiMsg.textContent=e.message}}
wifiList.onchange=e=>{if(e.target.value)document.getElementById('wifiSsid').value=e.target.value};
document.getElementById('wifiScan').onclick=wifiScan;
document.getElementById('wifiSave').onclick=async()=>{const ssid=document.getElementById('wifiSsid').value.trim(),pass=document.getElementById('wifiPass').value;if(!ssid){wifiMsg.textContent='请先填 WiFi 名称';return}wifiMsg.textContent='正在保存...';try{const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)});const j=await r.json();if(!r.ok||!j.ok)throw Error(j.message||'保存失败');wifiMsg.textContent=j.message+'，稍后若 IP 变化请重新访问新地址'}catch(e){wifiMsg.textContent=e.message}};
wifiStatus();wifiScan();
const inputMsg=document.getElementById('inputMsg');
document.querySelectorAll('[data-btn]').forEach(el=>{el.onclick=async()=>{const b=el.dataset.btn;try{const r=await fetch('/api/input?btn='+b+'&ms=260');const j=await r.json();inputMsg.textContent=j.ok?('已发送 '+b):(j.message||'失败')}catch(e){inputMsg.textContent=e.message}}});
const pad1=document.getElementById('pad1'),pad2=document.getElementById('pad2'),padBtn=document.getElementById('padBtn'),padMsg=document.getElementById('padMsg');
const btnNames=[[0x01,'A'],[0x02,'B'],[0x04,'SELECT'],[0x08,'START'],[0x10,'上'],[0x20,'下'],[0x40,'左'],[0x80,'右']];
function padText(p){if(!p.paired)return '<span style="color:#f88">未配对</span>';return (p.connected?'<span style="color:#62efb1">已连接</span>':'<span style="color:#ffd166">已配对·离线</span>')+' '+p.mac+(p.battery!=null?' 电量'+p.battery+'%':'')}
async function padStatus(){try{const j=await (await fetch('/api/gamepad-status')).json();if(!j.ok)throw Error(j.message||'读取失败');pad1.innerHTML=padText(j.players[0]);pad2.innerHTML=padText(j.players[1]);const b=j.players[0].buttons;const names=btnNames.filter(x=>b&x[0]).map(x=>x[1]);padBtn.textContent='0x'+b.toString(16).toUpperCase().padStart(2,'0')+(names.length?' ('+names.join('+')+')':'');if(j.pairing.active){padMsg.textContent='P'+j.pairing.player+' 配对中… 请长按手柄 SELECT+START';}else if(j.pairing.succeeded){padMsg.textContent='P'+j.pairing.player+' 配对成功 ✓';} }catch(e){padMsg.textContent=e.message}}
async function pair(p){padMsg.textContent='正在打开 P'+p+' 配对窗口...';try{const r=await fetch('/api/gamepad-pair',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'player='+p+'&seconds=20'});const j=await r.json();if(!r.ok||!j.ok)throw Error(j.message||'失败');padMsg.textContent=j.message}catch(e){padMsg.textContent=e.message}padStatus()}
document.getElementById('pair1').onclick=()=>pair(1);document.getElementById('pair2').onclick=()=>pair(2);document.getElementById('padRefresh').onclick=padStatus;
padStatus();setInterval(padStatus,2000);
</script></body></html>)SETTINGS_HTML";

String jsonEscape(const String& input) {
    String output;
    output.reserve(input.length() + 8);
    for (size_t i = 0; i < input.length(); i++) {
        const char c = input[i];
        if (c == '"' || c == '\\') {
            output += '\\';
            output += c;
        } else if ((uint8_t)c >= 0x20) {
            output += c;
        }
    }
    return output;
}

void sendResult(bool ok, const String& message, int code = 200) {
    const String body = String("{\"ok\":") + (ok ? "true" : "false") +
                        ",\"message\":\"" + jsonEscape(message) + "\"}";
    server.send(code, "application/json; charset=utf-8", body);
}

void registerRoutes() {
    if (routesRegistered) return;
    server.on("/", HTTP_GET, []() {
        server.send_P(200, "text/html; charset=utf-8", kSettingsHtml);
    });
    server.on("/api/device-settings", HTTP_GET, []() {
        if (!jsonProvider) {
            sendResult(false, "设置接口未就绪", 503);
            return;
        }
        server.send(200, "application/json; charset=utf-8", jsonProvider());
    });
    server.on("/api/device-settings", HTTP_POST, []() {
        if (!applyHandler) {
            sendResult(false, "设置接口未就绪", 503);
            return;
        }
        String error;
        if (!applyHandler(server.arg("key"), server.arg("value"), error)) {
            sendResult(false, error, 400);
            return;
        }
        sendResult(true, "设置已保存");
    });
    /* ---- 网页配网（2026-11-05）：屏幕按键不好使时的唯一通路 ---- */
    server.on("/api/wifi-status", HTTP_GET, []() {
        const bool connected = WiFi.status() == WL_CONNECTED;
        String body = "{\"ok\":true,\"connected\":";
        body += connected ? "true" : "false";
        body += ",\"ssid\":\"" + jsonEscape(connected ? WiFi.SSID() : String("")) + "\"";
        body += ",\"ip\":\"" + jsonEscape(connected ? WiFi.localIP().toString() : String("")) + "\"";
        /* 设置网页在离线时会自开热点，这里一并告诉页面 */
        body += ",\"ap\":\"" + jsonEscape(status.accessPoint ? status.ssid : String("")) + "\"}";
        server.send(200, "application/json; charset=utf-8", body);
    });

    server.on("/api/wifi-scan", HTTP_GET, []() {
        /* 同步扫描约 2~4 秒；WebServer 是单线程，扫完才能回包，页面有"扫描中"提示 */
        const int count = WiFi.scanNetworks(false, true);
        String body = "{\"ok\":true,\"networks\":[";
        int emitted = 0;
        for (int i = 0; i < count && emitted < 20; i++) {
            const String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) continue;   /* 隐藏网络 */
            char savedPass[64] = {0};
            const bool saved = wifiProvisioningGetCredentialsForSsid(ssid.c_str(), savedPass,
                                                                     sizeof(savedPass));
            if (emitted > 0) body += ",";
            body += "{\"ssid\":\"" + jsonEscape(ssid) + "\",\"rssi\":" +
                    String(WiFi.RSSI(i)) + ",\"saved\":" + (saved ? "true" : "false") + "}";
            emitted++;
        }
        WiFi.scanDelete();
        body += "]}";
        server.send(200, "application/json; charset=utf-8", body);
    });

    server.on("/api/wifi", HTTP_POST, []() {
        const String ssid = server.arg("ssid");
        const String pass = server.arg("pass");
        if (ssid.length() == 0) {
            sendResult(false, "WiFi 名称不能为空", 400);
            return;
        }
        /* 凭证先落盘（这步不会断网），真正的 WiFi.begin 延迟 800ms 执行，
           否则响应还没发出去连接就被掐断了。 */
        if (!wifiProvisioningSaveCredentialsOnly(ssid.c_str(), pass.c_str())) {
            sendResult(false, "保存失败（密码需 ≥8 位或留空）", 400);
            return;
        }
        pendingWifiSsid = ssid;
        pendingWifiPass = pass;
        pendingWifiAtMs = millis() + 800;
        sendResult(true, "已保存，正在连接 " + ssid);
    });

    /* ---- 网页手柄配对（2026-11-05）：屏上「游戏手柄」菜单够不着时的备用通路 ---- */
    server.on("/api/gamepad-status", HTTP_GET, []() {
        String body = "{\"ok\":true,\"pairing\":{\"active\":";
        body += espNowHostPairingActive() ? "true" : "false";
        body += ",\"succeeded\":";
        body += espNowHostPairingSucceeded() ? "true" : "false";
        body += ",\"player\":";
        body += String((int)espNowHostPairingPlayer() + 1);
        body += "},\"players\":[";
        for (uint8_t p = 0; p < 2; p++) {
            if (p > 0) body += ",";
            char mac[24] = {0};
            const bool paired = espNowHostHasPairedController(p);
            const bool connected = espNowHostControllerConnected(p);
            uint8_t battery = 0;
            const bool hasBattery = espNowHostGetBatteryPercent(p, battery);
            if (paired) espNowHostGetPairedMacString(p, mac, sizeof(mac));
            body += "{\"player\":" + String((int)p + 1);
            body += ",\"paired\":" + String(paired ? "true" : "false");
            body += ",\"connected\":" + String(connected ? "true" : "false");
            body += ",\"mac\":\"" + jsonEscape(String(mac)) + "\"";
            body += ",\"buttons\":" + String((int)espNowHostGetControllerState(p));
            body += ",\"battery\":";
            body += hasBattery ? String((int)battery) : String("null");
            body += "}";
        }
        body += "]}";
        server.send(200, "application/json; charset=utf-8", body);
    });

    server.on("/api/gamepad-pair", HTTP_POST, []() {
        int player = server.arg("player").toInt();
        int seconds = server.arg("seconds").toInt();
        if (player < 1 || player > 2) player = 1;
        if (seconds < 5) seconds = 20;
        if (seconds > 60) seconds = 60;
        espNowHostStartPairing((uint8_t)(player - 1), (uint32_t)seconds * 1000);
        Serial.printf("设置网页 手柄配对窗口已开：P%d %d 秒\n", player, seconds);
        sendResult(true, String("P") + player + " 配对中（" + seconds +
                                 " 秒），请同时长按手柄 SELECT + START");
    });

    /* ---- 网页遥控（2026-11-05）：没有手柄/实体键时的唯一操作入口 ----
       GET /api/input?btn=up|down|left|right|a|b|select|start&ms=250 */
    server.on("/api/input", HTTP_GET, []() {
        const String btn = server.arg("btn");
        int ms = server.arg("ms").toInt();
        if (ms <= 0) ms = 250;
        if (ms > 3000) ms = 3000;
        uint8_t bit = 0;
        if (btn == "a") bit = 0x01;
        else if (btn == "b") bit = 0x02;
        else if (btn == "select") bit = 0x04;
        else if (btn == "start") bit = 0x08;
        else if (btn == "up") bit = 0x10;
        else if (btn == "down") bit = 0x20;
        else if (btn == "left") bit = 0x40;
        else if (btn == "right") bit = 0x80;
        if (bit == 0) {
            sendResult(false, "未知按键", 400);
            return;
        }
        virtualButtonMask = bit;
        virtualButtonUntilMs = millis() + (uint32_t)ms;
        sendResult(true, btn + " 按下 " + String(ms) + "ms");
    });

    wallpaperWebServerRegisterRoutes(server);
    romWebServerRegisterRoutes(server);
    server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
    routesRegistered = true;
}
}  // namespace

uint8_t settingsWebVirtualButtons() {
    if (virtualButtonUntilMs == 0) return 0;
    if ((int32_t)(millis() - virtualButtonUntilMs) > 0) {
        virtualButtonMask = 0;
        virtualButtonUntilMs = 0;
        return 0;
    }
    return virtualButtonMask;
}

bool settingsWebServerBegin(bool sdCardAvailable,
                            SettingsWebJsonProvider provider,
                            SettingsWebApplyHandler handler) {
    settingsWebServerStop();
    jsonProvider = provider;
    applyHandler = handler;
    status = SettingsWebServerStatus{};
    wallpaperWebServerPrepare(sdCardAvailable);
    romWebServerPrepare(sdCardAvailable);

    if (WiFi.status() == WL_CONNECTED) {
        status.url = String("http://") + WiFi.localIP().toString() + "/";
    } else {
        String id = gameBoxDeviceId();
        String suffix = id.length() > 6 ? id.substring(id.length() - 6) : id;
        status.ssid = String("GameBox-Settings-") + suffix;
        WiFi.mode(WIFI_AP_STA);
        if (!WiFi.softAP(status.ssid.c_str())) {
            status.message = "无法启动设置热点";
            return false;
        }
        startedFallbackAp = true;
        status.accessPoint = true;
        status.url = String("http://") + WiFi.softAPIP().toString() + "/";
    }

    /* 网页服务会切 WiFi 模式（AP_STA），模式切换会把 modem sleep 打开。
       ESP-NOW 接收靠 WiFi 射频实时在线，省电模式会让手柄包延迟甚至丢失，
       表现就是网页一开手柄就「断断续续」——这里强制关掉。 */
    WiFi.setSleep(false);

    registerRoutes();
    server.begin();
    status.active = true;
    status.message = "设置网页已启动";
    Serial.printf("设置网页已启动：%s%s%s\n", status.url.c_str(),
                  status.accessPoint ? " 热点=" : "",
                  status.accessPoint ? status.ssid.c_str() : "");
    return true;
}

void settingsWebServerLoop() {
    if (status.active) server.handleClient();

    /* 网页触发的配对窗口：无论当前停在哪个界面（主循环/设置菜单都会调这里），
       保证配对包有被处理，否则 PairAck 发不出去。只在配对窗口开启时跑。 */
    if (espNowHostPairingActive()) espNowHostUpdate();

    /* 网页配网：保存后延迟 800ms 才真正断网重连，保证刚才的 HTTP 回包能发完 */
    if (pendingWifiAtMs != 0 && (int32_t)(millis() - pendingWifiAtMs) >= 0) {
        const String ssid = pendingWifiSsid;
        const String pass = pendingWifiPass;
        pendingWifiAtMs = 0;
        pendingWifiSsid = "";
        pendingWifiPass = "";
        Serial.printf("设置网页 开始连接 WiFi：SSID=%s\n", ssid.c_str());
        WiFi.disconnect(false, true);
        WiFi.begin(ssid.c_str(), pass.c_str());
    }
}

void settingsWebServerStop() {
    if (status.active) server.stop();
    wallpaperWebServerCleanup();
    romWebServerCleanup();
    if (startedFallbackAp) {
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
    }
    startedFallbackAp = false;
    status.active = false;
}

SettingsWebServerStatus settingsWebServerStatus() {
    return status;
}
