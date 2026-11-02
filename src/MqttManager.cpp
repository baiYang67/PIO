/**
 * @file    MqttManager.cpp
 * @brief   MQTT 连接 / 发布 / 订阅 / Home Assistant 自动发现 实现
 *
 * 连接时的三个动作（顺序不能乱）：
 *   1. connect() 时通过 will 参数注册 LWT（Last Will and Testament）
 *      —— 设备异常掉线，Broker 会自动向 status topic 发布 "offline"
 *   2. 连接成功后发布 "online"（retain=true，新订阅者立刻能读到当前状态）
 *   3. 订阅 config / ota 两个下行 Topic，再发布 HA Discovery
 */
#include "MqttManager.h"
#include "Config.h"
#include <ArduinoJson.h>

MqttManager* MqttManager::self_ = nullptr;
char MqttManager::jsonBuf_[12288];

// ---------------- Topic 生成 ----------------
// 统一加 "env-monitor/<DEVICE_ID>/" 前缀：多台设备接入同一 Broker 时互不干扰。
String MqttManager::dataTopic()   const { return topicPrefix() + "/data"; }
String MqttManager::stateTopic()  const { return topicPrefix() + "/state"; }
String MqttManager::configTopic() const { return topicPrefix() + "/config"; }
String MqttManager::statusTopic() const { return topicPrefix() + "/status"; }
String MqttManager::otaTopic()    const { return topicPrefix() + "/ota"; }

// ---------------- 初始化 ----------------
void MqttManager::begin(const char* server, uint16_t port,
                        const char* user, const char* pass,
                        const char* clientId, const char* deviceId) {
    self_     = this;
    // 空字符串按"匿名登录"处理：传 nullptr 给 PubSubClient 表示不使用认证
    user_     = (user && *user) ? user : nullptr;
    pass_     = (pass && *pass) ? pass : nullptr;
    clientId_ = clientId;
    deviceId_ = deviceId;

    client_.setServer(server, port);
    // 收发缓冲都扩到 12KB：批量 JSON 可能超过默认 256B，
    // 若不放大，publish 会静默失败（返回 false）。
    client_.setBufferSize(sizeof(jsonBuf_));
    client_.setCallback(&MqttManager::onMessage);
}


bool MqttManager::connected() { return client_.connected(); }
void MqttManager::loop()      { client_.loop(); }

void MqttManager::setConfigCallback(void (*cb)(const char*, size_t)) { configCb_ = cb; }
void MqttManager::setOtaCallback(void (*cb)(const char*))            { otaCb_ = cb; }

// ---------------- 连接管理 ----------------
bool MqttManager::ensureConnected() {
    if (client_.connected()) return true;

    Serial.println("[MQTT] 正在连接...");
    // connect(clientId, user, pass, willTopic, willQos, willRetain, willMessage)
    //   → 注册 LWT：设备异常掉线（断电/断网/看门狗复位）时，
    //     Broker 会代发 status="offline"（retain），HA 立即显示离线。
    //   keepAlive 使用默认 15s，由 loop() 里的 PINGREQ 维持。
    bool ok = client_.connect(
        clientId_, user_, pass_,
        statusTopic().c_str(), 0, true, "offline");

    if (ok) {
        Serial.println("[MQTT] 连接成功");
        // retain=true：新订阅者（如重启后的 HA）能立刻读到当前在线状态
        client_.publish(statusTopic().c_str(), "online", true);
        // 订阅两个下行 Topic；QoS 0 足够，命令丢失可重发
        client_.subscribe(configTopic().c_str());
        client_.subscribe(otaTopic().c_str());
        // 每次（重）连接都重发 Discovery，保证 HA 侧实体不会因 Broker 重置而丢失
        publishDiscovery();
        return true;
    }

    Serial.printf("[MQTT] 连接失败, state=%d\n", client_.state());
    // 失败退避 5s：避免 Broker 不可达时以毫秒级疯狂重连、抢占 CPU 与日志。
    // 注意这是阻塞延迟，只在 MqttUploadTask 中调用本函数才安全。
    vTaskDelay(pdMS_TO_TICKS(5000));
    return false;
}

// ---------------- 消息回调 ----------------
void MqttManager::onMessage(char* topic, byte* payload, unsigned int len) {
    if (!self_) return;
    // 拷贝一份 Topic：MQTT 库的 topic 缓冲在回调返回后会被复用
    String t(topic);
    // 同时把 payload 打出来（截断 64 字节）：排查"HA 到底有没有把命令发过来"的关键日志
    //   批量上传走的是发布方向，不受影响
    char peek[65];
    size_t peekLen = (len < sizeof(peek) - 1) ? len : sizeof(peek) - 1;
    if (peekLen > 0) memcpy(peek, payload, peekLen);
    peek[peekLen] = '\0';
    Serial.printf("[MQTT] 收到 [%s] len=%u payload=%s\n", topic, len, peek);

    // 配置下发：把原始字节直接交给 ConfigManager 解析（内部用 ArduinoJson）
    if (t == self_->configTopic()) {
        if (self_->configCb_) self_->configCb_((const char*)payload, len);
        return;
    }

    if (t == self_->otaTopic()) {
        // ✅ v7 新 API：JsonDocument（不指定容量）
        // payload 不以 '\0' 结尾，因此必须用带长度的 deserializeJson
        JsonDocument doc;
        if (deserializeJson(doc, payload, len) == DeserializationError::Ok) {
            const char* url = doc["url"] | "";
            // url 为空则忽略（防止误发空消息导致 OTA 流程空转）
            if (self_->otaCb_ && url[0] != '\0') self_->otaCb_(url);
        }
        return;
    }
}


// ---------------- 发布：单条实时状态 ----------------
bool MqttManager::publishState(const SensorRecord& r) {
    // 未连接直接返回：调用方（MqttUploadTask）会保留数据并在下一轮重试
    if (!client_.connected()) return false;


    // ✅ v7：JsonDocument 自动管理内存
    JsonDocument doc;
    doc["timestamp"]   = r.timestamp;
    doc["temperature"] = r.temp;
    doc["humidity"]    = r.humi;
    doc["pressure"]    = r.press;
    doc["bmp_temp"]    = r.bmpTemp;

    // 单条只有 100 多字节，用 String 序列化即可；
    // retain=false：state 只表达"当前值"，不需要 Broker 持久化历史。
    String payload;
    serializeJson(doc, payload);
    return client_.publish(stateTopic().c_str(), payload.c_str(), false);
}


// ---------------- 发布：批量历史数据 ----------------
/**
 * 发布的 JSON 结构（与后端/归档脚本约定，改动需同步后端）：
 * {
 *   "device_id": "C3SA57231",
 *   "record_count": 30,
 *   "data": [
 *     {"ts":1730000000,"temperature":25.3,"humidity":60.2,"pressure":1009.1,"bmp_temp":26.0},
 *     ...
 *   ]
 * }
 */
bool MqttManager::publishBatch(const SensorRecord* arr, int n) {
    if (!client_.connected() || n <= 0) return false;

    JsonDocument doc;
    doc["device_id"]    = deviceId_;
    doc["record_count"] = n;      // 便于后端校验实际条数（防截断）

    // ✅ v7：doc["data"].to<JsonArray>() 替代 createNestedArray
    JsonArray data = doc["data"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
        // ✅ v7：arr.add<JsonObject>() 替代 createNestedObject
        // 字段名用短名 ts（每条重复 30 次，压缩后能省不少流量）
        JsonObject o = data.add<JsonObject>();
        o["ts"]          = arr[i].timestamp;
        o["temperature"] = arr[i].temp;
        o["humidity"]    = arr[i].humi;
        o["pressure"]    = arr[i].press;
        o["bmp_temp"]    = arr[i].bmpTemp;
    }

    // 序列化到静态缓冲：len >= 缓冲区说明被截断，宁可放弃本次上传也不能发半截 JSON
    size_t len = serializeJson(doc, jsonBuf_, sizeof(jsonBuf_));
    if (len == 0 || len >= sizeof(jsonBuf_)) {
        Serial.println("[MQTT] 批量 JSON 序列化失败/溢出");
        return false;
    }

    Serial.printf("[MQTT] 批量上传 %d 条 (%u 字节)\n", n, (unsigned)len);
    // 发布结果返回给 uploadAllPending()：只有 true 才会 commit() 出队
    return client_.publish(dataTopic().c_str(), jsonBuf_);
}


// ---------------- HA Discovery ----------------
/**
 * Home Assistant MQTT Discovery 说明
 * ------------------------------------------------------------
 * HA 启动/重载时会订阅 homeassistant/+/+/config，
 * 设备把"实体描述"以 retain=true 发布到对应 Topic，HA 即自动创建实体，
 * 无需在 HA 里手工写 YAML。
 *
 * 每个 payload 的结构：
 * {
 *   "name":"Temperature", "unique_id":"<ID>_temp",
 *   "state_topic":"env-monitor/<ID>/state",
 *   "value_template":"{{ value_json.temperature }}",
 *   "unit_of_measurement":"°C", "device_class":"temperature",
 *   "state_class":"measurement",
 *   "device":{"identifiers":["<ID>"],"name":"<HOSTNAME>",
 *             "model":"ESP32-C3 EnvNode","sw_version":"3.1.0"}
 * }
 * 其中 device 段让 7 个实体在 HA 里归到同一台设备下（devBase 复用）。
 *
 * 注意：volatile 命令类实体（number）的 command_topic 是 config topic，
 *       HA 侧修改数值后会以 JSON 形式回传，由 ConfigManager 解析。
 */
void MqttManager::publishDiscovery() {
    // 复用片段：device 对象；末尾不闭合，由各 payload 的 "}}" 一并闭合
    String devBase = String("\"device\":{\"identifiers\":[\"") + deviceId_ +
                     "\"],\"name\":\"" + HOSTNAME +
                     "\",\"model\":\"ESP32-C3 EnvNode\",\"sw_version\":\"" FW_VERSION "\"";


    // 1) 温度传感器：state topic 中的 temperature 字段
    {
        String t = "homeassistant/sensor/" + deviceId_ + "_temperature/config";
        String p = String("{\"name\":\"Temperature\",\"unique_id\":\"") + deviceId_ +
                   "_temp\",\"state_topic\":\"" + stateTopic() +
                   "\",\"value_template\":\"{{ value_json.temperature }}\"," +
                   "\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\"," +
                   "\"state_class\":\"measurement\"," + devBase + "}}";
        client_.publish(t.c_str(), p.c_str(), true);
    }
    // 2) 湿度传感器：unit=% / device_class=humidity
    {
        String t = "homeassistant/sensor/" + deviceId_ + "_humidity/config";
        String p = String("{\"name\":\"Humidity\",\"unique_id\":\"") + deviceId_ +
                   "_hum\",\"state_topic\":\"" + stateTopic() +
                   "\",\"value_template\":\"{{ value_json.humidity }}\"," +
                   "\"unit_of_measurement\":\"%\",\"device_class\":\"humidity\"," +
                   "\"state_class\":\"measurement\"," + devBase + "}}";
        client_.publish(t.c_str(), p.c_str(), true);
    }
    // 3) 气压传感器：unit=hPa / device_class=pressure
    {
        String t = "homeassistant/sensor/" + deviceId_ + "_pressure/config";
        String p = String("{\"name\":\"Pressure\",\"unique_id\":\"") + deviceId_ +
                   "_pres\",\"state_topic\":\"" + stateTopic() +
                   "\",\"value_template\":\"{{ value_json.pressure }}\"," +
                   "\"unit_of_measurement\":\"hPa\",\"device_class\":\"pressure\"," +
                   "\"state_class\":\"measurement\"," + devBase + "}}";
        client_.publish(t.c_str(), p.c_str(), true);
    }
    // 4) BMP280 芯片内部温度（仅参考，可用于判断板级温升）
    {
        String t = "homeassistant/sensor/" + deviceId_ + "_bmp_temp/config";
        String p = String("{\"name\":\"BMP Temperature\",\"unique_id\":\"") + deviceId_ +
                   "_bmp_temp\",\"state_topic\":\"" + stateTopic() +
                   "\",\"value_template\":\"{{ value_json.bmp_temp }}\"," +
                   "\"unit_of_measurement\":\"°C\",\"device_class\":\"temperature\"," +
                   "\"state_class\":\"measurement\"," + devBase + "}}";
        client_.publish(t.c_str(), p.c_str(), true);
    }
    // 5) 采样间隔（HA number 实体）：通过 config topic 下发 JSON 修改
    //    command_template 把 HA 的数值包装成 {"sample_interval": <value>}
    {
        String t = "homeassistant/number/" + deviceId_ + "_sample_interval/config";
        String p = String("{\"name\":\"Sample Interval\",\"unique_id\":\"") + deviceId_ +
                   "_si\",\"command_topic\":\"" + configTopic() +
                   "\",\"command_template\":\"{\\\"sample_interval\\\": {{ value }}}\"," +
                   "\"min\":" + Acquisition::MIN_INTERVAL + "," +
                   "\"max\":" + Acquisition::MAX_UPLOAD_INTV + "," +
                   "\"step\":1000,\"unit_of_measurement\":\"ms\"," + devBase + "}}";
        client_.publish(t.c_str(), p.c_str(), true);
    }
    // 6) 上传间隔（HA number 实体）：同样走 config topic
    {
        String t = "homeassistant/number/" + deviceId_ + "_upload_interval/config";
        String p = String("{\"name\":\"Upload Interval\",\"unique_id\":\"") + deviceId_ +
                   "_ui\",\"command_topic\":\"" + configTopic() +
                   "\",\"command_template\":\"{\\\"upload_interval\\\": {{ value }}}\"," +
                   "\"min\":" + Acquisition::MIN_INTERVAL + "," +
                   "\"max\":" + Acquisition::MAX_UPLOAD_INTV + "," +
                   "\"step\":1000,\"unit_of_measurement\":\"ms\"," + devBase + "}}";
        client_.publish(t.c_str(), p.c_str(), true);
    }
    // 7) 在线状态（binary_sensor）：订阅 status topic，
    //    配合 LWT，设备掉线时会自动变成 offline
    {
        String t = "homeassistant/binary_sensor/" + deviceId_ + "_online/config";
        String p = String("{\"name\":\"Online\",\"unique_id\":\"") + deviceId_ +
                   "_online\",\"state_topic\":\"" + statusTopic() +
                   "\",\"payload_on\":\"online\",\"payload_off\":\"offline\"," +
                   "\"device_class\":\"connectivity\"," + devBase + "}}";
        client_.publish(t.c_str(), p.c_str(), true);
    }

    // 所有 Discovery 消息均 retain=true：Broker 会保存，
    // 因此 HA 重启后无需等设备重连也能立即恢复实体。
    // 若某条 publish 失败（缓冲区/网络瞬时问题），下次重连时 ensureConnected()
    // 会再次调用本函数整体重发。
    Serial.println("[MQTT] HA Discovery 已发布（7 个实体）");
}