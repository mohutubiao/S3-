#pragma once

#include <Arduino.h>

enum class WifiProvisioningState {
    Idle,
    PortalRunning,
    Connecting,
    Connected,
    Failed,
};

struct WifiProvisioningStatus {
    WifiProvisioningState state = WifiProvisioningState::Idle;
    String apSsid;
    String selectedSsid;
    String ip;
    String message;
};

void wifiProvisioningBegin();
void wifiProvisioningLoop();
void wifiProvisioningStop();
bool wifiProvisioningActive();
bool wifiProvisioningFinished();
bool wifiProvisioningConnected();
WifiProvisioningStatus wifiProvisioningStatus();
bool wifiProvisioningHasSavedConfig();
int wifiProvisioningSavedNetworkCount();
bool wifiProvisioningGetSavedSsid(char* out, size_t outSize);
bool wifiProvisioningGetSavedCredentials(char* ssidOut, size_t ssidSize,
                                         char* passOut, size_t passSize);
bool wifiProvisioningGetLastSuccessfulCredentials(char* ssidOut, size_t ssidSize,
                                                  char* passOut, size_t passSize);
bool wifiProvisioningGetCredentialsForSsid(const char* ssid,
                                           char* passOut, size_t passSize);
void wifiProvisioningRememberSuccessfulSsid(const char* ssid);

/* 网页配网（2026-11-05）：把用户在浏览器里填的 SSID/密码直接存进 NVS 并立刻
   发起连接，不需要进屏上的配网菜单、也不需要连配网热点。返回 false 表示保存失败。 */
bool wifiProvisioningApplyManualCredentials(const char* ssid, const char* pass);

/* 只保存不重连（HTTP 响应要先发出去，连接随后由调用方延迟发起）。
   密码留空表示开放网络；非空但少于 8 位直接拒绝（WPA2 最短 8）。 */
bool wifiProvisioningSaveCredentialsOnly(const char* ssid, const char* pass);
bool wifiProvisioningGetMatchingSavedCredentials(char* ssidOut, size_t ssidSize,
                                                char* passOut, size_t passSize);
bool wifiProvisioningGetBestSavedCredentialsFromScan(int scanCount,
                                                     char* ssidOut, size_t ssidSize,
                                                     char* passOut, size_t passSize,
                                                     int* rssiOut = nullptr);
