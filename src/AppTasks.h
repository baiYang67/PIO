#pragma once
/**
 * @file    AppTasks.h
 * @brief   FreeRTOS 任务声明 + 任务间共享上下文 AppContext
 *
 * 设计意图
 * ------------------------------------------------------------
 * 任务函数不能捕获 C++ 成员，因此把"任务需要的所有模块指针"打包进 AppContext，
 * 通过 xTaskCreate 的 pvParameters 传入。这样：
 *   - 不依赖全局变量，模块关系一目了然
 *   - 以后要做单元测试/多实例，只需要换一个 context
 *
 * 生命周期约定
 * ------------------------------------------------------------
 * AppContext 由 AppController 作为成员持有（静态存储），任务只读其中的指针，
 * 因此只要程序在运行，这些指针就一直有效。不要传入栈上的临时对象。
 *
 * 任务优先级（ESP32-C3 单核，数字越大越优先）
 * ------------------------------------------------------------
 *   SensorTask 5 > MqttUploadTask 3 > OtaTask 2 > DisplayTask 1
 * 理由：采样最不能被拖慢；显示晚 1s 无感；OTA 只在需要时占用。
 */
#include "EnvironmentSensor.h"
#include "OledDisplay.h"
#include "RingBuffer.h"
#include "ConfigManager.h"
#include "MqttManager.h"
#include "OtaManager.h"

/**
 * 应用上下文：把各模块指针打包传给任务，避免任务函数使用全局变量
 */
struct AppContext {
    EnvironmentSensor* sensor;    // 采集传感器数据
    OledDisplay*       display;   // 刷新本地屏幕
    RingBuffer*        buffer;    // 采样与上传之间的解耦队列
    ConfigManager*     config;    // 采样/上传周期（运行期可改）
    MqttManager*       mqtt;      // 网络发布与下行命令
    OtaManager*        ota;       // 远程升级
    // 跨任务标志：MqttUploadTask 置位，DisplayTask 读取。
    // volatile 防止编译器把读操作优化到循环外（单写单读的 bool 无需再加锁）。
    volatile bool      uploading = false;   // 供 OLED 显示"上传中"
};

/** 采集任务（优先级 5）：周期读取传感器 → 写入环形缓冲 */
void SensorTask(void* pv);

/** MQTT 任务（优先级 3）：保活 + 实时状态推送 + 定时批量上传 */
void MqttUploadTask(void* pv);

/** 显示任务（优先级 1）：周期刷新 OLED */
void DisplayTask(void* pv);

/** OTA 任务（优先级 2）：处理 ArduinoOTA 与 HTTP OTA 请求 */
void OtaTask(void* pv);
