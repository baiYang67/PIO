#pragma once
/**
 * @file    OtaManager.h
 * @brief   OTA 升级管理（双通道：ArduinoOTA + HTTP OTA）
 *
 * 两条升级通道
 * ------------------------------------------------------------
 *  1. ArduinoOTA（局域网）
 *     PlatformIO: pio run -t upload --upload-port <设备IP>
 *     或 Arduino IDE 通过网络端口上传；带密码保护（"ota_pass"）。
 *  2. HTTP OTA（远程）
 *     MQTT 下发 {"url":"http://server/firmware.bin"} 到 ota topic，
 *     设备异步下载并写入备用分区，成功后自动重启切到新固件。
 *
 * 线程模型（重要）
 * ------------------------------------------------------------
 *  - requestHttpOta() 由 MQTT 回调（MqttUploadTask 上下文）调用，
 *    只写 URL + 给信号量，立即返回，绝不阻塞 MQTT 保活。
 *  - 真正的下载与重启在 OtaTask（优先级 2）中的 loop() 里执行。
 *  - 二进制信号量最大计数为 1：连续下发多个 URL 时，后一个会覆盖前一个
 *    （只保留最后一次请求），这是有意为之——避免命令堆积。
 *
 * @warning HTTP OTA 期间会长时间占用网络与 Flash，请确保 WDT_TIMEOUT
 *          足够大（当前 30s 且下载循环中未喂狗，超大固件可能触发看门狗复位）。
 */
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class OtaManager {
public:
    /**
     * @brief 启动 ArduinoOTA 服务并创建 OTA 请求信号量
     * @param hostname 设备网络名（ArduinoOTA 以此识别设备）
     * @param password OTA 密码，防止局域网内被随意刷机
     */
    void begin(const char* hostname, const char* password);

    /** @brief 由 OtaTask 周期调用：处理局域网 OTA + 检查 HTTP OTA 请求 */
    void loop();

    /**
     * @brief 请求一次 HTTP OTA（非阻塞，可在 MQTT 回调中安全调用）
     * @param url 固件二进制 HTTP 地址
     */
    void requestHttpOta(const char* url);

private:
    /**
     * @brief 实际的下载 + 写入 + 重启流程（阻塞，仅在 OtaTask 中调用）
     * @param url 固件地址
     */
    void doHttpOta(const String& url);

    SemaphoreHandle_t otaSem_ = nullptr;   // 二值信号量：1=有待处理的 OTA 请求
    String            otaUrl_;             // 待下载地址（与信号量配合使用）
};
