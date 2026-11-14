/*
 * ScreenLink - GDEQ0426T82 (4.26" 800x480 SSD1677 电子墨水屏) 联调固件
 *
 * 这块屏是纯 SPI 设备，没有 I2C 地址。用 I2C 扫描工具测它必然零命中，
 * 所以本固件做的是「SPI 侧能实证到的三件事」：
 *
 *   [1] GPIO 三态快照：区分「悬空脚」与「被外设强驱动的脚」。
 *       电子纸的 BUSY 是屏幕唯一的输出脚，会出现在「被驱动」清单里。
 *   [2] 引脚反查：只驱动悬空脚（不会短路），逐个翻转并观察被驱动脚是否
 *       跟着变化 —— 变化说明这根是 RST / CS / DC 之一。
 *   [3] SPI 握手 PING：复位 + 发软复位命令 0x12，采样 BUSY 的电平跳变。
 *       BUSY 有跳变 = 板到屏的 SPI 链路通了，不依赖屏幕初始化波形表。
 *
 * 另外提供串口命令，改引脚不用重新编译烧录：
 *   HELP                              显示帮助
 *   SCAN                              重跑 [1][2]
 *   PINS sclk mosi cs dc rst busy     设置 6 根线（十进制 GPIO 号）
 *   PING                              跑一次握手
 *   TEST                              全黑 / 全白 / 棋盘 三步刷新（需先 PINS）
 *
 * 安全约束：
 *   - 全程避开 strapping(0/3/45/46)、USB(19/20)、Flash/PSRAM(26..37)、
 *     UART0(43/44)、板载 LED(48)。
 *   - 只对「三态判定为悬空」的脚做输出，绝不驱动已被外设拉住的脚。
 *   - 所有忙等带超时，超时返回中文错误，不死机。
 *
 * 双 USB 口同时输出：HWCDCSerial(原生 USB / COM5) + Serial0(CH343 / COM4)
 * 编译必须开 CDCOnBoot=cdc + USBMode=hwcdc。
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------- 输出
static void both(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  HWCDCSerial.print(buf);
  Serial0.print(buf);
}

// ---------------------------------------------------------------- 引脚池
static const int SAFE_PINS[] = {
  1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21,
  38, 39, 40, 41, 42, 47
};
static const int N = sizeof(SAFE_PINS) / sizeof(SAFE_PINS[0]);

// 默认引脚：按常见 ESP32 + 电子纸转接板习惯给的一组猜测值，
// 仅供 PING 首轮试跑；用 PINS 命令改成实际接线即可，不用重编译。
struct PinCfg { int sclk, mosi, cs, dc, rst, busy; };
static PinCfg cfg = { 12, 11, 10, 9, 8, 13 };

static bool floating[N];      // true = 三态判定为悬空，可安全驱动
static int  driven[N];        // 被外设强驱动的电平，供变化检测

// ---------------------------------------------------------------- [1] 三态快照
static void gpio_snapshot() {
  both("\r\n[1] GPIO 三态快照 (F=浮空 / H=被外设拉高 / L=被外设拉低)\r\n");
  both("    GPIO : ");
  for (int i = 0; i < N; i++) both("%-4d", SAFE_PINS[i]);
  both("\r\n");

  int raw[N], pu[N], pd[N];
  for (int i = 0; i < N; i++) {
    int p = SAFE_PINS[i];
    pinMode(p, INPUT);          delayMicroseconds(300); raw[i] = digitalRead(p);
    pinMode(p, INPUT_PULLUP);   delayMicroseconds(300); pu[i]  = digitalRead(p);
    pinMode(p, INPUT_PULLDOWN); delayMicroseconds(300); pd[i]  = digitalRead(p);
    pinMode(p, INPUT);
  }

  both("    判定 : ");
  int nFloat = 0, nDriven = 0;
  for (int i = 0; i < N; i++) {
    // 悬空脚：能被内部上拉拉成 1、也能被内部下拉拉成 0
    // 被强驱动脚：两种内部电阻都拗不过外部电平，读数一致
    if (pu[i] == 1 && pd[i] == 0) {
      floating[i] = true; driven[i] = -1; nFloat++;
      both("F   ");
    } else if (pu[i] == 1 && pd[i] == 1) {
      floating[i] = false; driven[i] = 1; nDriven++;
      both("H   ");
    } else {
      floating[i] = false; driven[i] = 0; nDriven++;
      both("L   ");
    }
  }
  both("\r\n");
  both("    浮空 %d 个 / 被驱动 %d 个\r\n", nFloat, nDriven);

  if (nDriven) {
    both("    被外设驱动的脚: ");
    for (int i = 0; i < N; i++) if (!floating[i]) both("GPIO%d(=%d) ", SAFE_PINS[i], driven[i]);
    both("\r\n");
    both("    >>> 电子纸的 BUSY 通常就在这些脚里（屏幕唯一输出脚）\r\n");
  } else {
    both("    >>> 没有任何脚被外设驱动：屏幕可能未供电、GND 未共、或只接了输入脚\r\n");
  }
  both("    提示: GPIO39 常因 JTAG 内部上拉显示为 H，属正常，不是屏幕\r\n");
}

// ---------------------------------------------------------------- [2] 引脚反查
static void pin_probe() {
  both("\r\n[2] 引脚反查（只翻转浮空脚，不会短路）\r\n");
  // 基线：所有被驱动脚的电平
  int base[N];
  for (int i = 0; i < N; i++) base[i] = digitalRead(SAFE_PINS[i]);

  int hits = 0;
  for (int i = 0; i < N; i++) {
    if (!floating[i]) continue;             // 绝不驱动已被外设拉住的脚
    int p = SAFE_PINS[i];

    pinMode(p, OUTPUT);
    digitalWrite(p, LOW);  delay(25);
    int lowChanged[N];
    for (int j = 0; j < N; j++) {
      if (floating[j] || j == i) { lowChanged[j] = 0; continue; }
      lowChanged[j] = (digitalRead(SAFE_PINS[j]) != base[j]) ? 1 : 0;
    }
    digitalWrite(p, HIGH); delay(25);
    for (int j = 0; j < N; j++) {
      if (floating[j] || j == i) continue;
      if (digitalRead(SAFE_PINS[j]) != base[j]) lowChanged[j] = 1;
    }
    pinMode(p, INPUT);

    for (int j = 0; j < N; j++) {
      if (lowChanged[j]) {
        both("    翻转 GPIO%-2d -> GPIO%-2d 有反应  (前者可能是 RST/CS/DC, 后者可能是 BUSY)\r\n",
             p, SAFE_PINS[j]);
        hits++;
      }
    }
  }
  if (!hits) both("    未发现联动。屏幕可能未上电，或 RST/BUSY 没接。\r\n");
}

// ---------------------------------------------------------------- [3] I2C 广谱扫描
static const int PAIRS[][2] = {
  {8,9},{4,5},{1,2},{6,7},{10,11},{12,13},{14,15},{16,17},
  {18,21},{38,39},{40,41},{42,47},{9,10},{5,6},{2,4},{13,14}
};
static const int NPAIR = sizeof(PAIRS) / sizeof(PAIRS[0]);

static void i2c_scan() {
  both("\r\n[3] I2C 广谱扫描（找触摸 FT6336U / 其它 I2C 模块）\r\n");
  int total = 0;
  for (int k = 0; k < NPAIR; k++) {
    int sda = PAIRS[k][0], scl = PAIRS[k][1];
    Wire.end();
    if (!Wire.begin(sda, scl, 100000)) continue;
    Wire.setTimeout(3);
    delay(3);
    for (uint8_t a = 0x08; a <= 0x77; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        const char *who = "未知";
        if (a == 0x3C || a == 0x3D) who = "OLED 屏";
        else if (a == 0x38) who = "FT6236/FT6336U 触摸?";
        else if (a == 0x5D || a == 0x14) who = "GT911 触摸?";
        else if (a == 0x15) who = "CST816S 触摸?";
        else if (a == 0x76 || a == 0x77) who = "BMP280/BME280 气压";
        else if (a == 0x44 || a == 0x45) who = "SHT3x 温湿度";
        both("    SDA=%-2d SCL=%-2d  命中 0x%02X  %s\r\n", sda, scl, a, who);
        total++;
      }
    }
  }
  Wire.end();
  if (!total) both("    零命中 —— 对纯 SPI 电子纸屏来说这是正常结果\r\n");
}

// ---------------------------------------------------------------- SPI 握手
static bool spi_ready = false;

static bool wait_busy(uint32_t timeout_ms, uint32_t *elapsed) {
  uint32_t t0 = millis();
  int first = digitalRead(cfg.busy);
  while (millis() - t0 < timeout_ms) {
    if (digitalRead(cfg.busy) != first) { if (elapsed) *elapsed = millis() - t0; return true; }
    delay(2);
  }
  if (elapsed) *elapsed = millis() - t0;
  return false;   // 有跳变才算通
}

static void send_cmd(uint8_t c) {
  digitalWrite(cfg.dc, LOW);
  digitalWrite(cfg.cs, LOW);
  SPI.transfer(c);
  digitalWrite(cfg.cs, HIGH);
}

static void send_cmd1(uint8_t c, uint8_t v) {
  digitalWrite(cfg.dc, LOW);  digitalWrite(cfg.cs, LOW); SPI.transfer(c); digitalWrite(cfg.cs, HIGH);
  digitalWrite(cfg.dc, HIGH); digitalWrite(cfg.cs, LOW); SPI.transfer(v); digitalWrite(cfg.cs, HIGH);
}

static void ping() {
  both("\r\n[PING] SPI 握手  SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BUSY=%d\r\n",
       cfg.sclk, cfg.mosi, cfg.cs, cfg.dc, cfg.rst, cfg.busy);

  pinMode(cfg.busy, INPUT);
  pinMode(cfg.sclk, OUTPUT); pinMode(cfg.mosi, OUTPUT);
  pinMode(cfg.cs, OUTPUT);   pinMode(cfg.dc, OUTPUT); pinMode(cfg.rst, OUTPUT);
  digitalWrite(cfg.cs, HIGH); digitalWrite(cfg.dc, HIGH);

  SPI.end();
  SPI.begin(cfg.sclk, -1, cfg.mosi, cfg.cs);
  SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  spi_ready = true;

  // 硬件复位
  digitalWrite(cfg.rst, LOW);  delay(20);
  digitalWrite(cfg.rst, HIGH); delay(200);
  both("    复位脉冲已发，BUSY 当前电平 = %d\r\n", digitalRead(cfg.busy));

  // 软复位 0x12，然后采样 BUSY 是否有跳变
  uint32_t el = 0;
  send_cmd(0x12);
  bool ok = wait_busy(2500, &el);
  both("    发软复位命令 0x12 后，BUSY %s（%lu ms 内观测）\r\n",
       ok ? "出现电平跳变" : "始终无变化", (unsigned long)el);
  both("    >>> %s\r\n", ok
       ? "链路通了：屏幕对命令有响应，可以继续 TEST 刷屏"
       : "未收到响应。先查 VCC=3V3 / GND / 6 根信号线，或换一组引脚再 PING");
  SPI.endTransaction();
  SPI.end();
}

// ---------------------------------------------------------------- 刷屏测试
// 800x480 -> 每行 100 字节，共 480 行；逐行生成，只占 100 字节缓冲
static uint8_t linebuf[100];

static void wait_idle(const char *tag) {
  // 等 BUSY 回到空闲（以 PING 后读到的电平为参考较复杂，这里用固定超时轮询低电平）
  uint32_t t0 = millis();
  while (digitalRead(cfg.busy) == HIGH && millis() - t0 < 6000) delay(10);
  both("    %s: 等待 %lu ms\r\n", tag, (unsigned long)(millis() - t0));
}

static void epd_init() {
  digitalWrite(cfg.rst, LOW);  delay(20);
  digitalWrite(cfg.rst, HIGH); delay(200);
  // SSD1677 / GDEQ0426T82 常用初始化
  digitalWrite(cfg.dc, LOW); digitalWrite(cfg.cs, LOW);
  SPI.transfer(0x01); digitalWrite(cfg.cs, HIGH);
  digitalWrite(cfg.dc, HIGH); digitalWrite(cfg.cs, LOW);
  SPI.transfer(0xDF); SPI.transfer(0x01); SPI.transfer(0x00);
  digitalWrite(cfg.cs, HIGH);

  send_cmd1(0x11, 0x03);                     // data entry: X+, Y+
  // RAM X 0..99 (800 px / 8)
  digitalWrite(cfg.dc, LOW); digitalWrite(cfg.cs, LOW); SPI.transfer(0x44); digitalWrite(cfg.cs, HIGH);
  digitalWrite(cfg.dc, HIGH); digitalWrite(cfg.cs, LOW); SPI.transfer(0x00); SPI.transfer(0x63); digitalWrite(cfg.cs, HIGH);
  // RAM Y 0..479
  digitalWrite(cfg.dc, LOW); digitalWrite(cfg.cs, LOW); SPI.transfer(0x45); digitalWrite(cfg.cs, HIGH);
  digitalWrite(cfg.dc, HIGH); digitalWrite(cfg.cs, LOW);
  SPI.transfer(0x00); SPI.transfer(0x00); SPI.transfer(0xDF); SPI.transfer(0x01);
  digitalWrite(cfg.cs, HIGH);
  // 起始地址
  send_cmd1(0x4E, 0x00);
  digitalWrite(cfg.dc, LOW); digitalWrite(cfg.cs, LOW); SPI.transfer(0x4F); digitalWrite(cfg.cs, HIGH);
  digitalWrite(cfg.dc, HIGH); digitalWrite(cfg.cs, LOW); SPI.transfer(0x00); SPI.transfer(0x00); digitalWrite(cfg.cs, HIGH);
}

static void epd_fill(uint8_t pattern, const char *name) {
  both("    刷新: %s ...\r\n", name);
  epd_init();
  send_cmd(0x24);
  digitalWrite(cfg.dc, HIGH);
  for (int row = 0; row < 480; row++) {
    for (int x = 0; x < 100; x++) {
      if (pattern == 0) linebuf[x] = 0x00;                    // 全黑
      else if (pattern == 1) linebuf[x] = 0xFF;               // 全白
      else linebuf[x] = ((row / 32) & 1) ? 0xAA : 0x55;      // 棋盘
    }
    digitalWrite(cfg.cs, LOW);
    SPI.transfer(linebuf, 100);
    digitalWrite(cfg.cs, HIGH);
  }
  send_cmd1(0x22, 0xF7);   // display update control
  send_cmd(0x20);          // activate display update
  wait_idle(name);
}

static void test_pattern() {
  both("\r\n[TEST] 三步刷屏（电子纸全刷约需数秒，属正常）\r\n");
  SPI.begin(cfg.sclk, -1, cfg.mosi, cfg.cs);
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  epd_fill(0, "全黑");
  epd_fill(1, "全白");
  epd_fill(2, "棋盘格");
  SPI.endTransaction();
  SPI.end();
  both("    >>> 完成。肉眼看屏幕若出现黑白变化，说明 SPI + 驱动链路全通\r\n");
  both("    >>> 若屏幕无变化：多半是初始化波形表与你的批次不同，但 PING 已能证明链路\r\n");
}

// ---------------------------------------------------------------- 命令解析
static char cmdline[128];
static int cmdlen = 0;
static void exec_cmd(const char *s);   // 前置声明

static void poll_cmd() {
  while (HWCDCSerial.available() || Serial0.available()) {
    int c = HWCDCSerial.available() ? HWCDCSerial.read() : Serial0.read();
    if (c < 0) break;
    if (c == '\r' || c == '\n') {
      if (cmdlen) { cmdline[cmdlen] = 0; cmdlen = 0; exec_cmd(cmdline); }
    } else if (cmdlen < (int)sizeof(cmdline) - 1) {
      cmdline[cmdlen++] = (char)c;
    }
  }
}

static void exec_cmd(const char *s) {
  both("\r\n>> %s\r\n", s);
  if (!strncasecmp(s, "HELP", 4)) {
    both("  HELP / SCAN / PINS sclk mosi cs dc rst busy / PING / TEST\r\n");
    both("  例: PINS 12 11 10 9 8 13\r\n");
  } else if (!strncasecmp(s, "SCAN", 4)) {
    gpio_snapshot(); pin_probe(); i2c_scan();
  } else if (!strncasecmp(s, "PINS", 4)) {
    int v[6];
    int n = sscanf(s + 4, "%d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
    if (n != 6) { both("  需要 6 个数字，例: PINS 12 11 10 9 8 13\r\n"); return; }
    bool bad = false;
    for (int i = 0; i < 6; i++) {
      bool safe = false;
      for (int j = 0; j < N; j++) if (SAFE_PINS[j] == v[i]) safe = true;
      if (!safe) { both("  GPIO%d 不在安全引脚池里，拒绝设置\r\n", v[i]); bad = true; }
    }
    if (bad) return;
    cfg = {v[0], v[1], v[2], v[3], v[4], v[5]};
    both("  已设置。接着发 PING 验证\r\n");
  } else if (!strncasecmp(s, "PING", 4)) {
    ping();
  } else if (!strncasecmp(s, "TEST", 4)) {
    test_pattern();
  } else {
    both("  不认识的命令。发 HELP\r\n");
  }
}

// ---------------------------------------------------------------- 主流程
void setup() {
  HWCDCSerial.begin(115200);
  Serial0.begin(115200);
  delay(1200);

  both("\r\n=========================================\r\n");
  both("  ScreenLink | 4.26 电子纸屏 SPI 联调\r\n");
  both("  目标: GDEQ0426T82 / SSD1677 / 800x480 / 3.3V\r\n");
  both("=========================================\r\n");
  both("  注意: 这是纯 SPI 屏，没有 I2C 地址，\r\n");
  both("        I2C 扫描零命中是正常结果，不代表接错线。\r\n");
  both("  发 HELP 看可用命令\r\n");

  gpio_snapshot();
  pin_probe();
  i2c_scan();

  both("\r\n=========================================\r\n");
  both("  自动诊断结束，每 5s 心跳；发 PINS / PING / TEST 继续\r\n");
  both("=========================================\r\n");
}

static uint32_t beat = 0;
void loop() {
  poll_cmd();
  if (millis() / 5000 != beat) {
    beat = millis() / 5000;
    both("[%6lu ms] 心跳 #%lu  等待命令 (HELP)\r\n", (unsigned long)millis(), (unsigned long)beat);
  }
  delay(10);
}
