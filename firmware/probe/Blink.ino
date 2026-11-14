/*
 * Blink - ESP32-S3-DevKitC-1 板载 LED 一亮一灭
 *
 * 引脚来源（不是猜的）：从原厂固件 16MB Flash 备份的 VFS 分区里
 * 提取出 MicroPython 的 main.py，内容为：
 *     pin = Pin(48, Pin.OUT)
 *     np  = NeoPixel(pin, 1)
 * 即板载 LED 是 1 颗可寻址 WS2812，数据脚接 GPIO48（DevKitC-1 v1.0）。
 * 该引脚与 arduino-esp32 3.3.12 variants/esp32s3 的 PIN_RGB_LED=48 一致。
 *
 * 注意：WS2812 不是普通 GPIO LED，digitalWrite(48, HIGH/LOW) 点不亮它。
 * 内核把板载 RGB 引脚编码成 RGB_BUILTIN = SOC_GPIO_PIN_COUNT + 48，
 * rgbLedWrite() 识别该编码后走隐藏 RGB 驱动发送单线归零码时序。
 * (2.x 内核里这个函数叫 neopixelWrite，3.x 已改名 rgbLedWrite)
 *
 * 目标板实测：ESP32-S3 QFN56 rev v0.2 / 16MB Flash / 8MB PSRAM / COM4
 * 烧录参数照抄原固件镜像头：DIO / 80MHz
 */

#define LED_GPIO 48  // 板上实际接的 I/O 口

#include <Arduino.h>

// 编译期断言：所选板型的 RGB 引脚必须与实测一致，换板/换版本时立刻暴露
#if defined(PIN_RGB_LED) && (PIN_RGB_LED != LED_GPIO)
#error "板型定义的 RGB 引脚与实测 GPIO48 不符，请复核 variants 或改 LED_GPIO"
#endif

static const uint8_t  LED_COUNT = 1;     // 1 颗 WS2812
static const uint32_t BLINK_MS  = 1000;  // 亮 1s / 灭 1s
static const uint8_t  ON_LEVEL  = 64;    // 50% 亮度级别，不刺眼

static uint32_t cycle = 0;

void setup() {
  Serial.begin(115200);
  delay(200);                            // 等 USB-UART 稳定，保证横幅不丢

  Serial.println();
  Serial.println("=========================================");
  Serial.println(" Blink | ESP32-S3-DevKitC-1");
  Serial.printf(" LED  : GPIO%d (WS2812 x%d), RGB_BUILTIN=%d\r\n",
                LED_GPIO, LED_COUNT, (int)RGB_BUILTIN);
  Serial.printf(" Chip : %s rev%d  %dMHz\r\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getCpuFreqMHz());
  Serial.printf(" Flash: %u MB\r\n", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  Serial.printf(" Mode : ON %lums / OFF %lums\r\n",
                (unsigned long)BLINK_MS, (unsigned long)BLINK_MS);
  Serial.println("=========================================");

  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);     // 上电先确保是灭
}

void loop() {
  // 亮：蓝色
  rgbLedWrite(RGB_BUILTIN, 0, 0, ON_LEVEL);
  Serial.printf("[%6lu ms] #%lu LED ON\r\n",
                (unsigned long)millis(), (unsigned long)++cycle);
  delay(BLINK_MS);

  // 灭：三通道全 0
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
  Serial.printf("[%6lu ms] LED OFF\r\n", (unsigned long)millis());
  delay(BLINK_MS);
}
