/**
 * @file    AppTasks.cpp
 * @brief   四个 FreeRTOS 任务的具体实现（采集 / 上传 / 显示 / OTA）
 *
 * 任务之间的数据流
 * ------------------------------------------------------------
 *   SensorTask ──push()──> RingBuffer ──peek()/commit()──> MqttUploadTask ──publish──> Broker
 *                                └──latest()──> DisplayTask / MqttUploadTask(state)
 *
 * 看门狗约定（每个任务都要遵守）
 * ------------------------------------------------------------
 *   esp_task_wdt_add(NULL)  把"当前任务"注册到任务看门狗；
 *   之后必须保证每 WDT_TIMEOUT(30s) 内至少调用一次 esp_task_wdt_reset()，
 *   否则整机 panic 重启。耗时较长的循环（例如批量上传）要在循环体内主动喂狗。
 */
#include "AppTasks.h"
#include "Config.h"
#include <esp_task_wdt.h>

/* ============================================================
 * 批量上传：从环形缓冲逐块取出并发送
 * 成功则 commit，失败则保留下轮重试
 * ============================================================ */
/**
 * @brief 把缓冲中所有待上传数据按 CHUNK_SIZE 分块发完
 * @param ctx 应用上下文（用到 buffer 与 mqtt）
 *
 * 流程：peek → publish → 成功才 commit。
 * 任何一步失败（MQTT 断开 / publish 返回 false）都直接 break，
 * 未 commit 的数据会在下一轮上传周期自动重试，实现"至少一次"语义。
 *
 * @note 仅由 MqttUploadTask 调用（依赖 PubSubClient 非线程安全的约束）。
 */
static void uploadAllPending(AppContext* ctx) {
    // 栈上批次缓冲：30 × sizeof(SensorRecord) ≈ 600B，
    // MqttUploadTask 栈 8192B 足够；避免动态分配造成堆碎片。
    SensorRecord batch[CHUNK_SIZE];
    int totalSent = 0;

    // 告诉 DisplayTask 当前正在上传（OLED 末行显示 [UP]）
    ctx->uploading = true;
    while (true) {
        // 上传多轮可能超过 30s（弱网重传），循环内持续喂狗
        esp_task_wdt_reset();

        // 每轮都重新确认连接：中途掉线时不再白白调用 publish
        if (!ctx->mqtt->connected()) {
            Serial.println("[Upload] MQTT 断开，暂停上传");
            break;
        }

        int n = ctx->buffer->peek(batch, CHUNK_SIZE);
        if (n == 0) break;   // 缓冲已空

        if (ctx->mqtt->publishBatch(batch, n)) {
            // 只有发布成功才出队，失败则保留数据（下轮重试）
            ctx->buffer->commit(n);
            totalSent += n;
        } else {
            Serial.println("[Upload] 发送失败，保留数据下轮重试");
            break;
        }

        // 每块之间保活 MQTT，避免 keepalive 超时
        // 5×10ms=50ms：既驱动 client_.loop() 收下行消息，又让低优先级任务有机会运行
        for (int k = 0; k < 5; k++) {
            esp_task_wdt_reset();
            ctx->mqtt->loop();
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    ctx->uploading = false;

    if (totalSent > 0) {
        Serial.printf("[Upload] 本轮共上传 %d 条\n", totalSent);
    }
}

/* ============================================================
 * 采集任务（优先级 5）
 *  - 周期采集一条 → 入环形缓冲
 *  - 采集间隔由 ConfigManager 决定，每条记录自带时间戳
 * ============================================================ */
void SensorTask(void* pv) {
    AppContext* ctx = (AppContext*)pv;
    // 注册到任务看门狗：本任务卡死（例如 I2C 总线被拉低）会触发整机重启
    esp_task_wdt_add(NULL);
    // 记录本次唤醒时刻，配合 vTaskDelayUntil 形成固定周期
    TickType_t lastWake = xTaskGetTickCount();

    while (true) {
        esp_task_wdt_reset();
        // 每轮都重新读取配置：MQTT 下发新间隔后，下一次循环立即生效
        RuntimeConfig cfg = ctx->config->get();

        SensorRecord r;
        if (ctx->sensor->read(r)) {
            // 读成功才入队；读失败（I2C 加锁超时）本周期跳过，不产生脏数据
            ctx->buffer->push(r);
        }

        // 使用 vTaskDelayUntil 保证周期稳定，不受 read() 耗时影响
        // （若用 vTaskDelay，实际周期 = 采样耗时 + 间隔，会逐渐漂移）
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(cfg.sampleIntervalMs));
    }
}


/* ============================================================
 * MQTT 任务（优先级 3）
 *  - 每 50ms 保活 MQTT
 *  - 有新数据时推送 state topic（HA 实时显示）
 *  - 每 uploadInterval 触发一次批量上传
 * ============================================================ */
void MqttUploadTask(void* pv) {
    AppContext* ctx = (AppContext*)pv;
    esp_task_wdt_add(NULL);
    TickType_t lastUpload   = xTaskGetTickCount();   // 上次批量上传的时刻
    uint32_t   lastStateTs  = 0;   // 上次推送 state 的记录时间戳

    while (true) {
        esp_task_wdt_reset();

        // ensureConnected() 内部失败时会 vTaskDelay(5s) 退避，
        // 5s < WDT_TIMEOUT(30s)，因此不会触发看门狗。
        ctx->mqtt->ensureConnected();
        // 必须周期性调用 loop()：1) 发送 PINGREQ 维持 keepalive
        //                        2) 收取 config/ota 下行消息并触发回调
        ctx->mqtt->loop();

        // ---------- 实时状态推送 ----------
        // 拿最新一条，如果时间戳比上次推送的更新，就推送到 state topic
        // （用时间戳判重，避免重复发送同一数据）
        // 注意：timestamp==0 表示 NTP 未同步，此时不推 state（避免 HA 收到 1970 年数据），
        //       但数据仍然会进入环形缓冲，随批量上传一起发出。
        SensorRecord latest = ctx->buffer->latest();
        if (latest.timestamp != 0 && latest.timestamp != lastStateTs) {
            if (ctx->mqtt->publishState(latest)) {
                lastStateTs = latest.timestamp;
            }
            // 发布失败则不更新 lastStateTs，下一轮（50ms 后）会自动重试
        }

        // ---------- 定时批量上传 ----------
        // 每轮都重新读配置，因此 MQTT 改了上传间隔后能立即生效
        RuntimeConfig cfg = ctx->config->get();
        TickType_t need = pdMS_TO_TICKS(cfg.uploadIntervalMs);

        // 用"无符号差值"比较 tick，天然处理 32 位计数器回绕
        if ((xTaskGetTickCount() - lastUpload) >= need) {
            lastUpload = xTaskGetTickCount();
            int pending = ctx->buffer->available();
            if (pending > 0) {
                Serial.printf("[Upload] 触发上传，缓冲 %d 条\n", pending);
                uploadAllPending(ctx);   // 内部会一直发到缓冲空或出错
            }
        }

        // 50ms 周期：兼顾 keepalive 及时性、下行命令响应速度与 CPU 占用
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ============================================================
 * OLED 显示任务（优先级 1）
 * ============================================================ */
void DisplayTask(void* pv) {
    AppContext* ctx = (AppContext*)pv;
    esp_task_wdt_add(NULL);

    while (true) {
        esp_task_wdt_reset();
        // latest() 是零开销快照（加锁后拷贝 20 字节）；
        // 首次采集完成前返回全 0 记录，屏幕会显示 0.00，属正常现象。
        SensorRecord d = ctx->buffer->latest();
        ctx->display->show(d, ctx->uploading, ctx->buffer->available());
        // 1s 刷新一次：人眼足够，且避免过度占用 I2C（传感器也要用同一条总线）
        vTaskDelay(pdMS_TO_TICKS(Display::REFRESH_INTERVAL));
    }
}

/* ============================================================
 * OTA 任务（优先级 2）
 * ============================================================ */
void OtaTask(void* pv) {
    AppContext* ctx = (AppContext*)pv;
    esp_task_wdt_add(NULL);

    while (true) {
        esp_task_wdt_reset();
        // loop() 内部：ArduinoOTA.handle() 非阻塞 + 检查 HTTP OTA 请求。
        // 一旦开始 HTTP OTA，doHttpOta() 会长时间阻塞（下载+写 Flash），
        // 期间不喂狗；因此 WDT_TIMEOUT 需留足余量，见 OtaManager.h 的 @warning。
        ctx->ota->loop();
        // 100ms 周期：足够及时响应局域网 OTA 的握手包
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
