#pragma once
/**
 * @file    ConfigManager.h
 * @brief   运行期配置管理（采样间隔 / 上传间隔），支持 NVS 持久化 + MQTT 动态下发
 *
 * 数据来源优先级
 * ------------------------------------------------------------
 *   1. NVS 中已保存的值（上次运行或上次 MQTT 下发的结果，重启后仍生效）
 *   2. 无 NVS 记录时使用 config.h 中的默认值
 *
 * 并发模型
 * ------------------------------------------------------------
 * 写入方：MqttUploadTask 收到 config topic 消息后调用 updateFromJson()
 * 读取方：SensorTask / MqttUploadTask / （未来）OLED
 * 两者跨任务访问 cfg_，因此用互斥锁保护，get() 返回的是"值拷贝"，
 * 调用方拿到后即使配置被更新也不会读到半新半旧的字段。
 *
 * 校验规则（见 ConfigManager::update）
 * ------------------------------------------------------------
 *   sampleIntervalMs >= Acquisition::MIN_INTERVAL
 *   sampleIntervalMs <= uploadIntervalMs <= Acquisition::MAX_UPLOAD_INTV
 * 非法值整条拒绝，保持旧配置不变（避免"只改了一半"的中间状态）。
 *
 * @note 每次 update() 都会写一次 NVS（Flash 擦写），因此不要用很高的频率
 *       下发配置；正常运维场景一天几次完全没问题。
 */
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "Config.h"          // ✅ 修复：使用 Acquisition::DEFAULT_* 必须先 include

/** 运行期可调的配置快照（按值拷贝使用，天然线程安全） */
struct RuntimeConfig {
    uint32_t sampleIntervalMs;   // 采样周期，单位毫秒
    uint32_t uploadIntervalMs;   // 批量上传周期，单位毫秒
};

class ConfigManager {
public:
    /** @brief 打开 NVS 命名空间并加载已保存的配置（无记录则用默认值） */
    void begin();

    /** @brief 获取当前配置快照（线程安全，返回副本） */
    RuntimeConfig get();

    /**
     * @brief 更新并持久化两个间隔
     * @return true=校验通过且已写入 NVS；false=参数非法，配置保持不变
     */
    bool update(uint32_t sampleMs, uint32_t uploadMs);

    /**
     * @brief 从 MQTT 下发的 JSON 更新配置
     *
     * 支持字段（都可选，缺省则沿用当前值，便于"只改一项"）：
     *   {"sample_interval": 5000, "upload_interval": 60000}
     *
     * @return true=解析成功且更新通过；false=JSON 非法或参数越界
     */
    bool updateFromJson(const char* payload, size_t len);

private:
    /** @brief 把 cfg_ 写入 NVS；调用前必须已持有 mutex_ */
    void saveLocked();

    RuntimeConfig cfg_ = {
        Acquisition::DEFAULT_SAMPLE_INTV,   // 默认 1s
        Acquisition::DEFAULT_UPLOAD_INTV    // 默认 10s
    };
    Preferences prefs_;                     // NVS 封装，命名空间 "env-cfg"
    SemaphoreHandle_t mutex_ = nullptr;     // 保护 cfg_ 与 NVS 写入
};
