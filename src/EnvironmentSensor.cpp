/**
 * @file    EnvironmentSensor.cpp
 * @brief   AHT20 + BMP280 具体驱动调用
 *
 * 两颗芯片都走 I2C，共用 Wire 总线与 I2cBus 互斥锁。
 * 驱动对象声明为文件内 static：不暴露到头文件，避免其他模块误用未加锁的驱动。
 */
#include "EnvironmentSensor.h"
#include "I2cBus.h"
#include "Config.h"
#include <AHT20.h>
#include <Adafruit_BMP280.h>

// 库对象为文件内静态，避免暴露到头文件
// 注意：这些对象内部直接操作全局 Wire，因此所有调用都必须放在 I2cBus 锁内。
static AHT20           aht;
static Adafruit_BMP280 bmp;

bool EnvironmentSensor::begin() {
    // ---------- AHT20 ----------
    // 上电后芯片需要约 40ms 稳定时间，若接线松动/地址不对 begin() 会失败。
    // 这里做最多 5 次重试（每次间隔 2s），给热插拔和电源抖动留余量。
    uint8_t retry = 0;
    while (!aht.begin() && retry++ < 5) {
        Serial.println("[Sensor] AHT20 初始化失败, 2s 后重试...");
        delay(2000);
    }
    // retry >= 5 表示已经累计失败 5 次，判定为硬件问题（接线/供电/地址）
    if (retry >= 5) {
        Serial.println("[Sensor] AHT20 初始化彻底失败");
        return false;
    }
    Serial.println("[Sensor] AHT20 初始化成功");

    // ---------- BMP280 ----------
    retry = 0;
    while (!bmp.begin(Pins::BMP280_ADDR) && retry++ < 5) {
        Serial.println("[Sensor] BMP280 初始化失败, 2s 后重试...");
        delay(2000);
    }
    if (retry >= 5) {
        Serial.println("[Sensor] BMP280 初始化彻底失败");
        return false;
    }
    // 常规采样模式：内部定时采样，读取时直接取最新值，无需每次触发测量
    // （对比 FORCED 模式：每次读都要等测量完成，会拉长 I2C 占用时间）
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL);
    Serial.println("[Sensor] BMP280 初始化成功");
    return true;
}

bool EnvironmentSensor::read(SensorRecord& out) {
    // 所有 I2C 事务必须持锁，防止与 OLED 刷新冲突
    // 加锁失败说明 OLED 正在刷屏（或总线异常），本次直接跳过，等下一周期
    if (!I2cBus::lock()) {
        Serial.println("[Sensor] I2C 加锁超时，跳过本次采集");
        return false;
    }

    out.temp    = aht.getTemperature();          // 单位 °C
    out.humi    = aht.getHumidity();             // 单位 %
    out.press   = bmp.readPressure() / 100.0f;   // Pa → hPa（1 hPa = 100 Pa）
    out.bmpTemp = bmp.readTemperature();         // BMP280 内部温度，仅作参考/交叉校验

    // 每条记录独立时间戳（Unix 秒）
    time_t t = time(nullptr);
    // time() 在 NTP 未同步时返回 1970 附近的值。
    // 用 < 2020-01-01(1577836800) 判定为"未同步"，统一写 0，
    // 后端据此区分"时间未知"与"真实 1970 年数据"。
    out.timestamp = (t > 1577836800) ? (uint32_t)t : 0;

    I2cBus::unlock();
    return true;
}
