/**
 * @file    WifiManager.cpp
 * @brief   WiFi 连接 / 重连实现
 */
#include "WifiManager.h"
#include <WiFi.h>

void WifiManager::begin(const char* ssid, const char* pwd, const char* hostname) {
    // true = 同时关闭已保存的配置并断开，确保从干净状态开始连接
    WiFi.disconnect(true);
    // STA 模式（设备作为客户端连接路由器），不开启 AP
    WiFi.mode(WIFI_STA);
    // 最小 Modem 睡眠：省电且兼容性较好；
    // 若现场出现频繁掉线/RTT 抖动，可改为 WIFI_PS_NONE（更耗电但更稳定）
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    WiFi.hostname(hostname);        // 影响 DHCP 主机名与 mDNS
    WiFi.begin(ssid, pwd);

    Serial.printf("[WiFi] 连接 %s", ssid);
    uint32_t t0 = millis();
    // 阻塞等待联网：期间每 500ms 打一个点，方便现场判断是"在连"还是"卡死"
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 > 120000) {          // 120s 超时
            Serial.println("\n[WiFi] 超时，重启设备");
            ESP.restart();                     // 重启后重新走一遍初始化
        }
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\n[WiFi] 已连接, IP=%s\n", WiFi.localIP().toString().c_str());
}

void WifiManager::checkReconnect() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] 连接丢失，尝试重连...");
        // 异步重连：使用 NVS 中保存的 SSID/密码
        WiFi.reconnect();
    }
}

bool WifiManager::connected() {
    return WiFi.status() == WL_CONNECTED;
}
