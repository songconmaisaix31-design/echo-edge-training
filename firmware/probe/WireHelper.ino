/*
 * 接线助手 —— 给硬件新手用的实时 I2C 检测固件
 *
 * 只做一件事：每秒扫一次 I2C 总线，把「新接上的模块」用中文报出来。
 * 全程只读地址、不写任何寄存器，接错线也不会损坏模块。
 *
 * 两个 USB 口同时输出：
 *   - HWCDCSerial : 原生 USB 口（VID 303A，通常 COM5）
 *   - Serial0     : UART0 / CH343（通常 COM4）
 * 编译必须开 CDCOnBoot=cdc。
 *
 * 板载 WS2812 (GPIO48) 当状态灯：
 *   没检测到模块 → 暗蓝慢闪；1 个 → 绿常亮；2 个以上 → 亮绿慢闪
 */

#include <Arduino.h>
#include <Wire.h>
#include <stdarg.h>
#include <stdio.h>

static const int SDA_PIN = 8;   // 数据线，接模块的 SDA
static const int SCL_PIN = 9;   // 时钟线，接模块的 SCL

static uint8_t last[16];
static int last_n = 0;
static uint32_t round_no = 0;

static void both(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  HWCDCSerial.print(buf);   // 原生 USB 口
  Serial0.print(buf);       // CH343 / UART0 口
}

static const char *guess(uint8_t a) {
  switch (a) {
    case 0x3C: case 0x3D: return "很可能是 OLED 显示屏";
    case 0x44: case 0x45: return "很可能是温湿度传感器 (SHT3x 类)";
    case 0x76: case 0x77: return "很可能是气压传感器 (BMP280 / BME280)";
    case 0x57:            return "很可能是心率 / PPG 模块 (MAX30102 类)";
    case 0x68:            return "可能是 IMU 或 RTC 模块";
    case 0x23:            return "可能是光照传感器 (BH1750)";
    default:
      if (a >= 0x50 && a <= 0x57) return "可能是存储芯片 (EEPROM)";
      return "不认识的模块，记下这个地址，我们查丝印";
  }
}

static int scan_bus(uint8_t *out, int cap) {
  int n = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0 && n < cap) out[n++] = a;
  }
  return n;
}

static bool contains(const uint8_t *arr, int n, uint8_t v) {
  for (int i = 0; i < n; i++) if (arr[i] == v) return true;
  return false;
}

static void set_led(int n, bool blink_on) {
  if (n == 0) {
    rgbLedWrite(RGB_BUILTIN, 0, 0, blink_on ? 20 : 0);      // 暗蓝慢闪 = 等待中
  } else if (n == 1) {
    rgbLedWrite(RGB_BUILTIN, 0, 60, 0);                     // 绿常亮 = 1 个模块
  } else {
    rgbLedWrite(RGB_BUILTIN, 0, blink_on ? 90 : 25, 0);      // 亮绿慢闪 = 多个模块
  }
}

void setup() {
  HWCDCSerial.begin(115200);
  Serial0.begin(115200);
  delay(1200);                     // 等 USB 枚举，保证开机横幅不丢

  both("\r\n=========================================\r\n");
  both("  接线助手 已启动\r\n");
  both("=========================================\r\n");
  both("  信号线定义： GPIO8 = SDA，GPIO9 = SCL\r\n");
  both("  电源：模块 VCC 只接 3V3，绝不接 5V\r\n");
  both("  插拔杜邦线前，请先拔掉 USB 线\r\n");
  both("  本程序只读取地址，不会写模块，接错也不烧\r\n");
  both("=========================================\r\n\r\n");

  Wire.begin(SDA_PIN, SCL_PIN, 100000);
  Wire.setTimeout(5);              // 单个地址最多等 5 ms，避免总线挂死卡住
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
}

void loop() {
  round_no++;
  uint8_t now[16];
  int n = scan_bus(now, 16);
  bool blink_on = (round_no % 2) == 0;

  // 报「新接上」的
  for (int i = 0; i < n; i++) {
    if (!contains(last, last_n, now[i])) {
      both("【接上了】发现新模块，地址 0x%02X —— %s\r\n", now[i], guess(now[i]));
      both("           当前在线 %d 个模块：", n);
      for (int j = 0; j < n; j++) both("0x%02X ", now[j]);
      both("\r\n");
    }
  }

  // 报「掉线」的
  for (int i = 0; i < last_n; i++) {
    if (!contains(now, n, last[i])) {
      both("【断开了】地址 0x%02X 不见了 —— 检查这根线是否松了\r\n", last[i]);
    }
  }

  if (n == 0 && (round_no % 10) == 1) {
    both("【等待中】还没检测到模块（这是正常的）。接好 4 根线再上电即可。\r\n");
    both("          如果已经接了线却一直没反应：先交换 SDA 和 SCL 两根线试试。\r\n");
  }

  if (n > 0 && (round_no % 15) == 0) {
    both("【状态】当前在线 %d 个模块：", n);
    for (int j = 0; j < n; j++) both("0x%02X ", now[j]);
    both("\r\n");
  }

  set_led(n, blink_on);
  memcpy(last, now, n);
  last_n = n;
  delay(1000);
}
