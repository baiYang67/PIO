#pragma once
/**
 * @file    I2cBus.h
 * @brief   I2C 总线互斥封装（全局单例，纯静态接口）
 *
 * 为什么需要它
 * ------------------------------------------------------------
 * AHT20、BMP280、SSD1306 挂在同一条 I2C 总线上，而 Arduino 的 Wire 库
 * 以及各 Adafruit 驱动内部都直接使用全局 Wire 对象，本身不是线程安全的。
 * 本工程有 SensorTask（优先级 5）与 DisplayTask（优先级 1）并发访问 I2C，
 * 若不串行化，会出现：
 *   - 事务被高优先级任务打断 → 从机 NACK / 数据错乱
 *   - OLED 显存尚未刷完就被传感器读取穿插 → 花屏
 *
 * 使用约定
 * ------------------------------------------------------------
 *   if (I2cBus::lock()) {
 *       ... 一整套 Wire/驱动调用 ...
 *       I2cBus::unlock();      // 必须与 lock 成对出现
 *   }
 * 临界区应尽量短：只包住"一次完整的传感器读数"或"一次整屏刷新"，
 * 不要在里面 delay() 或做大块 JSON 处理，否则会拖慢另一边。
 *
 * 关于超时
 * ------------------------------------------------------------
 * lock() 默认最多等 200ms。超时说明另一方占用过久或死锁，
 * 此时应放弃本次事务（跳过这次采集/刷新），而不是死等，以免拖垮看门狗。
 */
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class I2cBus {
public:
    /**
     * @brief 初始化 I2C 总线与互斥锁，必须在任何传感器/显示操作之前调用一次
     * @param sda SDA 引脚号
     * @param scl SCL 引脚号
     * @note  总线速率固定 400kHz（快速模式），见 I2cBus.cpp
     */
    static void begin(int sda, int scl);

    /**
     * @brief 加锁
     * @param timeout 最长等待时间（默认 200ms）
     * @return true=成功持有锁；false=超时，调用方必须放弃本次事务且不能 unlock()
     * @note  begin() 之前调用时（mutex_ 为空）直接返回 true，便于单测/早期初始化。
     */
    static bool lock(TickType_t timeout = pdMS_TO_TICKS(200));

    /** @brief 解锁，必须与一次成功的 lock() 成对出现 */
    static void unlock();

private:
    static SemaphoreHandle_t mutex_;   // FreeRTOS 互斥量，含优先级继承
};