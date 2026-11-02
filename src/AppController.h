#pragma once
/**
 * @file    AppController.h
 * @brief   顶层编排器：模块持有者 + 初始化顺序 + 任务创建 + 主循环（WiFi 重连 / LED 心跳）
 *
 * 为什么用"编排器"而不是让 main.cpp 直接调用各模块
 * ------------------------------------------------------------
 *  - 所有模块对象集中在一个类里，生命周期清晰（全局唯一实例）
 *  - 初始化顺序集中可见，避免"A 依赖 B 但 B 未初始化"的顺序问题
 *  - 任务的 AppContext 需要一组稳定的指针，由本类统一提供
 *
 * 启动顺序（begin 内，顺序有依赖，勿随意调整）
 * ------------------------------------------------------------
 *   WDT → LED → I2C 总线 → Config(NVS) → RingBuffer → 传感器 → OLED
 *   → WiFi（阻塞等 IP） → NTP（最多 15s） → MQTT 配置 → OTA → 创建 4 个任务
 *
 * 主循环职责（loop，运行在 loopTask）
 * ------------------------------------------------------------
 *  - 每 10s 检查 WiFi，掉线则触发重连
 *  - LED 心跳：MQTT 在线慢闪（1Hz），离线快闪（5Hz）
 */
#include "WifiManager.h"
#include "AppTasks.h"

class AppController {
public:
    /** @brief 一次性初始化并创建所有 FreeRTOS 任务（由 setup() 调用） */
    void begin();

    /** @brief 主循环轻量工作（由 Arduino loop() 调用，周期约 50ms） */
    void loop();

private:
    /** @brief 初始化任务看门狗（兼容 Arduino Core 2.x / 3.x 的不同 API） */
    void initWdt();

    /** @brief 配置 NTP 并尝试同步一次（失败不阻塞启动，仅记录） */
    void syncTimeOnce();

    /** @brief 初始化状态灯 GPIO */
    void initLed();

    /** @brief MQTT config topic 回调 → 转交 ConfigManager */
    static void onConfigMsg(const char* payload, size_t len);

    /** @brief MQTT ota topic 回调 → 转交 OtaManager（仅排队，不下载） */
    static void onOtaMsg(const char* url);

    // ---- 模块实例（顺序即构造顺序；均为无参构造，互不依赖） ----
    EnvironmentSensor sensor_;   // AHT20 + BMP280
    OledDisplay       display_;  // SSD1306
    RingBuffer        buffer_;   // 采样 → 上传的队列
    ConfigManager     config_;   // 运行期配置（NVS）
    MqttManager       mqtt_;     // MQTT 客户端
    OtaManager        ota_;      // 双通道 OTA
    WifiManager       wifi_;     // WiFi 连接/重连

    /** 任务共享上下文：成员指针在对象生命周期内恒定，可安全传给任务 */
    AppContext        ctx_{};

    /** 静态回调需要访问实例（PubSubClient 的回调没有 user data 参数） */
    static AppController* self_;
};
