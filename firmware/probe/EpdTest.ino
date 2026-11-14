/*
 * EpdTest - 4.26" 墨水屏 (GDEY0426T82 / SSD1677 / 800x480) ESP32-S3 点屏固件
 *
 * 驱动逻辑逐条移植自官方参赛套件:
 *   MOCE-Contestant-Kit-1.0.0-alpha.1/modules/epaper-42/drivers/common/epd0426.c
 *   (面板型号/分辨率/BUSY 电平/复位电平均取自同目录 module.json)
 *
 * 关键点: 这块屏走 **3 线 SPI + 9 bit 字** 模式(BS1 高、D/C 固定低),
 * 所以 DC 脚不用接。每个 SPI 字 9 bit: 第 1 bit 是 D/C, 后 8 bit 是数据。
 * 官方驱动把 9 bit 字按位打包成 8 bit 字节流再发, 所以任意 SPI 主机都能驱动。
 *
 * 接线(7 根, DC 不接):
 *   屏 U1-1 3V3  -> ESP32 3V3
 *   屏 U1-8 GND  -> ESP32 GND
 *   屏 U1-6 SCK  -> GPIO12
 *   屏 U1-7 MOSI -> GPIO11
 *   屏 U1-5 CS   -> GPIO10
 *   屏 U1-3 RES  -> GPIO9
 *   屏 U1-2 BUSY -> GPIO13
 *
 * 串口命令(115200, COM4):
 *   HELP / PINS sck mosi cs rst busy / DEMO / WHITE / BLACK / CHECK / BARS / FLIP
 *
 * 安全: 只用 3.3V; 绝不接 5V; 上电前确认 3V3 与 GND 不短路。
 */

#include <Arduino.h>
#include <SPI.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ------------------------------------------------------------------ 引脚
static int PIN_SCK = 12;
static int PIN_MOSI = 11;
static int PIN_CS = 10;
static int PIN_RST = 9;
static int PIN_BUSY = 13;

static const uint32_t BUSY_TIMEOUT_MS = 3000;    // 全刷最坏 1.6s, 给足余量
static const uint32_t SPI_HZ = 4000000;

static const int W = 800;
static const int H = 480;
static const int ROW_BYTES = W / 8;               // 100
static const size_t FRAME_BYTES = (size_t)ROW_BYTES * H;   // 48000

static uint8_t frame[FRAME_BYTES];
static bool flipY = true;                          // 原生扫描 Y 递减, 画面可能要上下翻
static bool ready = false;
static uint32_t retryAt = 0;                       // 非 0 = 到点重试初始化
static int retryCount = 0;

// ------------------------------------------------------------------ 输出
static void say(const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
  Serial.flush();
}

// ------------------------------------------------------------------ BUSY
static bool busyAlive = true;   // BUSY 一旦超时, 就认为该线没接好, 后续不再等

static bool waitIdle(const char *tag) {
  if (!busyAlive) return false;
  uint32_t t0 = millis();
  while (digitalRead(PIN_BUSY) == HIGH) {
    if (millis() - t0 > BUSY_TIMEOUT_MS) {
      busyAlive = false;
      say("  [警告] %s: BUSY 一直为高 %lums, 判定 BUSY 线没接好, 后续不再等待\r\n",
          tag, (unsigned long)BUSY_TIMEOUT_MS);
      return false;
    }
    delay(1);
  }
  return true;
}

// ------------------------------------------------------- 9bit 字打包发送
// 官方实现: prefix 个 0x7F(NOP) 把整笔事务对齐到 8 个 9bit 字(72 clk),
// 保证 CS 翻转边界落在整字上; 然后 D/C=0 的命令字, 再 D/C=1 的数据字。
static void sendCmd(uint8_t cmd, const uint8_t *data, size_t len, uint8_t fill) {
  static uint8_t packed[288];          // 256 字 = 288 字节
  size_t prefix = (8u - ((len + 1u) % 8u)) % 8u;
  size_t words = prefix + 1u + len;    // 必为 8 的整数倍

  waitIdle("cmd");
  digitalWrite(PIN_CS, LOW);

  for (size_t offset = 0; offset < words;) {
    size_t count = words - offset;
    if (count > 256u) count = 256u;

    memset(packed, 0, sizeof(packed));
    for (size_t i = 0; i < count; i++) {
      size_t pos = offset + i;
      uint16_t word;
      if (pos < prefix)          word = 0x7Fu;                       // NOP
      else if (pos == prefix)    word = cmd;                         // D/C=0
      else                       word = 0x100u | (data ? data[pos - prefix - 1u] : fill);  // D/C=1
      for (unsigned b = 0; b < 9u; b++) {
        size_t bit = i * 9u + b;
        if (word & (1u << (8u - b))) packed[bit / 8u] |= (uint8_t)(0x80u >> (bit % 8u));
      }
    }
    SPI.writeBytes(packed, (uint8_t)(count * 9u / 8u));
    offset += count;
  }

  digitalWrite(PIN_CS, HIGH);
  delay(1);   // 保证 CS 高电平间隔
}

// ------------------------------------------------------- BUSY 在线判定
// 屏的 BUSY 是推挽输出, 内部上下拉拗不过它; 若上下拉能改变读数,
// 说明这根脚是悬空的 —— 也就是屏根本没接上。返回 -1=悬空, 0/1=真实电平。
static int probeBusy() {
  pinMode(PIN_BUSY, INPUT_PULLUP);   delay(2);
  int a = digitalRead(PIN_BUSY);
  pinMode(PIN_BUSY, INPUT_PULLDOWN); delay(2);
  int b = digitalRead(PIN_BUSY);
  pinMode(PIN_BUSY, INPUT);          delay(2);
  if (a != b) return -1;
  return a;
}

// ------------------------------------------------------------------ 初始化
static void epdReset() {
  pinMode(PIN_CS, OUTPUT);   digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_RST, OUTPUT);  digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BUSY, INPUT);  // 模块输出, 高=忙
  delay(10);
  digitalWrite(PIN_RST, LOW);  delay(10);
  digitalWrite(PIN_RST, HIGH); delay(10);
  waitIdle("reset");
}

static void setWindow() {
  const uint8_t x[] = {0x00, 0x00, 0x1F, 0x03};   // 0..799
  const uint8_t y[] = {0xDF, 0x01, 0x00, 0x00};   // 0..479
  const uint8_t z[] = {0x00, 0x00};
  const uint8_t t[] = {0xDF, 0x01};
  sendCmd(0x44, x, sizeof(x), 0);
  sendCmd(0x45, y, sizeof(y), 0);
  sendCmd(0x4E, z, sizeof(z), 0);
  sendCmd(0x4F, t, sizeof(t), 0);
}

static bool epdInit() {
  busyAlive = true;

  // 先判定 BUSY 这根线到底有没有接到屏
  int link = probeBusy();
  if (link < 0) {
    say("[EPD] 判定: BUSY(GPIO%d) 悬空 -> 屏没有接到 ESP32 上\r\n", PIN_BUSY);
    say("      请把屏模块 U1 的 8 根线接到 ESP32(见接线表), 接好再按 RST\r\n");
    busyAlive = false;          // 悬空就别再等 3 秒超时了
  } else {
    say("[EPD] 判定: BUSY(GPIO%d) 被屏驱动, 电平=%d -> 屏已在线\r\n", PIN_BUSY, link);
  }

  say("[EPD] 复位...\r\n");
  epdReset();

  const uint8_t softstart[] = {0xAE, 0xC7, 0xC3, 0xC0, 0x80};
  const uint8_t gate[] = {0xDF, 0x01, 0x02};      // 480 条栅极
  const uint8_t update[] = {0x40, 0x00};          // 旁路副平面, 单芯片

  sendCmd(0x12, NULL, 0, 0x00);  delay(10);       // 软复位
  waitIdle("swreset");
  sendCmd(0x18, NULL, 1, 0x80);                   // 温度传感器
  sendCmd(0x0C, softstart, sizeof(softstart), 0); // 软启动
  sendCmd(0x01, gate, sizeof(gate), 0);           // 驱动输出控制
  sendCmd(0x11, NULL, 1, 0x01);                   // RAM 入口: X+, Y-
  sendCmd(0x3C, NULL, 1, 0x01);                   // 边框波形
  sendCmd(0x21, update, sizeof(update), 0);       // 显示更新控制
  setWindow();

  // 用 BUSY 是否曾有高电平来判断屏到底在不在线上
  bool busySeen = false;
  for (int i = 0; i < 20; i++) {
    if (digitalRead(PIN_BUSY) == HIGH) { busySeen = true; break; }
    delay(5);
  }
  say("[EPD] 初始化命令已发完, BUSY 当前=%d, 曾见忙=%d\r\n",
      digitalRead(PIN_BUSY), (int)busySeen);
  ready = true;
  return (link >= 0);          // 以推挽判定为准, 不看随机的忙信号
}

// ------------------------------------------------------------------ 刷屏
static void epdShow() {
  if (!ready) return;
  uint32_t t0 = millis();
  setWindow();
  sendCmd(0x24, frame, FRAME_BYTES, 0x00);        // 当前帧
  setWindow();
  sendCmd(0x26, NULL, FRAME_BYTES, 0xFF);         // 上一帧填白, 全刷
  sendCmd(0x22, NULL, 1, 0xF7);                   // 测温+OTP波形+全刷+掉电
  sendCmd(0x20, NULL, 0, 0x00);                   // 启动更新
  delay(1);
  waitIdle("refresh");
  say("[EPD] 刷新完成, 耗时 %lums\r\n", (unsigned long)(millis() - t0));
  Serial.flush();
}

// ------------------------------------------------------------------ 画布
static inline void putPixel(int x, int y, bool white) {
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  int yy = flipY ? (H - 1 - y) : y;
  size_t idx = (size_t)yy * ROW_BYTES + (x >> 3);
  uint8_t m = (uint8_t)(0x80u >> (x & 7));
  if (white) frame[idx] |= m; else frame[idx] &= (uint8_t)~m;
}
static void fillAll(bool white) { memset(frame, white ? 0xFF : 0x00, FRAME_BYTES); }
static void fillRect(int x, int y, int w, int h, bool white) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) putPixel(i, j, white);
}
static void drawRect(int x, int y, int w, int h, int t, bool white) {
  fillRect(x, y, w, t, white);
  fillRect(x, y + h - t, w, t, white);
  fillRect(x, y, t, h, white);
  fillRect(x + w - t, y, t, h, white);
}

// ------------------------------------------------------------------ 画面
static void pageWhite() { fillAll(true);  epdShow(); }
static void pageBlack() { fillAll(false); epdShow(); }

static void pageCheck() {
  fillAll(true);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      if (((x >> 4) + (y >> 4)) & 1) putPixel(x, y, false);
  epdShow();
}

static void pageBars() {
  fillAll(true);
  for (int b = 0; b < 8; b++)                 // 8 级灰阶条(用抖动近似)
    for (int y = b * 60; y < (b + 1) * 60; y++)
      for (int x = 40; x < 760; x++) {
        int level = 8 - b;                    // 1..8
        bool white = ((x + y) % 8) >= level ? true : false;
        putPixel(x, y, white);
      }
  drawRect(20, 20, W - 40, H - 40, 4, false);
  // 四角标记: 验证方向(X 增/Y 减)和地址是否对得上
  fillRect(20, 20, 60, 20, false);
  fillRect(W - 80, 20, 60, 20, false);
  fillRect(20, H - 40, 60, 20, false);
  fillRect(W - 80, H - 40, 60, 20, false);
  epdShow();
}

static void demo() {
  say("\r\n==== 开始点屏: 白 -> 黑 -> 棋盘 -> 灰阶 ====\r\n");
  say("每张约 2 秒, 全部走完停在灰阶图\r\n");
  pageWhite();
  pageBlack();
  pageCheck();
  pageBars();
  say("==== 结束。屏幕应对应最后一张: 外框+四角块+灰阶条 ====\r\n");
}

// ------------------------------------------------------------------ 命令
static void help() {
  say("\r\n命令: HELP | PINS sck mosi cs rst busy | DEMO | WHITE | BLACK | CHECK | BARS | FLIP\r\n");
  say("当前引脚: SCK=%d MOSI=%d CS=%d RST=%d BUSY=%d  flipY=%d\r\n",
      PIN_SCK, PIN_MOSI, PIN_CS, PIN_RST, PIN_BUSY, (int)flipY);
}

static void applyPins() {
  SPI.end();
  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  ready = false;
  epdInit();
}

static void handle(char *line) {
  while (*line == ' ') line++;
  char *arg = strchr(line, ' ');
  if (arg) *arg++ = 0;

  if (!strcasecmp(line, "HELP"))     { help(); return; }
  if (!strcasecmp(line, "DEMO"))     { demo(); return; }
  if (!strcasecmp(line, "WHITE"))    { pageWhite(); return; }
  if (!strcasecmp(line, "BLACK"))    { pageBlack(); return; }
  if (!strcasecmp(line, "CHECK"))    { pageCheck(); return; }
  if (!strcasecmp(line, "BARS"))     { pageBars(); return; }
  if (!strcasecmp(line, "FLIP"))     { flipY = !flipY; say("flipY=%d, 重画\r\n", (int)flipY); pageBars(); return; }
  if (!strcasecmp(line, "PINS")) {
    int a, b, c, d, e;
    if (arg && sscanf(arg, "%d %d %d %d %d", &a, &b, &c, &d, &e) == 5) {
      PIN_SCK = a; PIN_MOSI = b; PIN_CS = c; PIN_RST = d; PIN_BUSY = e;
      say("引脚已改: SCK=%d MOSI=%d CS=%d RST=%d BUSY=%d\r\n", a, b, c, d, e);
      say("重新初始化屏幕...\r\n");
      applyPins();
      pageBars();
      return;
    }
    say("用法: PINS 12 11 10 9 13\r\n");
    return;
  }
  say("不认识: %s  (发 HELP)\r\n", line);
}

// ------------------------------------------------------------------ main
void setup() {
  Serial.begin(115200);
  delay(300);
  say("\r\n\r\n=== EpdTest / GDEY0426T82 800x480 / SSD1677 ===\r\n");
  say("接线: 3V3-GND必接, SCK=%d MOSI=%d CS=%d RST=%d BUSY=%d, DC 不接\r\n",
      PIN_SCK, PIN_MOSI, PIN_CS, PIN_RST, PIN_BUSY);

  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));

  // 允许上电后再接线: 没探到屏就每 5 秒重试一次初始化。
  // 连续 3 次探不到就不再依赖 BUSY, 强行画一次(防止 BUSY 线本身有问题时永远不画)。
  if (epdInit()) {
    demo();
  } else {
    say("没探到屏。请检查 3V3/GND/SCK/MOSI/CS/RES/BUSY 七根线。\r\n");
    say("接好后不用按复位, 每 5 秒自动重试一次。\r\n");
    retryAt = millis() + 5000;
  }
  help();
}

void loop() {
  static char line[64];
  static size_t n = 0;
  static uint32_t beatAt = 0;
  static uint32_t beat = 0;

  // ---- 心跳: 证明固件活着, 并实时报告屏是否接上 ----
  if (millis() >= beatAt) {
    beatAt = millis() + 5000;
    int lk = probeBusy();
    say("[%6lums] 心跳 #%lu  ready=%d BUSY=%s\r\n",
        (unsigned long)millis(), (unsigned long)++beat, (int)ready,
        lk < 0 ? "悬空(屏未接)" : (lk ? "高(忙)" : "低(空闲)"));
  }

  // ---- 自动重试: 上电后再接线也能自动点亮 ----
  if (retryAt && millis() >= retryAt) {
    retryCount++;
    say("[EPD] 第 %d 次重试初始化...\r\n", retryCount);
    bool seen = epdInit();
    if (seen || retryCount >= 3) {
      if (!seen) say("[EPD] BUSY 始终没动, 仍强行画一次, 若屏正常即可显示\r\n");
      retryAt = 0;
      demo();
    } else {
      retryAt = millis() + 5000;
    }
  }

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (n) { line[n] = 0; handle(line); n = 0; }
    } else if (n < sizeof(line) - 1) {
      line[n++] = c;
    }
  }
}
