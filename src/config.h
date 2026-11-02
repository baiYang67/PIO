#pragma once
/**
 * @file    config.h
 * @brief   全局编译期配置：设备标识 / 引脚 / 显示 / MQTT / 采集参数 / 看门狗 / NTP
 *
 * 设计思路
 * --------
 * 1. "运行期不会变"的参数集中在这里，移植到新硬件时只需改本文件。
 * 2. "运行期可调"的参数（采样间隔、上传间隔）由 ConfigManager 管理并存 NVS，
 *    不要往这里加，否则会和 MQTT 动态配置产生两套真值。
 * 3. 敏感凭据（WiFi 密码、MQTT 地址）放在 secrets.h，本文件只做存在性检查。
 *
 * 常见修改场景
 * ------------
 * - 换 I2C 引脚 / 状态灯  → 修改 namespace Pins
 * - 换屏幕分辨率          → 修改 namespace Display（同时确认 SSD1306 型号）
 * - 改采集间隔上下限      → 修改 namespace Acquisition
 * - 改时区                → 修改 GMT_Offset
 */
#include <Arduino.h>

// ===== 安全隔离 =====
// secrets.h 不随仓库分发（含 WiFi/MQTT 凭据）。
// 缺少该文件时在编译期直接 #error，比"编译通过但运行期连不上"更容易定位。
#ifdef __has_include
#  if __has_include("secrets.h")
#    include "secrets.h"        // 提供 WIFI_SSID / WIFI_PWD / MQTT_SERVER / MQTT_PORT ...
#  else
#    error "需要创建secrets.h文件！"
#  endif
#endif

// ===== 设备标识 =====
// HOSTNAME           : 局域网主机名（DHCP / mDNS / ArduinoOTA 使用），也作为 HA 设备名
// DEVICE_ID          : MQTT Topic 前缀 + HA Discovery 的 unique_id 前缀，多设备时必须唯一
// deviceSerialNumber : 预留的硬件序列号（当前未被业务代码使用，便于后续接入资产系统）
const String HOSTNAME = "EnvSensor-C3-01";
const String DEVICE_ID = "C3SA57231";
const String deviceSerialNumber = "ESP01SAT4404";

// ===== 硬件接口 =====
// ESP32-C3 的 I2C 可映射到任意 GPIO。
// AHT20 / BMP280 / SSD1306 挂同一条总线，靠不同器件地址区分，因此必须由 I2cBus 串行化访问。
namespace Pins {
  constexpr uint8_t I2C_SDA = 8;         // I2C 数据线
  constexpr uint8_t I2C_SCL = 9;         // I2C 时钟线
  constexpr uint8_t LED = 10;            // 状态灯：心跳指示（见 AppController::loop）
  constexpr uint8_t BMP280_ADDR = 0x76;  // BMP280 地址：SDO 接地=0x76，接 VCC=0x77
  constexpr uint8_t AHT20_ADDR = 0x38;   // AHT20 固定地址（由 AHT20 库内部使用，此处仅备查）
}

// ===== 显示配置 =====
namespace Display {
  constexpr uint16_t WIDTH = 128;              // SSD1306 水平像素
  constexpr uint16_t HEIGHT = 64;              // SSD1306 垂直像素
  constexpr uint8_t RESET_PIN = -1;            // -1 表示模组无独立复位脚，随 MCU 上电复位
  constexpr uint32_t REFRESH_INTERVAL = 1000;  // OLED 刷新周期(ms)；过快会挤占 I2C 带宽
}

// ===== MQTT 配置 =====
// 注意：真正使用的 Topic 由 MqttManager::topicPrefix() 动态拼接，
//       形如 env-monitor/<DEVICE_ID>/state。
//       下面这些常量是早期的、不带设备 ID 的固定 Topic，仅为兼容/备查保留；
//       调整 Topic 结构时请同步检查 MqttManager.cpp 的 topic 生成函数。
namespace MQTT {
  namespace Topics {
    const String BASE = "env-monitor/";                  // Topic 根前缀
    constexpr const char* DATA = "env-monitor/data";     // 批量历史数据（旧版格式）
    constexpr const char* CONFIG = "env-monitor/config"; // 配置下发（旧版格式）
    const String STATUS = "env-monitor/status";          // 在线状态（旧版格式）
    const String OTA = "env-monitor/ota";                // OTA 指令（旧版格式）
  }
}

// ===== 数据采集配置 =====
namespace Acquisition {
  constexpr uint32_t MIN_INTERVAL = 1000;              // 间隔下限 1s：过密会让 I2C 与 MQTT 饱和
  constexpr uint32_t DEFAULT_SAMPLE_INTV = 1000;       // 默认采样间隔 1s（NVS 无记录时使用）
  constexpr uint32_t DEFAULT_UPLOAD_INTV = 10000;      // 默认上传间隔 10s
  constexpr uint32_t MAX_UPLOAD_INTV = 600000;         // 上传间隔上限 10min
}

// ===== 看门狗配置 =====
// 单位：秒。所有任务必须在该时间内调用 esp_task_wdt_reset()，否则整机重启。
// 取值需大于"最慢一次正常操作"（例如 HTTP OTA 下载），否则会被误复位。
constexpr uint32_t WDT_TIMEOUT = 30;

// ===== NTP 配置 =====
// 东八区 = 8 * 3600 秒；中国不使用夏令时，Daylight_Offset 固定为 0。
constexpr long GMT_Offset = 8 * 3600;
constexpr int Daylight_Offset = 0;
// 按顺序尝试：优先局域网路由器（快、不依赖外网），失败再走公网 NTP。
// 公网不可达时 getLocalTime() 会超时失败，此时记录 timestamp=0。
static const char* const ntpServers[] = {
    "192.168.8.1", "pool.ntp.org", "time.nist.gov"
};
