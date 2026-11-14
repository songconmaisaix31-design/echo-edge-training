/*
 * Probe - ESP32-S3 外接屏/触摸模块硬件探测
 *
 * 用途：用户用杜邦线把触摸屏飞线接到板子上，但屏型号与引脚未知。
 *       本固件不依赖任何屏驱动，只做两件事来实证「电气上连没连上」：
 *         1) 在若干常见 SDA/SCL 引脚对上扫描 I2C 地址 0x08..0x77
 *            (GT911=0x5D/0x14, CST816S=0x15, FT6236/FT5x06=0x38, ...)
 *         2) 给所有安全 GPIO 拍输入电平快照，看哪些线被外设拉高/拉低
 *
 * 关键点：板子现在通过原生 USB 口(VID_303A:1001 / COM5)连接，
 *         所以编译必须开 CDCOnBoot=cdc，让 Serial 走 USB CDC，
 *         否则日志仍走 UART0(原 COM4)，这里一行都看不到。
 *
 * 安全引脚：刻意避开 strapping(0/3/45/46)、USB(19/20)、
 *           flash/octal-PSRAM(26..37)、UART0(43/44)、板载 RGB LED(48)。
 */

#include <Arduino.h>
#include <Wire.h>

static const int SAFE_PINS[] = {
  1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21,
  38, 39, 40, 41, 42, 47
};
static const int SAFE_COUNT = sizeof(SAFE_PINS) / sizeof(SAFE_PINS[0]);

struct PinPair { int sda, scl; };
static const PinPair PAIRS[] = {
  {8, 9}, {4, 5}, {1, 2}, {6, 7}, {10, 11}, {12, 13}, {14, 15}, {16, 17},
  {18, 21}, {38, 39}, {40, 41}, {42, 47}, {9, 10}, {5, 6}, {2, 4}, {13, 14}
};
static const int PAIR_COUNT = sizeof(PAIRS) / sizeof(PAIRS[0]);

// 常见触摸控制器地址，命中时额外标注
static const char *knownAddr(uint8_t a) {
  switch (a) {
    case 0x14: return "GT911/CST816S?";
    case 0x15: return "CST816S?";
    case 0x38: return "FT6236/FT5x06?";
    case 0x48: return "GT911 alt?";
    case 0x5D: return "GT911?";
    default:   return "";
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);                       // 等 USB CDC 枚举完成，避免丢横幅

  Serial.println();
  Serial.println("=========================================");
  Serial.println(" Probe | ESP32-S3 外接屏/触摸硬件探测");
  Serial.printf(" Chip : %s rev%d  %dMHz\r\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getCpuFreqMHz());
  Serial.printf(" Flash: %u MB  PSRAM: %u bytes\r\n",
                (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)),
                (unsigned)ESP.getPsramSize());
  Serial.println("=========================================");

  // --- 1. GPIO 输入电平快照 ---
  Serial.println();
  Serial.println("[1] GPIO 输入电平快照 (1=高 / 0=低)");
  char bitmap[128];
  int n = 0;
  for (int i = 0; i < SAFE_COUNT; i++) {
    int p = SAFE_PINS[i];
    pinMode(p, INPUT);
    int v = digitalRead(p);
    n += snprintf(bitmap + n, sizeof(bitmap) - n, "%s%d:%d", i ? " " : "", p, v);
  }
  Serial.printf("    %s\r\n", bitmap);
  Serial.println("    说明: 若某些脚被外设上拉为 1，通常是 CS / RST / INT / SDA / SCL");

  // --- 2. I2C 地址扫描 ---
  Serial.println();
  Serial.println("[2] I2C 扫描 (常见 SDA/SCL 引脚对, 地址 0x08..0x77)");
  int total = 0;
  for (int k = 0; k < PAIR_COUNT; k++) {
    int sda = PAIRS[k].sda, scl = PAIRS[k].scl;
    Wire.end();
    if (!Wire.begin(sda, scl, 100000)) {
      Serial.printf("    SDA=%-2d SCL=%-2d  Wire.begin 失败\r\n", sda, scl);
      continue;
    }
    delay(5);
    uint8_t found[8];
    int nf = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0 && nf < 8) found[nf++] = addr;
    }
    if (nf) {
      total += nf;
      Serial.printf("    SDA=%-2d SCL=%-2d  >>> 命中: ", sda, scl);
      for (int i = 0; i < nf; i++) {
        Serial.printf("0x%02X(%s) ", found[i], knownAddr(found[i]));
      }
      Serial.println();
    }
  }
  Wire.end();
  if (total == 0) {
    Serial.println("    未在候选引脚对上发现 I2C 设备。");
    Serial.println("    => 屏可能是纯 SPI (ST7789/ILI9341 + XPT2046)，或接在其它 GPIO 上。");
  }

  Serial.println();
  Serial.println("=========================================");
  Serial.println(" 探测结束，每 5s 心跳一次");
  Serial.println("=========================================");
}

static uint32_t beat = 0;
void loop() {
  Serial.printf("[%6lu ms] heartbeat #%lu\r\n",
                (unsigned long)millis(), (unsigned long)++beat);
  delay(5000);
}
