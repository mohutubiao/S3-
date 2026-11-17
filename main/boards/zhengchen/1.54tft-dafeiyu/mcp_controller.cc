#include <cJSON.h>
#include <esp_log.h>
#include <wifi_manager.h>

#include <cstring>
#include <string>

#include "application.h"
#include "board.h"
#include "config.h"
#include "mcp_server.h"
#include "sdkconfig.h"
#include "settings.h"
#include "display.h"
#include "esp_now_link.h" // self.car.distance：距离保护阈值下发（ESP-NOW → C3 从机）
#include "ws2812_led.h"   // self.led.control：板载彩灯（语音开关/颜色/律动）

#define TAG "MCPController"

class MCPController {
public:
    MCPController() {
        RegisterMcpTools();
        ESP_LOGI(TAG, "注册MCP工具");
    }

	void RegisterMcpTools() {
        auto& mcp_server = McpServer::GetInstance();
		ESP_LOGI(TAG, "开始注册MCP工具...");

	/* 2026-11-15 精简：原来的 set_mode / get_mode 合成一个（不传 mode = 查询）。
       少一个工具、少一段描述，服务端 prompt 更短，AI 也不容易选错。 */
	mcp_server.AddTool(
        "self.AEC.mode",
        "查询或设置「语音打断」开关（它同时控制设备端 AEC 回声消除）。"
        "用户说「关掉语音打断」「打开语音打断」「现在打断开着吗」「为什么说话打断不了」时使用。\n"
        "参数：\n"
        "   `mode`: 可选。`kAecOnDeviceSide`=打开，`kAecOff`=关闭；不传 = 只查询状态\n",
        PropertyList({
            Property("mode", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            const std::string mode = properties["mode"].value<std::string>();
            if (mode.empty()) {
                if (app.IsVoiceInterruptEnabled()) {
                    return std::string("{\"success\": true, \"message\": \"语音打断当前是开启的（AEC 开）\"}");
                }
                return std::string("{\"success\": true, \"message\": \"语音打断当前是关闭的（AEC 关）\"}");
            }
            /* 关/开 AEC 会重建音频通道，先等这一句播完再切 */
            vTaskDelay(pdMS_TO_TICKS(2000));
            /* 统一走 SetVoiceInterruptEnabled（内部同步 AEC + NVS），
               否则网页上的打断开关和实际 AEC 状态会分叉。 */
            if (mode == "kAecOff") {
                app.SetVoiceInterruptEnabled(false);
                return std::string("{\"success\": true, \"message\": \"语音打断已关闭（AEC 一并关闭），AI 说话时喊唤醒词不会打断\"}");
            }
            app.SetVoiceInterruptEnabled(true);
            return std::string("{\"success\": true, \"message\": \"语音打断已开启（AEC 一并开启），AI 说话时喊唤醒词可打断\"}");
        }
    );
	
    mcp_server.AddTool(
        "self.device.get_ip",
        "获取设备的 IP 地址和网页控制页地址。当用户问「你的 IP 是多少」「怎么打开控制页」"
        "「网址是什么」时使用此工具。\n"
        "返回：IP、网页控制页完整地址\n",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            auto& wifi = WifiManager::GetInstance();
            std::string ip = wifi.GetIpAddress();
            if (ip.empty()) {
                return "{\"success\": false, \"message\": \"当前没有连上 Wi-Fi，拿不到 IP\"}";
            }
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "{\"success\": true, \"ip\": \"%s\", \"web\": \"http://%s\", "
                     "\"message\": \"我的 IP 是 %s，浏览器打开 http://%s 可以进网页控制页\"}",
                     ip.c_str(), ip.c_str(), ip.c_str(), ip.c_str());
            return std::string(buf);
        }
    );

    /* 2026-11-15 精简：原来的 self.led.control 已删除，能力并入板级的
       self.led.set（颜色/亮度/效果 + 新增 dance 参数），避免两个灯工具
       让 AI 选错、也省一段 prompt。 */

    // ⚠️ 2026-10-31：这个工具【默认不再注册】。
    //
    // 原因：只要注册了，服务端 AI 在任意一轮对话里都可能判定「用户想重启」
    //       并调用它，实际表现就是「一按 BOOT / 一语音唤醒就重启」，设备几乎
    //       没法正常使用。一台随时会被远端_reset 的设备，比没有远程重启能力糟得多。
    //
    // 恢复办法：把下面的 #if 0 改成 #if 1 重新编译即可 —— 但请先确认你能接受
    //       「AI 随时可能把设备重启」这个风险。真需要远程重启的话，更合适的做法
    //       是让工具只返回一个「请求已记录」，由用户在网页上点确认后才真正重启。
#if 0
    mcp_server.AddTool(
        "self.res.esp_restart",
        "重启设备。当用户意图重启设备时使用此工具。\n",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            ESP_LOGW(TAG, "!!! esp_restart 由 MCP 工具调用（服务端请求重启），非崩溃 !!!");
            vTaskDelay(pdMS_TO_TICKS(1000));
            // Reboot the device
            esp_restart();
            return true;
        }
    );
#endif

    /* ── 2026-11-12 距离保护（GY-530 / VL53L0X，装在 C3 从机上）──
       传感器在从机，阈值由主机下发（ESP-NOW 配置包），从机存 NVS。
       单位对外用厘米（用户说话就是「30 厘米」），内部换算成毫米。 */
    mcp_server.AddTool(
        "self.car.distance",
        "设置或查询小车的激光距离保护（GY-530）：防碰撞 + 防跌落。"
        "用户说「碰撞距离设成 30 厘米」「跌落阈值调大」「关掉防撞」「距离保护现在多少」时使用。\n"
        "用户问「刚才为什么停住」「为什么不走了」「是什么拦住你的」「撞到东西了吗」时，"
        "也必须用 action=get：返回里的 last_block 就是最后一次拦截原因（碰撞还是跌落、距离多远、多久之前）。\n"
        "参数：\n"
        "   `action`: get(查询) / set(修改) / on(开启) / off(关闭)\n"
        "   `obstacle_cm`: action=set 时填，小于等于这个距离就判定前方有障碍并拒绝前进，单位厘米（3~200）\n"
        "   `fall_cm`: action=set 时填，大于等于这个距离判定前方悬空/跌落并拒绝前进，单位厘米（8~800）\n"
        "返回值：\n"
        "   当前设置与下发结果，直接播报给用户\n",
        PropertyList({
            Property("action", kPropertyTypeString),
            /* ⚠️ 必须带默认值：value<int>() 是 std::get，属性缺失时 variant 里
               是 bool → bad_variant_access 直接 abort。 */
            Property("obstacle_cm", kPropertyTypeInteger, 0),
            Property("fall_cm", kPropertyTypeInteger, 0),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            const auto action = properties["action"].value<std::string>();

            /* 主机侧记一份，用于 get；从机自己也会存 NVS，双向都不会丢 */
            Settings s("car_guard", true);
            int obs_mm = s.GetInt("obs_mm", 60);
            int fall_mm = s.GetInt("fall_mm", 450);
            bool enabled = s.GetInt("enabled", 1) != 0;

            char buf[768];
            if (action == "get") {
                BlockInfo blk;
                const bool has_blk = EspNowLink::Get().LastBlock(&blk);
                LinkStatus st;
                const bool linked = EspNowLink::Get().Status(&st);
                const char* base = enabled ? "已开启" : "已关闭";

                char link_txt[128];
                if (linked) {
                    snprintf(link_txt, sizeof(link_txt),
                             "从机在线，当前距离 %.1f 厘米，传感器%s",
                             st.mm / 10.0,
                             (st.flags & TLM_FLAG_SENSOR) ? "正常" : "未就绪");
                } else {
                    snprintf(link_txt, sizeof(link_txt), "从机离线，拿不到实时距离");
                }

                char blk_txt[192];
                if (has_blk) {
                    snprintf(blk_txt, sizeof(blk_txt),
                             "；最后一次是%s，距离 %.1f 厘米，%.0f 秒前，本次开机一共拦了 %d 次",
                             blk.type == 2 ? "前方悬空被防跌落拦住"
                                           : "前方有障碍被防碰撞拦住",
                             blk.mm / 10.0, blk.age_ms / 1000.0, blk.count);
                } else {
                    snprintf(blk_txt, sizeof(blk_txt), "；本次开机还没被拦过");
                }

                /* 网页设的值可能被从机「地面基线夹紧」改掉，
                   播报时用从机实际上报的阈值，别让用户以为设的就是跑的 */
                const int live_obs = linked ? st.obstacle_mm : obs_mm;
                const int live_fall = linked ? st.fall_mm : fall_mm;
                snprintf(buf, sizeof(buf),
                         "{\"success\": true, \"enabled\": %s, \"obstacle_cm\": %.1f, "
                         "\"fall_cm\": %.1f, \"obstacle_live_cm\": %.1f, "
                         "\"fall_live_cm\": %.1f, \"linked\": %s, \"distance_cm\": %.1f, "
                         "\"sensor_ok\": %s, \"last_block\": %s, \"block_count\": %d, "
                         "\"message\": \"距离保护%s；从机实际阈值 碰撞 %d 厘米 / 跌落 %d 厘米；%s%s\"}",
                         enabled ? "true" : "false", obs_mm / 10.0, fall_mm / 10.0,
                         live_obs / 10.0, live_fall / 10.0,
                         linked ? "true" : "false", linked ? st.mm / 10.0 : 0.0,
                         (linked && (st.flags & TLM_FLAG_SENSOR)) ? "true" : "false",
                         has_blk ? (blk.type == 2 ? "\"跌落\"" : "\"碰撞\"") : "null",
                         has_blk ? blk.count : 0,
                         base, live_obs / 10, live_fall / 10, link_txt, blk_txt);
                return std::string(buf);
            }
            if (action == "on" || action == "off") {
                enabled = (action == "on");
            } else if (action == "set") {
                int obs_cm = properties["obstacle_cm"].value<int>();
                int fall_cm = properties["fall_cm"].value<int>();
                if (obs_cm > 0) obs_mm = obs_cm * 10;
                if (fall_cm > 0) fall_mm = fall_cm * 10;
            } else {
                return "{\"success\": false, \"message\": \"不认识这个动作，只能是 get/set/on/off\"}";
            }

            if (obs_mm < 30) obs_mm = 30;
            if (obs_mm > 2000) obs_mm = 2000;
            if (fall_mm < obs_mm + 50) fall_mm = obs_mm + 50;
            if (fall_mm > 8000) fall_mm = 8000;

            if (!EspNowLink::Get().SendConfig(obs_mm, fall_mm, enabled)) {
                return "{\"success\": false, \"message\": \"指令没发出去，ESP-NOW 还没启动（要先进入小车/遥控模式，并且连上 WiFi）\"}";
            }

            s.SetInt("obs_mm", obs_mm);
            s.SetInt("fall_mm", fall_mm);
            s.SetInt("enabled", enabled ? 1 : 0);
            s.Commit();   /* ⚠️ 不 Commit 就重启会丢（踩过） */

            snprintf(buf, sizeof(buf),
                     "{\"success\": true, \"enabled\": %s, \"obstacle_cm\": %.1f, "
                     "\"fall_cm\": %.1f, \"message\": \"距离保护%s：障碍 %d 厘米，跌落 %d 厘米\"}",
                     enabled ? "true" : "false", obs_mm / 10.0, fall_mm / 10.0,
                     enabled ? "已开启" : "已关闭", obs_mm / 10, fall_mm / 10);
            return std::string(buf);
        }
    );

    /* ── 2026-11-12 探索模式：从机自主溜达 + 自动避障/防跌落 ──
       逻辑在从机（C3）上跑，主机只发启停命令。 */
    mcp_server.AddTool(
        "self.car.explore",
        "让小车自己探索、随机溜达（遇障自动转向，临边先倒退再转向）。"
        "用户说「去探索一下」「自己走走」「随便转转」「探索五分钟」「别转了/停下来」时使用。\n"
        "参数：\n"
        "   `action`: start(开始) / stop(停止) / status(查询)\n"
        "   `duration_s`: 探索多少秒，默认 120，最长 3600\n"
        "   `speed_pct`: 速度百分比 20~100，默认 100\n"
        "注意：探索期间用户给任何手动指令（网页摇杆/语音控制）都会立即退出探索。\n",
        PropertyList({
            Property("action", kPropertyTypeString),
            Property("duration_s", kPropertyTypeInteger, 0),
            Property("speed_pct", kPropertyTypeInteger, 0),
        }),
        [](const PropertyList& p) -> ReturnValue {
            const auto action = p["action"].value<std::string>();
            int dur = p["duration_s"].value<int>();
            int spd = p["speed_pct"].value<int>();
            if (dur <= 0) dur = 120;
            if (spd <= 0) spd = 100;

            auto& link = EspNowLink::Get();
            char buf[224];

            if (action == "stop") {
                if (!link.SendExplore(0, spd, dur)) {
                    return "{\"success\":false,\"message\":\"指令没发出去，ESP-NOW 没启动\"}";
                }
                return "{\"success\":true,\"message\":\"已停止探索\"}";
            }

            if (action == "status") {
                LinkStatus st;
                const bool linked = link.Status(&st);
                const bool running = linked && ((st.flags & TLM_FLAG_EXPLORE) != 0);
                snprintf(buf, sizeof(buf),
                         "{\"success\":true,\"running\":%s,\"linked\":%s,\"message\":\"%s\"}",
                         running ? "true" : "false", linked ? "true" : "false",
                         running ? "正在探索" : (linked ? "没有在探索" : "从机离线"));
                return std::string(buf);
            }

            if (action != "start") {
                return "{\"success\":false,\"message\":\"action 只能是 start/stop/status\"}";
            }
            if (!link.SendExplore(1, spd, dur)) {
                return "{\"success\":false,\"message\":\"指令没发出去，ESP-NOW 没启动（先进入小车模式并连上 WiFi）\"}";
            }
            snprintf(buf, sizeof(buf),
                     "{\"success\":true,\"message\":\"开始探索 %d 秒，速度 %d%%\"}", dur, spd);
            return std::string(buf);
        }
    );

    /* ── 2026-11-12 语音打断开关 ── */
    mcp_server.AddTool(
        "self.voice.interrupt",
        "打开或关闭「语音打断」：AI 说话时喊它能否立刻打断。"
        "用户说「打开语音打断」「别插话 / 别打断你」「等我说完」「能不能打断你」时使用。\n"
        "参数：\n"
        "   `action`: get(查询) / on(允许打断) / off(不允许打断)\n",
        PropertyList({
            Property("action", kPropertyTypeString),
        }),
        [](const PropertyList& p) -> ReturnValue {
            auto& app = Application::GetInstance();
            const auto action = p["action"].value<std::string>();
            if (action == "on") {
                app.SetVoiceInterruptEnabled(true);
            } else if (action == "off") {
                app.SetVoiceInterruptEnabled(false);
            }
            char buf[192];
            snprintf(buf, sizeof(buf),
                     "{\"success\":true,\"interrupt\":%s,\"message\":\"语音打断%s\"}",
                     app.IsVoiceInterruptEnabled() ? "true" : "false",
                     app.IsVoiceInterruptEnabled()
                         ? "已打开，AI 说话时可以直接打断"
                         : "已关闭，AI 会说完再响应你");
            return std::string(buf);
        }
    );

        ESP_LOGI(TAG, "MCP工具注册完成");
    }

};

static MCPController* g_mcp_controller = nullptr;

void InitializeMCPController() {
    if (g_mcp_controller == nullptr) {
        g_mcp_controller = new MCPController();
        ESP_LOGI(TAG, "注册MCP工具");
    }
}