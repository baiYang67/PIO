/**
 * @file    RingBuffer.cpp
 * @brief   环形缓冲实现：head_/tail_ 取模回绕 + 计数器 + 互斥锁
 *
 * 索引约定（务必保持）
 * ------------------------------------------------------------
 *   tail_ ──> 最老的一条（下一次出队位置）
 *   head_ ──> 下一条写入位置（最老 + count_）
 *   count_ ──> 有效条数
 * 因此：第 i 条（0=最老）位于 buf_[(tail_ + i) % RING_BUFFER_SIZE]。
 *
 * 锁使用原则
 * ------------------------------------------------------------
 * 临界区内只做内存拷贝和下标运算，耗时在微秒级；
 * 加锁超时直接放弃本次操作（宁可丢一条采样，也不让任务卡死）。
 */
#include "RingBuffer.h"

void RingBuffer::begin() {
    // 创建互斥锁；此后一直有效，不释放
    mutex_ = xSemaphoreCreateMutex();
}

void RingBuffer::push(const SensorRecord& r) {
    // 采集任务允许 50ms 短等待；上传任务持锁时间极短（微秒级）
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        Serial.println("[Ring] push 加锁超时，丢弃本次采集");
        return;
    }

    // 缓冲区满：先丢掉最老的一条，为新人腾位置（不阻塞、不扩容）
    if (count_ == RING_BUFFER_SIZE) {
        tail_ = (tail_ + 1) % RING_BUFFER_SIZE;
        count_--;
        dropped_++;
        // 每丢 RING_DROP_WARN_INTERVAL 条提示一次，避免长期断网时刷屏
        if (dropped_ % RING_DROP_WARN_INTERVAL == 1) {
            Serial.printf("[Ring] 缓冲区满，累计丢弃 %u 条最老数据\n", dropped_);
        }
    }

    buf_[head_] = r;                       // 写入新数据
    head_ = (head_ + 1) % RING_BUFFER_SIZE; // 写入指针回绕
    count_++;
    latest_ = r;                            // 同步最新值快照，供 OLED/state 使用

    xSemaphoreGive(mutex_);
}

int RingBuffer::peek(SensorRecord* out, int maxN) {
    int n = 0;
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return 0;

    // 取 min(现有条数, 数组容量)
    n = (count_ < maxN) ? count_ : maxN;
    // 从 tail_ 开始按时间顺序拷贝，保证补传顺序与采集顺序一致
    for (int i = 0; i < n; i++) {
        out[i] = buf_[(tail_ + i) % RING_BUFFER_SIZE];
    }

    xSemaphoreGive(mutex_);
    return n;
}

void RingBuffer::commit(int n) {
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return;

    // 只在"确认已发送成功"后调用：从最老端移除 n 条
    // count_ > 0 兜底，防止误传超大 n 导致负数
    for (int i = 0; i < n && count_ > 0; i++) {
        tail_ = (tail_ + 1) % RING_BUFFER_SIZE;
        count_--;
    }

    xSemaphoreGive(mutex_);
}

int RingBuffer::available() {
    int c = 0;
    // 只读取一个 int，等待时间取短一些，避免影响实时性
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        c = count_;
        xSemaphoreGive(mutex_);
    }
    return c;
}

SensorRecord RingBuffer::latest() {
    SensorRecord r = {};
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        r = latest_;
        xSemaphoreGive(mutex_);
    }
    return r;   // 加锁失败时返回全 0 记录，调用方据此判断"无数据"
}

uint32_t RingBuffer::droppedCount() {
    uint32_t d = 0;
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        d = dropped_;
        xSemaphoreGive(mutex_);
    }
    return d;
}
