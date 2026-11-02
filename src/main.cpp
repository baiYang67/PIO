/**
 * @file    main.cpp
 * @brief   程序入口：只做两件事 —— 构造全局编排器、把 setup/loop 转发给它
 *
 * 线程模型总览（ESP32-C3 为单核，靠优先级抢占调度）
 * ------------------------------------------------------------
 *   loopTask      : Arduino 主循环，优先级 1，负责 WiFi 重连检查 + LED 心跳
 *   SensorTask    : 优先级 5，周期采样入环形缓冲
 *   MqttUploadTask: 优先级 3，MQTT 保活 / 实时状态推送 / 定时批量上传
 *   OtaTask       : 优先级 2，ArduinoOTA 与 HTTP OTA
 *   DisplayTask   : 优先级 1，刷新 OLED
 *
 * 模块之间不直接调用，而是通过 AppController 持有的对象 + AppContext 指针通信，
 * 具体实现见 AppController.cpp / AppTasks.cpp。
 */
#include <Arduino.h>
#include "AppController.h"

// 全局唯一实例。
// 必须放在全局作用域：所有 FreeRTOS 任务都通过指针访问它持有的模块对象，
// 若定义在 setup() 的栈上，setup() 返回后任务将访问已释放的内存。
static AppController app;

/**
 * Arduino 启动流程入口，只执行一次。
 * 初始化顺序见 AppController::begin()：WDT → LED → I2C → 配置/缓冲 → 传感器 → 显示
 * → WiFi → NTP → MQTT → OTA → 创建任务。
 */
void setup() {
    app.begin();
}

/**
 * Arduino 主循环（运行在 loopTask 中，优先级 1）。
 * 这里只做轻量、非阻塞的工作；业务逻辑都在各 FreeRTOS 任务里，
 * 因此 loop() 卡顿不会影响采样与上传。
 */
void loop() {
    app.loop();
}