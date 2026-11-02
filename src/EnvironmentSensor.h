#pragma once
/**
 * @file    EnvironmentSensor.h
 * @brief   环境传感器聚合层：AHT20（温湿度）+ BMP280（气压/芯片温度）
 *
 * 分层意图
 * ------------------------------------------------------------
 * 上层任务只关心"读一条完整记录"，不关心总线上挂了哪个型号的芯片。
 * 换传感器（例如 SHT30 或 BME280）时只需改 EnvironmentSensor.cpp，
 * AppTasks / MQTT / OLED 代码都不用动。
 *
 * 线程安全
 * ------------------------------------------------------------
 * read() 内部会对 I2C 总线加锁，可与 OLED 刷新并发调用。
 * 但同一个 EnvironmentSensor 对象不要被两个任务同时 read()，
 * 因为 AHT20/BMP280 的驱动对象（文件内 static）不是可重入的。
 */
#include "RingBuffer.h"

class EnvironmentSensor {
public:
    /**
     * @brief 初始化两颗传感器（失败会重试，全部失败返回 false）
     * @return true=两颗芯片都就绪
     * @note  阻塞式：单次重试间隔 2s、最多 5 次；由 AppController 在启动阶段调用。
     */
    bool begin();

    /**
     * @brief 读取一次完整数据
     * @param out 输出记录；timestamp 由本函数填充（NTP 未同步时为 0）
     * @return true=读取成功；false=I2C 加锁超时（本次跳过，不影响后续采样）
     */
    bool read(SensorRecord& out);
};
