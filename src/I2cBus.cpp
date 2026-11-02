/**
 * @file    I2cBus.cpp
 * @brief   I2C 总线互斥锁实现
 *
 * 生命周期：begin() 只在 AppController::begin() 中调用一次，
 *          此后 mutex_ 一直有效，无需释放（互斥量随程序常驻）。
 */
#include "I2cBus.h"

SemaphoreHandle_t I2cBus::mutex_ = nullptr;

void I2cBus::begin(int sda, int scl) {
    // 使用"互斥量(Mutex)"而不是"二值信号量(Binary Semaphore)"：
    // 互斥量具备优先级继承 —— 当低优先级的 DisplayTask(1) 持锁时，
    // 高优先级的 SensorTask(5) 等待锁会让 DisplayTask 临时继承优先级 5，
    // 从而尽快执行完并释放锁，避免优先级反转导致调度停滞。
    mutex_ = xSemaphoreCreateMutex();

    Wire.begin(sda, scl);
    // 400kHz 快速模式：AHT20/BMP280/SSD1306 均支持。
    // 若排线较长、出现 NACK 或花屏，可降到 100000 提高稳定性（代价是刷新变慢）。
    Wire.setClock(400000);
}

bool I2cBus::lock(TickType_t t) {
    // begin() 之前 mutex_ 为空：直接放行，避免初始化顺序问题导致永远拿不到锁。
    if (mutex_ == nullptr) return true;
    // pdTRUE 表示在超时时间内拿到锁；超时返回 pdFALSE
    return xSemaphoreTake(mutex_, t) == pdTRUE;
}

void I2cBus::unlock() {
    if (mutex_ != nullptr) xSemaphoreGive(mutex_);
}