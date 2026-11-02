#pragma once
/**
 * @file    MqttManager.h
 * @brief   MQTT 通信：连接管理、实时/批量发布、配置与 OTA 下行、HA 自动发现
 *
 * Topic 约定（均带 DEVICE_ID 前缀，天然支持多设备共存）
 * ------------------------------------------------------------
 *   env-monitor/<ID>/state   发布  最新一条实时状态（HA sensor 订阅）
 *   env-monitor/<ID>/data    发布  批量历史数据（JSON，用于归档/补传）
 *   env-monitor/<ID>/config  订阅  配置命令 {"sample_interval":..,"upload_interval":..}
 *   env-monitor/<ID>/status  发布  在线状态（retain；连接时注册 LWT，掉线自动置 offline）
 *   env-monitor/<ID>/ota     订阅  OTA 命令 {"url":"http://server/firmware.bin"}
 *
 * 线程安全（务必遵守）
 * ------------------------------------------------------------
 *  PubSubClient 不是线程安全的：loop()/publish()/connect() 都在操作同一个 socket。
 *  本工程约定：所有 client_ 操作只在 MqttUploadTask 中执行，
 *  其他任务不要直接调用 publishState / publishBatch / loop / ensureConnected。
 *  消息回调 onMessage() 运行在"调用 client_.loop() 的那个任务"上下文，
 *  所以回调里只能做轻量转发（更新配置/给 OTA 信号量），不能阻塞。
 *
 * 内存策略
 * ------------------------------------------------------------
 *  批量 JSON 序列化到静态缓冲区 jsonBuf_[12288]，
 *  避免高频 malloc/free 造成堆碎片（ESP32 长期运行的常见故障源）。
 *  同时 PubSubClient 的内部收发缓冲也设为同样大小。
 */
#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include "RingBuffer.h"

class MqttManager {
public:
    /**
     * @brief 配置服务器与凭据（不发起连接）
     * @param server   Broker 地址
     * @param port     Broker 端口（通常 1883 / TLS 8883）
     * @param user     用户名；空字符串表示匿名
     * @param pass     密码；空字符串表示匿名
     * @param clientId MQTT ClientId，同一 Broker 下必须唯一，重复会被踢下线
     * @param deviceId 设备 ID，用于拼接 Topic 与 HA unique_id
     */
    void begin(const char* server, uint16_t port,
               const char* user, const char* pass,
               const char* clientId, const char* deviceId);

    /**
     * @brief 确保已连接；断线则尝试连接并（重新）订阅、发布 Discovery
     * @return true=当前已连接
     * @note  连接失败内部会 vTaskDelay(5s)，因此只能在 MqttUploadTask 中调用。
     */
    bool ensureConnected();

    /** @brief 泵 MQTT 事件循环（维持 keepalive、收取下行消息），仅 MqttUploadTask 调用 */
    void loop();

    /** @brief 当前 TCP/MQTT 是否处于已连接状态（只读查询，任意任务可调用） */
    bool connected();

    /** @brief 发布一条实时状态到 state topic（非 retain，HA 只关心最新值） */
    bool publishState(const SensorRecord& r);

    /** @brief 发布一批历史数据到 data topic（数组长度由 n 指定） */
    bool publishBatch(const SensorRecord* arr, int n);

    /** @brief 发布 HA Discovery（7 个实体：4 传感器 + 2 数值配置 + 1 在线状态） */
    void publishDiscovery();

    /** @brief 注册 config topic 回调（payload 为原始字节，长度 len） */
    void setConfigCallback(void (*cb)(const char* payload, size_t len));

    /** @brief 注册 ota topic 回调（已从 JSON 中解析出 url） */
    void setOtaCallback(void (*cb)(const char* url));

    // Topic 生成器（供 AppTasks 打印/调试）
    String dataTopic()      const;
    String stateTopic()     const;
    String configTopic()    const;
    String statusTopic()    const;
    String otaTopic()       const;

private:
    /**
     * @brief PubSubClient 的静态消息回调
     * @param topic 收到的 Topic（MQTT 库内部缓冲，函数返回后失效，必须立即拷贝/比较）
     * @param payload 消息体（不以 '\0' 结尾，长度见 len）
     * @param len 消息体长度
     */
    static void onMessage(char* topic, byte* payload, unsigned int len);

    /** 静态回调无法捕获 this，用单例指针转发（与 AppController::self_ 同一套路） */
    static MqttManager* self_;

    /** @brief 拼出 "env-monitor/<deviceId>" 前缀 */
    String topicPrefix() const { return String("env-monitor/") + deviceId_; }

    WiFiClient   wifi_;
    PubSubClient client_{wifi_};   // 必须在 wifi_ 之后构造（构造参数引用 wifi_）

    void (*configCb_)(const char*, size_t) = nullptr;
    void (*otaCb_)(const char*)            = nullptr;

    const char* clientId_ = nullptr;   // 指向 AppController 传入的静态字符串，无需拷贝
    const char* user_     = nullptr;
    const char* pass_     = nullptr;
    String      deviceId_;             // 需要拼接，故用 String 保存副本

    // 静态 JSON 缓冲，避免每次 malloc 造成堆碎片
    // 12KB 足够容纳 50 条 × ~110 字节/条 + 外层结构
    static char jsonBuf_[12288];
};
