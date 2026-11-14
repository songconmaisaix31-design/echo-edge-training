/* ------------------------------------------------------------------
 * SensorHub —— ESP32-S3 三合一传感器联调固件
 *
 * 目标：绕开 ECW6700，用 ESP32 直接驱动三个模块，一次把三路测清楚。
 *
 *   1) 温湿度  GXHT30C      I2C  0x44   自动尝试 SDA/SCL 两种组合
 *   2) 红外    ITR9909+LM393 GPIO 数字DO 需 1~5cm 近距离反射
 *   3) TTS     SYN6288E     UART 9600  0xFD 帧 + XOR
 *
 * 为什么在这里做：ECW 的 CLI 发不出任意字节，SYN6288 测不了；
 * 而 SCL/SDA 线序官方未冻结，用固件自动遍历两种组合最省事。
 *
 * 串口命令（115200）：
 *   HELP            命令表
 *   SCAN            重新扫描 I2C（两种线序都试）
 *   READ            读一次温湿度
 *   AUTO            开/关 每 3 秒自动读温湿度
 *   IR              读红外电平（连读 8 次看抖动）
 *   IRWATCH 20      监听红外 20 秒，手靠近看是否翻转
 *   SPEAK           让 TTS 说一句（SYN6288 9600）
 *   SPEAKQ          发查询状态帧，看是否回 0x4F
 *   PINS sda scl    改 I2C 引脚
 *   TPINS tx rx     改 TTS 引脚
 *   IRPIN n         改红外引脚
 * ------------------------------------------------------------------ */
#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include <stdlib.h>

// ---------------- 默认引脚（避开 17/18=ECW桥, 19/20=USB, 43/44=调试口） ----
static int PIN_SDA = 8;
static int PIN_SCL = 9;
static int PIN_IR  = 10;
static int PIN_TTX = 4;          // ESP32 TX -> TTS 模块 RX(U1-2)
static int PIN_TRX = 5;          // ESP32 RX <- TTS 模块 TX(U1-3)

static const uint8_t GXHT30_ADDR = 0x44;
static const uint32_t TTS_BAUD  = 9600;

static bool i2cUp = false;
static bool autoRead = true;
static uint32_t nextAuto = 0;
static bool irLast = false;
static int  irFound = -1;        // 哪个脚上看到过翻转

// 必须放在最前面: Arduino 会把函数原型插到文件开头
struct Th { float t, rh; bool ok; uint8_t err; };

// ---------------- 串口输出 ----------------
static void say(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

// ---------------- CRC-8 (GXHT30C: poly 0x31, init 0xFF) ----------------
static uint8_t crc8(const uint8_t *data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// ---------------- I2C 扫描 ----------------
static int scanBus(int sda, int scl, uint8_t *hits, int maxHits) {
  Wire.end();
  delay(10);
  Wire.begin(sda, scl, 100000);
  delay(20);
  int n = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      if (n < maxHits) hits[n] = a;
      n++;
    }
  }
  return n;
}

static bool tryStartI2C() {
  uint8_t hits[16];
  say("[I2C] 组合 A: SDA=IO%d SCL=IO%d ... ", PIN_SDA, PIN_SCL);
  int n = scanBus(PIN_SDA, PIN_SCL, hits, 16);
  say("命中 %d\n", n);
  if (n > 0) {
    for (int i = 0; i < n && i < 16; i++) say("       0x%02X\n", hits[i]);
    i2cUp = true;
    return true;
  }

  say("[I2C] 组合 B: SDA=IO%d SCL=IO%d (对调) ... ", PIN_SCL, PIN_SDA);
  n = scanBus(PIN_SCL, PIN_SDA, hits, 16);
  say("命中 %d\n", n);
  if (n > 0) {
    for (int i = 0; i < n && i < 16; i++) say("       0x%02X\n", hits[i]);
    int t = PIN_SDA; PIN_SDA = PIN_SCL; PIN_SCL = t;
    say("       >>> SDA/SCL 是反的, 已自动对调\n");
    i2cUp = true;
    return true;
  }

  Wire.end();
  i2cUp = false;
  return false;
}

// ---------------- GXHT30C 读温湿度 ----------------
static Th readTh() {
  Th r{0, 0, false, 0};
  if (!i2cUp) {
    if (!tryStartI2C()) { r.err = 1; return r; }
  }

  Wire.beginTransmission(GXHT30_ADDR);
  Wire.write(0x24);            // 单次测量, 高重复性, 不拉伸时钟
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) { r.err = 2; return r; }

  delay(20);

  int got = Wire.requestFrom((int)GXHT30_ADDR, 6);
  if (got != 6) { r.err = 3; return r; }

  uint8_t d[6];
  for (int i = 0; i < 6; i++) d[i] = Wire.read();

  if (crc8(d, 2) != d[2] || crc8(d + 3, 2) != d[5]) { r.err = 4; return r; }

  uint16_t rawT = ((uint16_t)d[0] << 8) | d[1];
  uint16_t rawH = ((uint16_t)d[3] << 8) | d[4];
  r.t  = -45.0f + 175.0f * (float)rawT / 65535.0f;
  r.rh = 100.0f * (float)rawH / 65535.0f;
  r.ok = true;
  return r;
}

static void printTh() {
  Th v = readTh();
  if (v.ok) {
    say("[TH ] 温度 %.2f C   湿度 %.2f %%RH   (CRC 通过)\n", v.t, v.rh);
  } else {
    const char *why = "未知";
    switch (v.err) {
      case 1: why = "I2C 一个器件都没扫到 -> 模块没接/没电/SDA与SCL都错"; break;
      case 2: why = "地址 0x44 无应答 -> 模块不在线, 或 ADDR 不是 0x44"; break;
      case 3: why = "读回字节数不对 -> 时序/上拉有问题"; break;
      case 4: why = "CRC 校验失败 -> 通信被干扰, 或 SDA/SCL 接反"; break;
    }
    say("[TH ] 失败: %s (code=%d)\n", why, v.err);
  }
}

// ---------------- 红外 ----------------
static void readIr() {
  int last = -1;
  say("[IR ] 读 GPIO%d 连续 8 次: ", PIN_IR);
  for (int i = 0; i < 8; i++) {
    int v = digitalRead(PIN_IR);
    say("%d", v);
    last = v;
    delay(40);
  }
  say("\n");
  say("[IR ] 当前=%s  (ITR9909 反射式: 手掌/白纸贴到 1~5cm 才会翻转)\n",
      last ? "HIGH" : "LOW");
}

static void watchIr(int seconds) {
  say("[IR ] 监听 %d 秒, 请把手指/白纸靠近探头 1~5cm 再拿开...\n", seconds);
  uint32_t t0 = millis();
  int base = digitalRead(PIN_IR);
  int cnt = 0;
  while (millis() - t0 < (uint32_t)seconds * 1000UL) {
    int v = digitalRead(PIN_IR);
    if (v != base) {
      say("     [变化] %d -> %d  (t=%lus)\n", base, v, (unsigned long)((millis() - t0) / 1000));
      base = v;
      cnt++;
      irFound = PIN_IR;
    }
    delay(8);
  }
  if (cnt == 0) {
    say("[IR ] %d 秒内没有电平变化\n", seconds);
    say("      可能原因: 1)DO 没接到 GPIO%d  2)模块没供电  3)电位器调到一端了\n", PIN_IR);
  } else {
    say("[IR ] 共 %d 次翻转 -> 红外模块在线且工作正常\n", cnt);
  }
}

// ---------------- TTS SYN6288 ----------------
static void ttsBegin() {
  Serial1.end();
  delay(10);
  Serial1.begin(TTS_BAUD, SERIAL_8N1, PIN_TRX, PIN_TTX);
  delay(30);
}

static void ttsSendFrame(const uint8_t *payload, uint16_t len) {
  ttsBegin();
  uint8_t frame[220];
  frame[0] = 0xFD;
  frame[1] = (uint8_t)(len >> 8);
  frame[2] = (uint8_t)(len & 0xFF);
  memcpy(frame + 3, payload, len);
  uint8_t x = 0;
  for (uint16_t i = 0; i < 3 + len; i++) x ^= frame[i];
  frame[3 + len] = x;
  Serial1.write(frame, 4 + len);
  Serial1.flush();
  say("[TTS] 已发送 %d 字节: ", 4 + len);
  for (uint16_t i = 0; i < 4 + len; i++) say("%02X ", frame[i]);
  say("\n");
}

static void ttsSpeak() {
  // 合成文本 "你好": 命令 0x01, 编码 0x00(GB2312), 文本 C4E3 BAC3
  uint8_t p[] = {0x01, 0x00, 0xC4, 0xE3, 0xBA, 0xC3};
  ttsSendFrame(p, sizeof(p));

  Serial1.flush();
  delay(50);
  int n = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 1200) {
    while (Serial1.available()) {
      uint8_t b = Serial1.read();
      say("[TTS] <- 0x%02X", b);
      if (b == 0x41) say("  (命令接收成功)");
      if (b == 0x4A) say("  (初始化成功)");
      if (b == 0x45) say("  (接收失败)");
      if (b == 0x4E) say("  (正在播报)");
      if (b == 0x4F) say("  (空闲)");
      say("\n");
      n++;
    }
    delay(5);
  }
  if (n == 0) {
    say("[TTS] 没收到任何状态回传\n");
    say("      可能原因: 1)模块 TX(U1-3) 没接到 GPIO%d  2)模块没供电  3)TX/RX 接反\n", PIN_TRX);
  }
}

static void ttsQuery() {
  uint8_t p[] = {0x21};          // 查询状态
  ttsSendFrame(p, sizeof(p));
  Serial1.flush();
  delay(50);
  int n = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 1000) {
    while (Serial1.available()) {
      uint8_t b = Serial1.read();
      say("[TTS] <- 0x%02X\n", b);
      n++;
    }
    delay(5);
  }
  if (n == 0) say("[TTS] 查询也无回传 -> 这条 UART 没通\n");
}

// ---------------- 命令 ----------------
static void help() {
  say("\n=== SensorHub 命令 ===\n");
  say("  SCAN           重扫 I2C(自动试 SDA/SCL 两种线序)\n");
  say("  READ           读一次温湿度\n");
  say("  AUTO           开/关自动读(每 3 秒)\n");
  say("  IR             读红外电平\n");
  say("  IRWATCH n      监听红外 n 秒\n");
  say("  SPEAK          让 TTS 说\"你好\"\n");
  say("  SPEAKQ         查询 TTS 状态\n");
  say("  PINS sda scl   改 I2C 引脚\n");
  say("  TPINS tx rx    改 TTS 引脚\n");
  say("  IRPIN n        改红外引脚\n");
  say("  STATUS         打印当前接线配置\n");
  say("  HELP           本表\n");
}

static void status() {
  say("\n--- 当前配置 ---\n");
  say("  I2C : SDA=IO%d  SCL=IO%d  %s\n", PIN_SDA, PIN_SCL, i2cUp ? "(已启动)" : "(未启动)");
  say("  IR  : DO=IO%d\n", PIN_IR);
  say("  TTS : TX=IO%d  RX=IO%d  %lu bps\n", PIN_TTX, PIN_TRX, (unsigned long)TTS_BAUD);
  say("  IR 是否见过翻转: %s\n", irFound >= 0 ? "是" : "还没测到");
}

static void handle(char *line) {
  // 去首尾空格
  while (*line == ' ') line++;
  char *e = line + strlen(line);
  while (e > line && (e[-1] == ' ' || e[-1] == '\r')) *--e = 0;

  if (!strcasecmp(line, "HELP") || !strcasecmp(line, "?")) { help(); return; }
  if (!strcasecmp(line, "STATUS")) { status(); return; }
  if (!strcasecmp(line, "SCAN")) {
    say("\n[I2C] 扫描开始\n");
    if (!tryStartI2C()) {
      say("[I2C] 两种线序都没扫到任何器件\n");
      say("      检查: 3V3 是否接到 3.3V / GND 是否共地 / 4 根线是否插到底\n");
    } else {
      say("[I2C] 成功, 可以用 READ 读温湿度了\n");
    }
    return;
  }
  if (!strcasecmp(line, "READ")) { printTh(); return; }
  if (!strcasecmp(line, "AUTO")) {
    autoRead = !autoRead;
    say("[TH ] 自动读取 %s\n", autoRead ? "开" : "关");
    return;
  }
  if (!strcasecmp(line, "IR")) { readIr(); return; }
  if (!strcasecmp(line, "SPEAK")) { ttsSpeak(); return; }
  if (!strcasecmp(line, "SPEAKQ")) { ttsQuery(); return; }
  if (!strncasecmp(line, "IRWATCH", 7)) {
    int s = atoi(line + 7);
    watchIr(s > 0 ? s : 15);
    return;
  }
  if (!strncasecmp(line, "PINS", 4)) {
    int a = 0, b = 0;
    if (sscanf(line + 4, "%d %d", &a, &b) == 2) {
      PIN_SDA = a; PIN_SCL = b; i2cUp = false;
      say("[I2C] 改为 SDA=IO%d SCL=IO%d, 重新扫描...\n", a, b);
      tryStartI2C();
    } else {
      say("用法: PINS 8 9\n");
    }
    return;
  }
  if (!strncasecmp(line, "TPINS", 5)) {
    int a = 0, b = 0;
    if (sscanf(line + 5, "%d %d", &a, &b) == 2) {
      PIN_TTX = a; PIN_TRX = b;
      say("[TTS] 改为 TX=IO%d RX=IO%d\n", a, b);
      ttsBegin();
    } else {
      say("用法: TPINS 4 5\n");
    }
    return;
  }
  if (!strncasecmp(line, "IRPIN", 5)) {
    int a = atoi(line + 5);
    if (a > 0) {
      PIN_IR = a;
      pinMode(PIN_IR, INPUT_PULLUP);
      say("[IR ] 改为 GPIO%d\n", a);
    }
    return;
  }
  if (!*line) return;
  say("不认识: %s  (发 HELP)\n", line);
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(300);

  say("\n\n=== SensorHub / ESP32-S3 三合一传感器联调 ===\n");
  say("温湿度 GXHT30C(I2C 0x44) | 红外 ITR9909(GPIO) | TTS SYN6288E(UART 9600)\n\n");

  pinMode(PIN_IR, INPUT_PULLUP);
  status();

  say("\n");
  if (tryStartI2C()) {
    say("[I2C] 通了, 直接读一次温湿度\n");
    printTh();
  } else {
    say("[I2C] 没扫到器件, 先按接线表检查, 接好发 SCAN\n");
  }

  readIr();
  help();
  nextAuto = millis() + 3000;
}

void loop() {
  static char line[96];
  static size_t n = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (n) { line[n] = 0; handle(line); n = 0; }
    } else if (n < sizeof(line) - 1) {
      line[n++] = c;
    }
  }

  if (autoRead && i2cUp && millis() >= nextAuto) {
    nextAuto = millis() + 3000;
    Th v = readTh();
    if (v.ok) say("[TH ] %.2f C  %.2f %%RH\n", v.t, v.rh);
  }
}
