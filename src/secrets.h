#pragma once
// ============================================================
// 敏感凭据模板（secrets.h）
//
// 用法：把本文件复制为同目录下的 secrets.h，并填入真实值。
//       config.h 会通过 __has_include 检查它是否存在，缺失则编译报错。
//
// 注意：
//   - 本文件包含 WiFi 密码与 MQTT 服务器地址，请勿提交到公开仓库
//     （建议把 src/secrets.h 加入 .gitignore，仅保留本模板）
//   - 修改凭据后需要重新编译并烧录/OTA 才会生效
// ============================================================
#define WIFI_SSID   "GL-SFT1200-183"  // 2.4GHz 热点名（ESP32-C3 不支持 5GHz）
#define WIFI_PWD    "@wdsj2019"       // WiFi 密码
#define MQTT_SERVER "192.168.8.134"   // MQTT Broker IP / 域名
#define MQTT_PORT   1883              // Broker 端口：1883=明文，8883=TLS
#define MQTT_USER   ""                // 留空表示匿名
#define MQTT_PASS   ""                // 留空表示匿名
