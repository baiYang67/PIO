/**
 * @file    AppController.cpp
 * @brief   启动编排、任务创建、主循环（WiFi 重连 + LED 心跳）
 *
 * 阅读顺序建议
 * ------------------------------------------------------------
 *   1. begin()  —— 看初始化顺序与依赖关系
 *   2. AppTasks.cpp —— 看四条任务如何协作
 *   3. RingBuffer / MqttManager —— 看数据如何缓存与上报
 */
#include "AppController.h"
#include "Config.h"
#include "I2cBus.h"
#include <time.h>
#include <esp_task_wdt.h>

AppController* AppController::self_ = nullptr;

/* ------------------------------------------------------------
 * 看门狗初始化（兼容 Arduino Core 2.x / 3.x）
 * ------------------------------------------------------------ */
void AppController::initWdt() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    // Core 3.x：通过 config 结构体配置，允许重复调用（reconfigure）
    esp_task_wdt_config_t c = {
        .timeout_ms     = WDT_TIMEOUT * 1000,  // 秒 → 毫秒
        .idle_core_mask = 0,                   // 不监控 idle 任务（单核也无 idle core 可配）
        .trigger_panic  = true                 // 超时直接 panic 重启，而不是只打日志
    };
    esp_task_wdt_reconfigure(&c);
#else
    // Core 2.x：只需超时秒数与是否 panic
    esp_task_wdt_init(WDT_TIMEOUT, true);
#endif
}

/* ------------------------------------------------------------
 * NTP 同步（最多 15 秒），失败仅告警不阻塞
 * ------------------------------------------------------------ */
void AppController::syncTimeOnce() {
    // 配置时区与最多 3 个 NTP 服务器（按数组顺序尝试）
    configTime(GMT_Offset, Daylight_Offset,
               ntpServers[0], ntpServers[1], ntpServers[2]);

    struct tm tm;
    // getLocalTime 会阻塞等待同步结果，第二个参数是超时时间(ms)
    if (getLocalTime(&tm, 15000)) {
        Serial.printf("[NTP] 时间已同步: %04d-%02d-%02d %02d:%02d:%02d\n",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        // 同步失败也继续运行：采样照常进行，只是 timestamp 会写 0
        Serial.println("[NTP] 同步失败，时间戳将为 0（数据无时间）");
    }
}

void AppController::initLed() {
    // 先拉低，避免上电瞬间灯亮一下/闪烁
    pinMode(Pins::LED, OUTPUT);
    digitalWrite(Pins::LED, LOW);
}

// ---- MQTT 回调：只做转发，保持轻量（回调运行在 MqttUploadTask 上下文） ----
void AppController::onConfigMsg(const char* payload, size_t len) {
    if (self_) self_->config_.updateFromJson(payload, len);
}

void AppController::onOtaMsg(const char* url) {
    // 仅排队；真正的下载在 OtaTask 中执行，不阻塞 MQTT 保活
    if (self_) self_->ota_.requestHttpOta(url);
}

/**
 * @brief 一次性初始化：按依赖顺序启动各模块，最后创建 FreeRTOS 任务
 *
 * 顺序说明（改动前务必确认依赖）：
 *   1. Serial/WDT/LED  —— 最先就绪，保证后续日志与超时保护可用
 *   2. I2C 总线         —— 传感器与 OLED 的前提
 *   3. Config/RingBuffer—— 无硬件依赖，任务创建前必须可用
 *   4. 传感器/OLED      —— 失败直接重启（没有它们节点无意义）
 *   5. WiFi/NTP         —— MQTT 与 OTA 依赖网络与正确时间
 *   6. MQTT/OTA         —— 只做配置，真正连接在任务里进行
 *   7. 创建任务         —— 放在最后，避免任务访问尚未初始化的模块
 */
void AppController::begin() {
    // 静态回调（MQTT 消息、OTA）通过 self_ 找到本实例
    self_ = this;

    Serial.begin(115200);
    delay(200);                 // 等 USB-CDC/串口稳定，避免开头几行日志丢失
    Serial.println("\n===== ESP32-C3 EnvNode 启动 =====");

    initWdt();                  // 尽早开启看门狗
    initLed();

    // I2C 必须先于任何传感器/显示调用；内部会创建总线互斥锁
    I2cBus::begin(Pins::I2C_SDA, Pins::I2C_SCL);
    Serial.println("[Init] I2C 总线已启动");

    config_.begin();            // 读取 NVS 中保存的采样/上传间隔
    buffer_.begin();            // 创建环形缓冲互斥锁

    // 传感器/屏幕属于"必需外设"：初始化失败重试后仍失败则重启，
    // 避免系统带着残缺硬件继续运行、产生大量无效数据。
    if (!sensor_.begin()) {
        Serial.println("[Init] 传感器初始化失败，重启");
        ESP.restart();
    }
    if (!display_.begin()) {
        Serial.println("[Init] OLED 初始化失败，重启");
        ESP.restart();
    }

    // 阻塞等待联网（最多 120s），成功后才继续 NTP/MQTT/OTA
    wifi_.begin(WIFI_SSID, WIFI_PWD, HOSTNAME.c_str());
    syncTimeOnce();             // 最多阻塞 15s；失败也能继续跑（时间戳=0）

    mqtt_.begin(MQTT_SERVER, MQTT_PORT,
                MQTT_USER, MQTT_PASS,
                HOSTNAME.c_str(),   // clientId：同一 Broker 下需唯一
                DEVICE_ID.c_str()); // deviceId：用于 Topic 前缀
    // 回调只做转发，实际耗时操作都在对应任务中完成
    mqtt_.setConfigCallback(&AppController::onConfigMsg);
    mqtt_.setOtaCallback(&AppController::onOtaMsg);

    // 注意：OTA 密码当前为硬编码常量，量产前建议移动到 secrets.h
    ota_.begin(HOSTNAME.c_str(), "ota_pass");

    // 把模块指针打包进上下文，供四个任务共享（生命周期 = 本对象生命周期）
    ctx_.sensor  = &sensor_;
    ctx_.display = &display_;
    ctx_.buffer  = &buffer_;
    ctx_.config  = &config_;
    ctx_.mqtt    = &mqtt_;
    ctx_.ota     = &ota_;

    // 单核，靠优先级抢占：Sensor(5) > MQTT(3) > OTA(2) > Display(1)
    // 参数：入口函数、任务名、栈字节数、参数、优先级、任务句柄(不需要)
    // 栈大小经验值：Sensor 3072 / MQTT 8192（JSON+批量缓冲）/ OTA 4096 / Display 3072
    xTaskCreate(SensorTask,     "sensor",  3072, &ctx_, 5, nullptr);
    xTaskCreate(MqttUploadTask, "mqtt",    8192, &ctx_, 3, nullptr);
    xTaskCreate(OtaTask,        "ota",     4096, &ctx_, 2, nullptr);
    xTaskCreate(DisplayTask,    "display", 3072, &ctx_, 1, nullptr);

    Serial.println("[Init] 所有任务已创建，系统就绪\n");
}

/**
 * @brief 主循环：WiFi 重连检查 + LED 心跳
 *
 * 这里使用 vTaskDelay(50ms) 而不是 busy-wait，
 * 让 loopTask 主动让出 CPU 给采样/上传任务。
 */
void AppController::loop() {
    static uint32_t lastWifiCheck = 0;
    static uint32_t lastLedToggle = 0;
    static bool     ledState      = false;

    uint32_t now = millis();

    // 每 10s 检查一次 WiFi：断线只触发 reconnect()，实际重连由协议栈异步完成
    if (now - lastWifiCheck > 10000) {
        wifi_.checkReconnect();
        lastWifiCheck = now;
    }

    // LED 心跳：MQTT 在线 1Hz 慢闪，离线 5Hz 快闪
    // 实现上每 period/2 翻转一次 → period=1000 时 1Hz；period=200 时 5Hz
    uint32_t period = mqtt_.connected() ? 1000 : 200;
    if (now - lastLedToggle > period / 2) {
        ledState = !ledState;
        digitalWrite(Pins::LED, ledState);
        lastLedToggle = now;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
}
