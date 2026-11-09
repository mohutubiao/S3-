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

	mcp_server.AddTool(
        "self.AEC.set_mode", 
        "设置AEC对话打断模式。当用户意图切换对话打断模式时或者用户觉得ai对话容易被打断时或者用户觉得无法实现对话打断时都使用此工具。\n"
        "参数：\n"
        "   `mode`: 对话打断模式，可选值只有`kAecOff`(关闭）和`kAecOnDeviceSide`（开启）\n"
        "返回值：\n"
        "   反馈状态信息，不需要确认，立即播报相关数据\n",
        PropertyList({
            Property("mode", kPropertyTypeString)
        }), 
        [](const PropertyList& properties) -> ReturnValue {
            auto mode = properties["mode"].value<std::string>();
            auto& app = Application::GetInstance();
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (mode == "kAecOff") {
                app.SetAecMode(kAecOff);
                return "{\"success\": true, \"message\": \"AEC对话打断模式已关闭\"}";
            }else {
                auto& board = Board::GetInstance();
                app.SetAecMode(kAecOnDeviceSide);
                
                return "{\"success\": true, \"message\": \"AEC对话打断模式已开启\"}";
            }
        }
    );

    mcp_server.AddTool(
        "self.AEC.get_mode",
        "获取AEC对话打断模式状态。当用户意图获取对话打断模式状态时使用此工具。\n"
        "返回值：\n"
        "   反馈状态信息，不需要确认，立即播报相关数据\n",
        PropertyList(),  
        [](const PropertyList&) -> ReturnValue {
            auto& app = Application::GetInstance();
            const bool is_currently_off = (app.GetAecMode() == kAecOff);
           if (is_currently_off) {
                return "{\"success\": true, \"message\": \"AEC对话打断模式处于关闭状态\"}";
            }else {
                return "{\"success\": true, \"message\": \"AEC对话打断模式处于开启状态\"}";
            }
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

    mcp_server.AddTool(
        "self.led.control",
        "控制板载 WS2812 彩灯：打开、关闭、换颜色、开关音乐律动。"
        "用户说「开灯」「关灯」「把灯变成红色」「彩灯跟着音乐律动」「关掉律动」时使用。\n"
        "参数：\n"
        "   `action`: 必填，on(打开) / off(关闭) / color(换颜色) / dance_on(开律动) / dance_off(关律动)\n"
        "   `color`: action 为 color 时填，如 #FF0000 或英文 red；其余 action 一律传空字符串\n"
        "返回值：\n"
        "   反馈状态信息，不需要确认，立即播报相关数据\n",
        PropertyList({
            Property("action", kPropertyTypeString),
            Property("color", kPropertyTypeString),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto* led = Ws2812Instance();
            if (led == nullptr) {
                return "{\"success\": false, \"message\": \"彩灯未初始化\"}";
            }
            const auto action = properties["action"].value<std::string>();
            if (action == "on") {
                /* 空颜色 + 亮度 -1 = 保持用户上次设的颜色和亮度，只把灯点亮 */
                led->Set("", -1, "on");
                return "{\"success\": true, \"message\": \"彩灯已打开\"}";
            }
            if (action == "off") {
                led->SetDanceMode(false);   // 律动开着时直接关灯会前功尽弃，一并关掉
                led->Set("", -1, "off");
                return "{\"success\": true, \"message\": \"彩灯已关闭\"}";
            }
            if (action == "color") {
                const auto color = properties["color"].value<std::string>();
                if (color.empty()) {
                    return "{\"success\": false, \"message\": \"没收到颜色，请用户再说一次\"}";
                }
                if (!led->Set(color, -1, "on")) {
                    return "{\"success\": false, \"message\": \"这个颜色我不认识，请换个说法，比如 #FF0000\"}";
                }
                return "{\"success\": true, \"message\": \"彩灯颜色已更换\"}";
            }
            if (action == "dance_on") {
                led->Set("", -1, "on");     // 灯得先亮着，律动才有东西可跳
                led->SetDanceMode(true);
                return "{\"success\": true, \"message\": \"彩灯律动已打开，灯会跟着音乐和人声跳\"}";
            }
            if (action == "dance_off") {
                led->SetDanceMode(false);
                return "{\"success\": true, \"message\": \"彩灯律动已关闭\"}";
            }
            return "{\"success\": false, \"message\": \"不认识这个彩灯动作\"}";
        }
    );

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