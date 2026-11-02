/**
 * @file    ConfigManager.cpp
 * @brief   运行期配置的加载 / 更新 / 持久化实现
 *
 * NVS 键名约定（不要随意修改，否则老设备升级后会"丢配置"）：
 *   "s_ms" → 采样间隔（毫秒）
 *   "u_ms" → 上传间隔（毫秒）
 */
#include "ConfigManager.h"
#include "Config.h"
#include <ArduinoJson.h>

void ConfigManager::begin() {
    mutex_ = xSemaphoreCreateMutex();

    // 命名空间 "env-cfg"，false = 读写模式（true 为只读）
    prefs_.begin("env-cfg", false);
    // 第二个参数是"键不存在时的默认值"，因此首次上电也能拿到合理配置
    cfg_.sampleIntervalMs = prefs_.getUInt("s_ms", Acquisition::DEFAULT_SAMPLE_INTV);
    cfg_.uploadIntervalMs = prefs_.getUInt("u_ms", Acquisition::DEFAULT_UPLOAD_INTV);

    Serial.printf("[Config] 加载配置: 采集=%lums, 上传=%lums\n",
                  cfg_.sampleIntervalMs, cfg_.uploadIntervalMs);
}

RuntimeConfig ConfigManager::get() {
    RuntimeConfig c;
    // portMAX_DELAY：配置读取是极短操作，值得等到锁（否则会读到未初始化值）
    xSemaphoreTake(mutex_, portMAX_DELAY);
    c = cfg_;                 // 值拷贝，出临界区后无共享
    xSemaphoreGive(mutex_);
    return c;
}

void ConfigManager::saveLocked() {
    // 前提：调用方已持有 mutex_
    // NVS 写入有寿命限制（约 10 万次），不要在循环里频繁调用 update()
    prefs_.putUInt("s_ms", cfg_.sampleIntervalMs);
    prefs_.putUInt("u_ms", cfg_.uploadIntervalMs);
}

bool ConfigManager::update(uint32_t s, uint32_t u) {
    // ---- 参数校验：任何一项非法则整体拒绝，保持配置原子性 ----
    if (s < Acquisition::MIN_INTERVAL) {
        Serial.printf("[Config] 拒绝: 采样间隔 %lu < %lu\n",
                      s, Acquisition::MIN_INTERVAL);
        return false;
    }
    // 上传间隔不能小于采样间隔（否则每轮上传都没有新数据），也不宜过大
    if (u < s || u > Acquisition::MAX_UPLOAD_INTV) {
        Serial.printf("[Config] 拒绝: 上传间隔 %lu 不在 [%lu, %lu]\n",
                      u, s, Acquisition::MAX_UPLOAD_INTV);
        return false;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    cfg_.sampleIntervalMs = s;
    cfg_.uploadIntervalMs = u;
    saveLocked();                 // 先改内存再落盘，二者在同一临界区内，保证一致
    xSemaphoreGive(mutex_);

    Serial.printf("[Config] 已更新并保存: 采集=%lums, 上传=%lums\n", s, u);
    return true;
}

bool ConfigManager::updateFromJson(const char* payload, size_t len) {
    // ✅ ArduinoJson 7.x 新 API：JsonDocument 自动管理容量，无需指定大小
    JsonDocument doc;
    if (deserializeJson(doc, payload, len) != DeserializationError::Ok) {
        Serial.println("[Config] JSON 解析失败");
        return false;
    }
    // 关键点：字段缺省时用当前值兜底（"|" 为 ArduinoJson 的默认值运算符），
    // 因此可以只下发 {"sample_interval": 5000} 而不影响上传间隔。
    RuntimeConfig cur = get();
    uint32_t s = doc["sample_interval"] | cur.sampleIntervalMs;
    uint32_t u = doc["upload_interval"] | cur.uploadIntervalMs;
    return update(s, u);
}
