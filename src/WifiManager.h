#pragma once
/**
 * @file    WifiManager.h
 * @brief   WiFi STA 模式连接、启动期等待与运行期断线重连
 *
 * 两个阶段
 * ------------------------------------------------------------
 *  begin()          : 启动阶段使用，阻塞等待联网（最多 120s），失败直接重启
 *  checkReconnect() : 运行阶段由 loopTask 每 10s 调用一次，仅触发 WiFi.reconnect()
 *
 * 为什么启动期允许阻塞
 * ------------------------------------------------------------
 * MQTT、NTP、OTA 都依赖网络，与其让它们各自超时重试，不如在启动时先同步拿到 IP，
 * 后续初始化逻辑可以保持线性、易读。
 */
#include <Arduino.h>

class WifiManager {
public:
    /**
     * @brief 连接 WiFi（阻塞）
     * @param ssid     热点名（来自 secrets.h）
     * @param pwd      密码（来自 secrets.h）
     * @param hostname 设备主机名，供路由器 DHCP 列表/mDNS 显示
     * @note  120s 内连不上会 ESP.restart()，由硬件看门狗之外的应用逻辑兜底。
     */
    void begin(const char* ssid, const char* pwd, const char* hostname);

    /**
     * @brief 检查并触发重连（非阻塞）
     * @note  只调用 WiFi.reconnect()，实际连接由 WiFi 协议栈异步完成；
     *        调用频率不宜过高（本工程 10s 一次）。
     */
    void checkReconnect();

    /** @brief 当前是否已连接（WL_CONNECTED） */
    bool connected();
};
