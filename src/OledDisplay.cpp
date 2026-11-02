/**
 * @file    OledDisplay.cpp
 * @brief   SSD1306 显示实现
 *
 * 驱动对象为文件内 static：整个工程只有一块屏，且必须与传感器共用 I2C 锁。
 */
#include "OledDisplay.h"
#include "I2cBus.h"
#include "Config.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// 参数含义：宽、高、I2C 总线指针、复位引脚（-1 = 无独立复位脚）
// I2C 地址 0x3C 是绝大多数 0.96" SSD1306 模组的默认地址（备选 0x3D）
static Adafruit_SSD1306 oled(
    Display::WIDTH, Display::HEIGHT, &Wire, Display::RESET_PIN);

bool OledDisplay::begin() {
    // SSD1306_SWITCHCAPVCC = 使用芯片内部电荷泵产生 7.5V 驱动电压（模组无需外部高压）
    if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("[OLED] 初始化失败");
        return false;
    }
    oled.setTextSize(1);        // 6×8 像素字体，128 宽可显示 21 个字符
    oled.setTextColor(WHITE);   // 单色屏：WHITE=点亮，BLACK=熄灭
    oled.clearDisplay();
    oled.display();             // 把显存推到屏幕（clearDisplay 只清 RAM）
    Serial.println("[OLED] 初始化成功");
    return true;
}

void OledDisplay::show(const SensorRecord& d, bool uploading, int bufferedCount) {
    // 显示刷新也走 I2C，必须加锁；超时（例如传感器正在读）则跳过本次刷新，
    // 下一轮 1s 后自然会再刷，不会造成可见问题。
    if (!I2cBus::lock(pdMS_TO_TICKS(200))) return;

    // 本工程采用"整屏重绘"而非局部刷新：实现简单，且 1Hz 刷新下 I2C 负载可接受。
    oled.clearDisplay();        // 只清显存
    oled.setCursor(0, 0);       // 光标移到左上角
    oled.printf("Temp : %.2f C\n", d.temp);
    oled.printf("Humi : %.2f %%\n", d.humi);   // printf 中输出 % 需写成 %%
    oled.printf("Press: %.2f hPa\n", d.press);
    oled.printf("BMP  : %.2f C\n", d.bmpTemp);

    // 最后一行：状态 + 缓冲条数（y=56，即第 8 行，避免与第 4 行重叠）
    oled.setCursor(0, 56);
    if (uploading) {
        // 正在批量上传：提示网络活动
        oled.printf("[UP] buf:%d", bufferedCount);
    } else {
        // 空闲：buffer 数字持续增长说明 MQTT 没连上或上传失败
        oled.printf("[OK] buf:%d", bufferedCount);
    }
    oled.display();             // 一次性把显存刷到屏幕，减少 I2C 事务

    I2cBus::unlock();
}
