#pragma once
/**
 * @file    RingBuffer.h
 * @brief   传感器数据环形缓冲（生产者=SensorTask，消费者=MqttUploadTask）
 *
 * 为什么需要缓冲
 * ------------------------------------------------------------
 * WiFi/MQTT 断线时采样不能停，数据先落到 RAM，恢复后按时间顺序补传，
 * 这样"网络抖动"不会造成数据空洞。
 *
 * 关键设计
 * ------------------------------------------------------------
 *  - push()   永不阻塞：满了覆盖最老数据，只计数告警，绝不拖慢采集任务
 *  - peek() + commit() 两阶段提交：先拷贝待发数据，MQTT 发布成功后再真正出队；
 *              发布失败则数据留在缓冲里，下一轮自动重试，做到"至少一次"投递
 *  - latest() 供 OLED 与实时状态推送使用，不影响队列本身
 *  - 所有公开方法内部都持互斥锁，可在任意任务中调用
 *
 * 容量取舍
 * ------------------------------------------------------------
 * 300 条 × 1s 采样 ≈ 5 分钟离线缓存；SensorRecord 为 20 字节，
 * 共约 6KB RAM，对 ESP32-C3 的 ~320KB SRAM 影响很小。
 * 容量与"最长可容忍断网时间"成正比，需要更长离线时间就调大 RING_BUFFER_SIZE。
 *
 * @warning peek() 与 commit() 之间若缓冲被写满并发生覆盖，
 *          commit() 移除的是"当前最老的 n 条"，可能与刚 peek 的数据不一致。
 *          默认参数下（1s 采样、300 条 = 5 分钟）几乎不会触发；若把容量调得很小，
 *          需要改为"按序列号提交"的写法。
 */
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/**
 * 环形缓冲容量（条）
 * 300 条 = 5 分钟数据（1 秒采样）。
 */
#define RING_BUFFER_SIZE 300

/** 每丢弃多少条打印一次告警，避免缓冲长期溢出时刷屏 */
#define RING_DROP_WARN_INTERVAL 50

/**
 * 每批上传条数
 * ------------------------------------------------------------
 * 30 条 × ~110 字节/条 ≈ 3.3KB JSON，安全落在 MqttManager 的 12KB 静态缓冲内。
 * 太大：单次 publish 占用时间长，期间无法响应 MQTT 保活/下行命令；
 * 太小：往返次数增加，网络开销变大。
 */
#define CHUNK_SIZE 30

/**
 * 单条传感器记录（约 20 字节，sizeof 含对齐可能略大）
 *
 * 每条都带独立 Unix 秒时间戳的原因：
 *   1. 断网补传时时间精确，不必用"批次起始时间 + 推断间隔"来还原
 *   2. 后端归档无需外部补时间轴
 */
struct SensorRecord {
    uint32_t timestamp;   // Unix 秒（UTC）；0 表示 NTP 尚未同步，时间不可信
    float    temp;        // AHT20 温度（°C）
    float    humi;        // AHT20 相对湿度（%）
    float    press;       // BMP280 气压（hPa）
    float    bmpTemp;     // BMP280 芯片内部温度（°C）
};

class RingBuffer {
public:
    /** @brief 创建互斥锁，系统启动时调用一次 */
    void begin();

    /** @brief 写入一条记录（满则覆盖最老数据） */
    void push(const SensorRecord& r);

    /**
     * @brief 从最老端拷贝最多 maxN 条数据，但不删除
     * @param out  调用方提供的数组，长度至少 maxN
     * @param maxN 本次最多取多少条（通常传 CHUNK_SIZE）
     * @return 实际拷贝条数；0 表示缓冲为空或加锁超时
     */
    int  peek(SensorRecord* out, int maxN);

    /**
     * @brief 确认已成功发送 n 条，从最老端真正移除（与 peek 配对使用）
     * @note  只能在对应 peek() 的发布成功后调用；失败时不要调用，以保留数据重试
     */
    void commit(int n);

    /** @brief 当前待上传条数（用于 OLED 显示与上传触发判断） */
    int  available();

    /** @brief 最近一次 push 的记录（OLED 显示 / 实时 state 推送） */
    SensorRecord latest();

    /** @brief 累计因缓冲满而覆盖丢弃的条数（诊断用） */
    uint32_t droppedCount();

private:
    SensorRecord buf_[RING_BUFFER_SIZE];   // 定长数组，无动态内存
    int head_ = 0;                         // 下一个写入位置
    int tail_ = 0;                         // 最老数据位置（下一个出队位置）
    int count_ = 0;                        // 当前有效条数，不变式：0 <= count_ <= RING_BUFFER_SIZE
    SensorRecord latest_ = {};             // 最新一条副本，零初始化保证未采样时安全
    uint32_t dropped_ = 0;                 // 累计覆盖次数
    SemaphoreHandle_t mutex_ = nullptr;    // 保护上述所有成员
};
