#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>

void configureGameBoxTls(WiFiClientSecure& client);

/* 2026-11-05 自建/局域网部署：服务地址是 http:// 时返回 true。
   此时设备认证、用量上报、远程配置、OTA 这些「云端才有」的功能一律跳过，
   否则它们会拿 TLS 去敲明文端口（服务器回 400），白白拖慢界面。 */
bool gameBoxServiceUsesPlainHttp();
// OTA bodies may be redirected to a private OSS hostname with a different CA.
// Only use this for downloads whose size and SHA-256 came from the authenticated
// GameBox API and are verified before the update is committed.
void configureHashedDownloadTls(WiFiClientSecure& client);
void applyGameBoxTimezone();
void startGameBoxNetworkTimeSync();
bool ensureGameBoxNetworkTime(String& error, uint32_t timeoutMs = 10000);
