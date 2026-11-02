# ESP32-C3 环境监测节点（EnvSensor）

基于 **ESP32-C3 + Arduino + PlatformIO** 的环境数据采集节点：周期采集温度/湿度/气压，
本地 OLED 显示，通过 MQTT 上报到 Broker，并支持 Home Assistant 自动发现与远程 OTA 升级。

---

## 1. 功能概览

| 功能 | 说明 |
| --- | --- |
| 环境采集 | AHT20（温湿度）+ BMP280（气压/芯片温度），默认 1 秒一次 |
| 断电/断网缓存 | 环形缓冲 300 条（约 5 分钟），恢复后按时间顺序补传 |
| 实时上报 | 每条新数据发布到 `state` topic，供 HA 实时显示 |
| 批量上报 | 每 10 秒把缓冲中的数据打包发布到 `data` topic（可配） |
| 本地显示 | 128×64 OLED：4 行数据 + 上传状态 + 待传条数 |
| 远程配置 | MQTT 下发 JSON 动态修改采样/上传间隔，存 NVS 重启不丢 |
| 双通道 OTA | 局域网 ArduinoOTA + MQTT 触发 HTTP 下载升级 |
| 可靠性 | 任务看门狗、WiFi 自动重连、MQTT LWT 离线状态、I2C 总线互斥 |
| HA 集成 | 自动生成 7 个实体（4 传感器 + 2 可调数值 + 1 在线状态） |

---

## 2. 硬件与接线

| 器件 | 型号 | I2C 地址 | 说明 |
| --- | --- | --- | --- |
| MCU | ESP32-C3 DevKitM-1 | — | 单核 RISC-V，160MHz，4MB Flash |
| 温湿度 | AHT20 | 0x38 | 走 I2C |
| 气压 | BMP280 | 0x76 | SDO 接地=0x76，接 VCC=0x77 |
| 显示屏 | SSD1306 128×64 | 0x3C | 走 I2C |
| 状态灯 | LED | GPIO10 | 心跳指示 |

| 信号 | GPIO |
| --- | --- |
| SDA | 8 |
| SCL | 9 |
| LED | 10 |

> 引脚与屏幕尺寸改 `src/config.h` 的 `Pins` / `Display`；BMP280 地址改 `Pins::BMP280_ADDR`，
> OLED 地址在 `OledDisplay.cpp` 中固定为 `0x3C`。

---

## 3. 目录结构

```
.
├── platformio.ini          # 工程配置：开发板、编译选项、依赖库
├── src/
│   ├── main.cpp            # 入口：构造 AppController，转发 setup/loop
│   ├── config.h            # 全局编译期配置（引脚、Topic、采集参数、NTP、WDT）
│   ├── secrets.h           # 敏感凭据（WiFi/MQTT），需自行创建，勿提交
│   ├── AppController.h/.cpp# 顶层编排：初始化顺序、创建任务、主循环（WiFi/LED）
│   ├── AppTasks.h/.cpp     # 四个 FreeRTOS 任务 + 共享上下文 AppContext
│   ├── I2cBus.h/.cpp       # I2C 总线互斥（三器件共享一条总线）
│   ├── EnvironmentSensor.h/.cpp # AHT20 + BMP280 聚合读取
│   ├── OledDisplay.h/.cpp  # SSD1306 显示封装
│   ├── RingBuffer.h/.cpp   # 环形缓冲 + SensorRecord 定义
│   ├── ConfigManager.h/.cpp# 运行期配置（NVS + JSON 下发）
│   ├── MqttManager.h/.cpp  # MQTT 连接/发布/订阅/HA Discovery
│   ├── WifiManager.h/.cpp  # WiFi 连接与断线重连
│   └── OtaManager.h/.cpp   # ArduinoOTA + HTTP OTA
└── lib/
    └── AHT20/              # 本地 AHT20 驱动库（工程内置，可修改）
```

---

## 4. 架构与数据流

```
  ┌──────────────── loopTask (prio 1) ─────────────────┐
  │ WiFi 重连检查(10s) + LED 心跳(在线1Hz/离线5Hz)     │
  └────────────────────────────────────────────────────┘

  AHT20 ─┐
         ├─ I2C(锁) ─> SensorTask(prio 5) ──push()──> RingBuffer(300条)
  BMP280 ┘                                               │
                                    ┌────────────────────┤
                                    │                    │
                    latest() ───────┘                    └───── peek()/commit()
                        │                                         │
                DisplayTask(prio 1) ──> SSD1306        MqttUploadTask(prio 3)
                                                                │
                                     ┌──────────────────────────┤
                                     │ publishState（每条新数据 → state）
                                     │ publishBatch（每 uploadInterval → data）
                                     └──────────────────────────┘
                                                                │
                                                  MQTT Broker ──> Home Assistant

  MQTT config/ota 下行 ─> onMessage() ─回调─> ConfigManager(NVS) / OtaManager(排队)
  OtaTask(prio 2) ─ ArduinoOTA.handle() + HTTP OTA 下载写入
```

### 任务优先级

| 任务 | 优先级 | 栈(B) | 周期 | 职责 |
| --- | --- | --- | --- | --- |
| SensorTask | 5 | 3072 | sampleInterval(默认1s) | 采样入缓冲 |
| MqttUploadTask | 3 | 8192 | 50ms 循环 | MQTT 保活、实时推送、定时批量上传 |
| OtaTask | 2 | 4096 | 100ms | ArduinoOTA / HTTP OTA |
| DisplayTask | 1 | 3072 | 1s | OLED 刷新 |

> 单核靠优先级抢占：采样最优先，显示最低。所有任务都注册了任务看门狗（30s），
> 卡死会 panic 重启。

---

## 5. MQTT 约定

Topic 前缀：`env-monitor/<DEVICE_ID>`（`DEVICE_ID` 见 `config.h`，多设备必须唯一）

| Topic | 方向 | retain | 说明 |
| --- | --- | --- | --- |
| `env-monitor/<ID>/state` | 发布 | 否 | 最新一条实时数据 |
| `env-monitor/<ID>/data` | 发布 | 否 | 批量历史数据 |
| `env-monitor/<ID>/config` | 订阅 | — | 修改采样/上传间隔 |
| `env-monitor/<ID>/status` | 发布 | 是 | `online`/`offline`（含 LWT） |
| `env-monitor/<ID>/ota` | 订阅 | — | 触发 HTTP OTA |

### 实时数据（state）

```json
{"timestamp":1730000000,"temperature":25.31,"humidity":60.2,"pressure":1009.12,"bmp_temp":26.05}
```

### 批量数据（data）

```json
{
  "device_id": "C3SA57231",
  "record_count": 10,
  "data": [
    {"ts":1730000000,"temperature":25.31,"humidity":60.2,"pressure":1009.12,"bmp_temp":26.05}
  ]
}
```

### 修改采集/上传间隔（config）

两个字段都可选，缺省沿用当前值；单位毫秒。

```json
{"sample_interval": 5000, "upload_interval": 60000}
```

校验规则：`sample >= 1000`，`sample <= upload <= 600000`，否则整条拒绝并保持旧配置。

### 触发远程升级（ota）

```json
{"url": "http://192.168.8.10/firmware.bin"}
```

`firmware.bin` 由 PlatformIO 生成于 `.pio/build/esp32-c3-devkitm-1/firmware.bin`，
用任意 HTTP 服务（如 `python -m http.server`）提供下载即可。

---

## 6. Home Assistant 集成

设备连接 Broker 后自动发布 MQTT Discovery，HA 中无需手写 YAML，
自动出现 1 台设备 + 7 个实体：

| 实体 | 类型 | 数据来源 |
| --- | --- | --- |
| Temperature | sensor | `state.temperature` |
| Humidity | sensor | `state.humidity` |
| Pressure | sensor | `state.pressure` |
| BMP Temperature | sensor | `state.bmp_temp` |
| Sample Interval | number | 写 `config` topic |
| Upload Interval | number | 写 `config` topic |
| Online | binary_sensor | `status` topic + LWT |

> 前提：HA 的 MQTT 集成已启用自动发现（默认前缀 `homeassistant`）。

---

## 7. 编译与烧录

```bash
# 1) 准备凭据：复制/编辑 src/secrets.h，填入真实 WiFi 与 MQTT 信息
# 2) 编译
pio run
# 3) 串口烧录（首次）
pio run -t upload
# 4) 查看日志（115200）
pio device monitor
```

局域网 OTA（无需连线）：

```bash
pio run -t upload --upload-port <设备IP>
# 或 Arduino IDE：工具 → 端口 → 网络端口
# OTA 密码：ota_pass（见 AppController::begin，量产前建议移入 secrets.h）
```

---

## 8. 运行日志示例

```
===== ESP32-C3 EnvNode 启动 =====
[Init] I2C 总线已启动
[Sensor] AHT20 初始化成功
[Sensor] BMP280 初始化成功
[OLED] 初始化成功
[WiFi] 已连接, IP=192.168.8.50
[NTP] 时间已同步: 2026-10-29 10:23:45
[MQTT] 连接成功
[MQTT] HA Discovery 已发布
[Init] 所有任务已创建，系统就绪
[Upload] 触发上传，缓冲 10 条
[Upload] 本轮共上传 10 条
```

---

## 9. 常见问题排查

| 现象 | 可能原因 | 处理 |
| --- | --- | --- |
| OLED 全黑 | 地址不是 0x3C / 接线反 | 地址在 `OledDisplay.cpp` 中固定为 0x3C；确认 3.3V 供电与 SDA/SCL |
| 气压恒为 0 | BMP280 地址不是 0x76 | `SDO` 接地=0x76，或改 `config.h` 中 `Pins::BMP280_ADDR` |
| 数据不上报 | MQTT 连不上 | 串口看 `[MQTT]` 日志；确认 Broker IP/端口/账号 |
| HA 没有实体 | 自动发现未开启 / 前缀不同 | 检查 HA MQTT 配置；确认 `homeassistant` 前缀 |
| 时间戳全为 0 | NTP 未同步（可能 UDP 123 被拦） | 改用国内 NTP 或放行端口 |
| 频繁重启 | 看门狗超时 | 看 panic 日志；确认传感器未卡住 I2C |
| 缓冲一直满 | 上传失败（publish 返回 false） | 检查 payload 是否超过 Broker 限制，或调大 CHUNK 间隔 |
| OTA 失败 | URL 不可达 / 固件不匹配 | 确认 `.bin` 与本设备分区表一致，且 URL 可直接下载 |

---

## 10. 已知限制与可扩展点

- 缓冲只在 RAM（300 条），深度掉电会丢失未上传数据；如需持久化可加 SPIFFS/NVS 落盘。
- 批量数据未做 Broker 端去重，弱网重传可能产生少量重复（数据带 `ts`，服务端可按时间戳去重）。
- 单一 `DEVICE_ID` 硬编码；部署多节点需在 `config.h` 中区分或改用 MAC 派生。
- BMP280 芯片温度与 AHT20 空气温度不是同一物理量，不要相互替代。
- 未启用 TLS 时 MQTT 明文传输，生产环境建议改 8883 + 证书。

