#pragma once

#include <Arduino.h>

using SettingsWebJsonProvider = String (*)();
using SettingsWebApplyHandler = bool (*)(const String& key, const String& value,
                                         String& error);

struct SettingsWebServerStatus {
    bool active = false;
    bool accessPoint = false;
    String ssid;
    String url;
    String message;
};

bool settingsWebServerBegin(bool sdCardAvailable,
                            SettingsWebJsonProvider jsonProvider,
                            SettingsWebApplyHandler applyHandler);
void settingsWebServerLoop();
void settingsWebServerStop();
SettingsWebServerStatus settingsWebServerStatus();

/* 网页遥控（2026-11-05）：浏览器按下虚拟键后，按键位保持一段时间。
   位定义与 serial_controller.h 的 DIJI_BTN_* 一致，updateButtons() 会把它
   OR 进实体按键状态里 —— 没有手柄/实体键也能操作菜单底排。 */
uint8_t settingsWebVirtualButtons();
