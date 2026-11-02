/**
 * @file    OtaManager.cpp
 * @brief   ArduinoOTA + HTTP OTA 实现
 *
 * ESP32 采用双分区（OTA_0 / OTA_1）机制：
 *   新固件写入"非运行分区"，重启后 bootloader 根据 otadata 选择新分区启动；
 *   若新固件启动失败（未调用 esp_ota_mark_app_valid 前的崩溃由回滚机制处理），
 *   可回退到旧分区。因此升级过程断电通常不会变砖。
 */
#include "OtaManager.h"
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Update.h>

void OtaManager::begin(const char* hostname, const char* password) {
    // 二值信号量初始为空：表示当前没有待处理的 HTTP OTA 请求
    otaSem_ = xSemaphoreCreateBinary();

    ArduinoOTA.setHostname(hostname);
    ArduinoOTA.setPassword(password);
    // 注意：ArduinoOTA 的 mDNS 与 WiFi 服务应在网络连接成功后启动，
    // 本工程在 WifiManager::begin() 之后调用，顺序见 AppController::begin()。
    ArduinoOTA.begin();
    Serial.println("[OTA] ArduinoOTA 已启动");
}

void OtaManager::loop() {
    // 1) 局域网 OTA：内部非阻塞，需要被周期调用才能响应上传请求
    ArduinoOTA.handle();

    // 2) MQTT 触发的 HTTP OTA
    //    用 0 超时"非阻塞查询"：没有请求就立刻返回，不耽误 ArduinoOTA
    if (otaSem_ && xSemaphoreTake(otaSem_, 0) == pdTRUE) {
        Serial.printf("[OTA] 开始 HTTP OTA: %s\n", otaUrl_.c_str());
        doHttpOta(otaUrl_);
    }
}

void OtaManager::requestHttpOta(const char* url) {
    if (!otaSem_ || !url) return;
    // 只记录地址并置位信号量；真正的下载留给 OtaTask，
    // 保证 MQTT 任务（负责保活）不被长时间阻塞。
    otaUrl_ = url;
    xSemaphoreGive(otaSem_);
    Serial.println("[OTA] 已排队 HTTP OTA 请求");
}

void OtaManager::doHttpOta(const String& url) {
    HTTPClient http;
    http.begin(url);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[OTA] HTTP GET 失败, code=%d\n", code);
        http.end();
        return;
    }

    // Content-Length 必须由服务器提供（chunked 传输不支持 Update.begin）
    int contentLen = http.getSize();
    Serial.printf("[OTA] 固件大小: %d 字节\n", contentLen);

    // 校验固件大小并锁定写 Flash 的句柄；
    // 若镜像大于 OTA 分区可用空间，这里会失败
    if (!Update.begin(contentLen)) {
        Serial.println("[OTA] Update.begin 失败");
        http.end();
        return;
    }

    // 边下载边写入，避免把整包固件缓存到 RAM
    WiFiClient* stream = http.getStreamPtr();
    size_t written = Update.writeStream(*stream);
    Serial.printf("[OTA] 已写入 %u / %d 字节\n", written, contentLen);

    // 写入字节数与声明一致且 Update.end() 校验通过（含镜像校验），才算成功
    if (written == (size_t)contentLen && Update.end()) {
        Serial.println("[OTA] 升级成功，即将重启");
        delay(500);           // 给串口日志一点输出时间
        ESP.restart();        // 重启后 bootloader 切换到新分区
    } else {
        // 失败时仍运行旧固件；重新下发一次 OTA 即可重试
        Serial.printf("[OTA] 升级失败: %s\n", Update.errorString());
    }
    http.end();
}
