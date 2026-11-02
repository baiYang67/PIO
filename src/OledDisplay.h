#pragma once
/**
 * @file    OledDisplay.h
 * @brief   SSD1306 OLED 显示封装（128×64，I2C 地址 0x3C）
 *
 * 显示布局（TextSize=1，6×8 字体，每行最多 21 字符）
 * ------------------------------------------------------------
 *   行 0 : Temp : 25.31 C
 *   行 1 : Humi : 60.20 %
 *   行 2 : Press: 1009.12 hPa
 *   行 3 : BMP  : 26.05 C
 *   行 7 : [OK] buf:12      ← 上传中显示 [UP]，buf 为待上传条数
 *
 * 线程安全
 * ------------------------------------------------------------
 * show() 内部对 I2C 加锁；与传感器读取互不干扰。
 * 只应由 DisplayTask 调用（避免多任务同时操作同一个 Adafruit_SSD1306 对象）。
 */
#include "RingBuffer.h"

class OledDisplay {
public:
    /**
     * @brief 初始化屏幕并清屏
     * @return false=未检测到屏幕（地址错误/接线问题/未供电）
     * @note  调用前必须先执行 I2cBus::begin()，否则 Wire 未初始化。
     */
    bool begin();

    /**
     * @brief 刷新一屏数据
     * @param d             要显示的最新记录（时间戳为 0 表示尚未采集到数据）
     * @param uploading     true=正在批量上传，末行显示 [UP]
     * @param bufferedCount 当前环形缓冲待上传条数
     * @note  I2C 加锁失败时直接跳过本次刷新（不影响采集与上传）。
     */
    void show(const SensorRecord& d, bool uploading, int bufferedCount);
};
