/*
 * EcwHud - EcwBridge + SSD1306 状态屏 (二合一)
 *
 * 保留 EcwBridge 的全部调试命令, 同时在 0.96 寸 OLED 上实时显示系统状态,
 * 这样不用开电脑也能看到桥通没通、总线上一共挂了几个器件。
 *
 * 屏幕接线(实测确认, 见 SSD1306_128x64_OLED_文档.md):
 *   GND -> GND     VCC -> 3V3 (只能 3.3V)
 *   SCL -> GPIO42  SDA -> GPIO41
 *
 * ECW 接线(不变):
 *   ESP32 GPIO17 -> ECW RX     ESP32 GPIO18 -> ECW TX     GND 共地
 *
 * 关于 41/42 与 SCAN/PROBE 的冲突:
 *   这两个脚同时是 OLED 的 I2C 线。SCAN/HUNT/PROBE 会把它们切成 UART/GPIO 用,
 *   跑完必须重新初始化 OLED, 否则屏停在最后一帧不再更新。代码里统一走 oledReinit()。
 *
 * 串口命令 (115200, COM4):
 *   原 EcwBridge: PROBE / SCAN / HUNT / LISTEN / SYNC / BRIDGE / SEND
 *   新增: HUD ON|OFF|TEXT <str> / BUSC / OLEDRST
 */

#include <Arduino.h>
#include "oled_ssd1306.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ------------------------------------------------------------------ 屏参数
static const int OLED_SDA = 41;
static const int OLED_SCL = 42;
static Oled oled;
static bool hudOn = true;

static char hudMode[12]  = "READY";   // 当前动作
static char hudLink[22]  = "LINK 17-18";
static char hudProb[22]  = "PROBE -";
static char hudBus[22]   = "BUS -";
static char hudEvt[22]   = "BOOT";
static bool hudDirty = true;

// ------------------------------------------------------------------ 输出
static void say(const char *fmt, ...) {
  char b[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  Serial.print(b);
}

// ------------------------------------------------------------------ 屏刷新
static bool hudRender() {
  if (!oled.online || !hudOn) return false;

  oled.clear();
  oled.text(0, 0, "ECW BRIDGE", 1);

  unsigned long s = millis() / 1000;
  char up[12];
  snprintf(up, sizeof(up), "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  oled.text(OLED_W - (int)strlen(up) * 6, 0, up, 1);

  oled.hline(0, 9, OLED_W);

  char line[24];
  snprintf(line, sizeof(line), "MODE %s", hudMode);
  oled.text(0, 13, line, 1);

  oled.text(0, 23, hudLink, 1);
  oled.text(0, 33, hudProb, 1);
  oled.text(0, 43, hudBus, 1);

  oled.hline(0, 52, OLED_W);
  oled.text(0, 55, hudEvt, 1);

  oled.online = oled.show();
  hudDirty = !oled.online;
  return oled.online;
}

static void setEvt(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(hudEvt, sizeof(hudEvt), fmt, ap);
  va_end(ap);
  hudDirty = true;
}

// SCAN/PROBE/BRIDGE 结束后调用: 重新把 I2C 拿回来交给屏
static void oledReinit() {
  oled.online = false;
  if (oled.begin((uint8_t)OLED_SDA, (uint8_t)OLED_SCL)) {
    hudDirty = true;
    hudRender();
  }
}

// ------------------------------------------------------------------ 候选引脚
// 排除: 0=BOOT, 19/20=USB D-/D+, 26~37=Flash/八线PSRAM, 43/44=UART0 TX/RX
static const int CAND[] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
  15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47, 48
};
static const int CAND_N = (int)(sizeof(CAND) / sizeof(CAND[0]));

static const uint32_t BAUDS[] = {
  115200, 921600, 1500000, 2000000, 1000000, 460800, 230400, 9600
};
static const int BAUD_N = (int)(sizeof(BAUDS) / sizeof(BAUDS[0]));

static const uint32_t CAP_MAX = 256;
static uint8_t capBuf[CAP_MAX];
static uint32_t capLen = 0;

// ------------------------------------------------------------------ 抓字节
static uint32_t capture(int pin, uint32_t baud, uint32_t ms) {
  capLen = 0;
  Serial1.end();
  delay(5);
  Serial1.begin(baud, SERIAL_8N1, pin, -1);
  Serial1.setTimeout(0);
  delay(15);

  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    while (Serial1.available()) {
      int c = Serial1.read();
      if (c < 0) break;
      if (capLen < CAP_MAX) capBuf[capLen] = (uint8_t)c;
      capLen++;
    }
    delay(1);
  }
  Serial1.end();
  delay(2);
  return capLen;
}

static void dumpCapture(uint32_t total) {
  uint32_t show = total < 32 ? total : 32;
  say("       HEX:");
  for (uint32_t i = 0; i < show; i++) {
    if (i && (i % 16) == 0) say("\r\n            ");
    say(" %02X", capBuf[i]);
  }
  say("\r\n       TXT:");
  for (uint32_t i = 0; i < show; i++) {
    char c = (char)capBuf[i];
    say("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
  }
  if (total > show) say(" ... 共 %lu 字节", (unsigned long)total);
  say("\r\n");
}

// ------------------------------------------------------------------ 引脚探测
static void probePins() {
  say("\r\n[PROBE] 引脚电平探测\r\n");
  say("  内部下拉还读 HIGH  -> 有外部驱动/上拉 -> 线接上了(且对方有电)\r\n");
  say("  内部上拉还读 LOW   -> 被外部拉到地     -> 可能接到了 GND\r\n");

  int highs[CAND_N], lows[CAND_N];
  int nh = 0, nl = 0;

  for (int i = 0; i < CAND_N; i++) {
    int p = CAND[i];
    pinMode(p, INPUT_PULLDOWN);
    delay(3);
    bool h = (digitalRead(p) == HIGH);
    pinMode(p, INPUT_PULLUP);
    delay(3);
    bool l = (digitalRead(p) == LOW);
    pinMode(p, INPUT);
    if (h) highs[nh++] = p;
    if (l) lows[nl++] = p;
  }

  if (nh == 0) {
    say("  没有任何脚被外部驱动 -> 线没接上, 或 ECW 没上电\r\n");
  } else {
    say("  被拉高的脚 (%d):", nh);
    for (int i = 0; i < nh; i++) say(" GPIO%d", highs[i]);
    say("\r\n");
  }

  if (nl == 0) {
    say("  没有脚被拉到地 -> GND 那根线可能没接\r\n");
  } else {
    say("  被拉到地的脚 (%d):", nl);
    for (int i = 0; i < nl; i++) say(" GPIO%d", lows[i]);
    say("\r\n");
  }

  pinMode(0, INPUT_PULLDOWN);
  delay(4);
  int ctl = digitalRead(0);
  pinMode(0, INPUT);
  say("  [对照] GPIO0(板载上拉) 读=%d -> %s\r\n", ctl,
      ctl == HIGH ? "方法有效" : "方法失效, 结论不可信");

  // 屏上只放最有用的两个信息: 高电平脚(可能有信号) 和 数量
  if (nh == 0) {
    snprintf(hudProb, sizeof(hudProb), "PROBE NONE");
  } else if (nh <= 2) {
    snprintf(hudProb, sizeof(hudProb), "PROBE G%d G%d", highs[0], nh > 1 ? highs[1] : 0);
  } else {
    snprintf(hudProb, sizeof(hudProb), "PROBE %d HIGH", nh);
  }
  hudDirty = true;
}

// ------------------------------------------------------------------ I2C 总线体检
// 把总线上所有应答的地址列出来。温湿度 GXHT30C 在 0x44, 屏在 0x3C。
static void busCheck() {
  say("\r\n[BUSC] 扫描 I2C 总线 (SDA=%d SCL=%d)...\r\n", OLED_SDA, OLED_SCL);

  // PROBE/SCAN 会把 41/42 切回普通 GPIO, Wire 当初做的引脚复用已经失效。
  // 这里必须重新把 I2C 外设挂上去, 否则扫描全零命中(实测踩过)。
  Wire.end();
  delay(2);
  Wire.begin((uint8_t)OLED_SDA, (uint8_t)OLED_SCL, 400000);
  Wire.setClock(400000);
  delay(2);

  int hits = 0;
  char tmp[22];
  tmp[0] = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      const char *who = "";
      if (a == 0x3C || a == 0x3D) who = " SSD1306 屏";
      else if (a == 0x44)         who = " GXHT30C 温湿度";
      say("  0x%02X%s\r\n", a, who);
      hits++;
      if (strlen(tmp) < 18) {
        char one[8];
        snprintf(one, sizeof(one), "%s%02X", tmp[0] ? " " : "", a);
        strncat(tmp, one, sizeof(tmp) - strlen(tmp) - 1);
      }
    }
  }
  if (hits == 0) {
    say("  零命中: 总线被拉死 / 模块没供电 / SDA-SCL 接反\r\n");
    snprintf(hudBus, sizeof(hudBus), "BUS NONE");
  } else {
    say("  共 %d 个器件\r\n", hits);
    snprintf(hudBus, sizeof(hudBus), "BUS %s", tmp);
  }
  hudDirty = true;
  setEvt("BUSC %d DEV", hits);
}

// ------------------------------------------------------------------ 全量扫描
static void scanAll(uint32_t dwellMs) {
  say("\r\n[SCAN] 全引脚 x 全波特率监听, 每格 %lums\r\n", (unsigned long)dwellMs);
  say("  预计耗时 %lu 秒\r\n",
      (unsigned long)((uint32_t)CAND_N * BAUD_N * (dwellMs + 25) / 1000));

  snprintf(hudMode, sizeof(hudMode), "SCAN");
  hudDirty = true;
  hudRender();

  int hits = 0;
  for (int bi = 0; bi < BAUD_N; bi++) {
    uint32_t baud = BAUDS[bi];
    for (int pi = 0; pi < CAND_N; pi++) {
      int pin = CAND[pi];
      uint32_t n = capture(pin, baud, dwellMs);
      if (n > 0) {
        hits++;
        say("  [命中] GPIO%d @ %lu bps, %lu 字节\r\n",
            pin, (unsigned long)baud, (unsigned long)n);
        dumpCapture(n);
        snprintf(hudEvt, sizeof(hudEvt), "HIT G%d %luB", pin, (unsigned long)n);
        hudDirty = true;
      }
    }
    say("  ... %lu bps 扫完\r\n", (unsigned long)baud);
  }

  say(hits ? "  扫描结束, 有数据\r\n" : "  扫描结束, 全静默\r\n");
  snprintf(hudMode, sizeof(hudMode), "READY");
  hudDirty = true;
  oledReinit();          // SCAN 动过 41/42, 必须把 I2C 抢回来
}

// ------------------------------------------------------------------ 边听边复位
static void hunt(uint32_t baud, uint32_t sec) {
  say("\r\n[HUNT] %lu bps, 轮流监听 %d 个脚, 持续 %lu 秒\r\n",
      (unsigned long)baud, CAND_N, (unsigned long)sec);
  say("  >>> 现在给 ECW 断电再上电, 或按 ECW 的 RST <<<\r\n");

  snprintf(hudMode, sizeof(hudMode), "HUNT");
  hudDirty = true;
  hudRender();

  uint32_t t0 = millis();
  int round = 0;
  while ((millis() - t0) < sec * 1000UL) {
    for (int i = 0; i < CAND_N && (millis() - t0) < sec * 1000UL; i++) {
      int pin = CAND[i];
      uint32_t n = capture(pin, baud, 130);
      if (n > 0) {
        say("  [命中] GPIO%d @ %lu bps, %lu 字节\r\n",
            pin, (unsigned long)baud, (unsigned long)n);
        dumpCapture(n);
        snprintf(hudEvt, sizeof(hudEvt), "HIT G%d %luB", pin, (unsigned long)n);
        hudDirty = true;
      }
    }
    round++;
    if ((round % 3) == 0) {
      say("  ... 第 %d 轮, 剩余 %lu 秒\r\n", round,
          (unsigned long)(sec - (millis() - t0) / 1000));
    }
  }
  say("  HUNT 结束, 共 %d 轮\r\n", round);
  snprintf(hudMode, sizeof(hudMode), "READY");
  hudDirty = true;
  oledReinit();
}

// ------------------------------------------------------------------ 单脚监听
static void listenPin(int pin, uint32_t baud, uint32_t sec) {
  say("\r\n[LISTEN] GPIO%d @ %lu bps, %lu 秒\r\n", pin, (unsigned long)baud, (unsigned long)sec);

  snprintf(hudMode, sizeof(hudMode), "LISTEN G%d", pin);
  hudDirty = true;
  hudRender();

  capLen = 0;
  Serial1.end();
  delay(5);
  Serial1.begin(baud, SERIAL_8N1, pin, -1);
  Serial1.setTimeout(0);

  uint32_t t0 = millis();
  bool streaming = false;
  while ((millis() - t0) < sec * 1000UL) {
    while (Serial1.available()) {
      int c = Serial1.read();
      if (c < 0) break;
      char ch = (char)c;
      if (ch == '\n' || ch == '\r' || (ch >= 0x20 && ch < 0x7F)) {
        Serial.write(ch);
      } else {
        char hx[8];
        snprintf(hx, sizeof(hx), "<%02X>", (uint8_t)c);
        Serial.print(hx);
      }
      streaming = true;
    }
    delay(1);
  }
  Serial1.end();
  say("\r\n  监听结束%s\r\n", streaming ? "" : " (一个字节都没收到)");
  snprintf(hudMode, sizeof(hudMode), "READY");
  hudDirty = true;
  oledReinit();
}

// ------------------------------------------------------------------ 发原始字节
static bool sendRaw(int txPin, int rxPin, uint32_t baud,
                    const uint8_t *data, size_t len) {
  Serial1.end();
  delay(5);
  Serial1.begin(baud, SERIAL_8N1, rxPin, txPin);
  Serial1.write(data, len);
  Serial1.flush();
  delay(20);
  return true;
}

// ------------------------------------------------------------------ 官方同步
static void syncTry(int txPin, int rxPin, uint32_t sec) {
  const uint32_t baud = 2000000;
  say("\r\n[SYNC] TX=GPIO%d RX=GPIO%d @ %lu bps, 连续发 0xA5, %lu 秒\r\n",
      txPin, rxPin, (unsigned long)baud, (unsigned long)sec);
  say("  >>> 同时按 ECW 的 RST, 或断电重上 <<<\r\n");

  snprintf(hudMode, sizeof(hudMode), "SYNC");
  hudDirty = true;
  hudRender();

  Serial1.end();
  delay(5);
  Serial1.begin(baud, SERIAL_8N1, rxPin, txPin);
  Serial1.setTimeout(0);

  uint8_t burst[64];
  memset(burst, 0xA5, sizeof(burst));

  uint32_t t0 = millis();
  uint32_t sent = 0;
  capLen = 0;
  uint32_t rxTotal = 0;

  while ((millis() - t0) < sec * 1000UL) {
    Serial1.write(burst, sizeof(burst));
    sent += sizeof(burst);
    while (Serial1.available()) {
      int c = Serial1.read();
      if (c < 0) break;
      if (capLen < CAP_MAX) capBuf[capLen] = (uint8_t)c;
      capLen++;
      rxTotal++;
    }
    delay(2);
  }
  Serial1.end();

  say("  已发 %lu 字节, 收到 %lu 字节\r\n",
      (unsigned long)sent, (unsigned long)rxTotal);
  if (rxTotal) {
    dumpCapture(rxTotal);
  } else {
    say("  零回包 -> 芯片没进 bootrom, 或 TX/RX 接反, 或没共地\r\n");
    say("  下一步: 把 TX/RX 对调再试一次 (SYNC %d %d %lu)\r\n",
        rxPin, txPin, (unsigned long)sec);
  }
  snprintf(hudMode, sizeof(hudMode), "READY");
  hudDirty = true;
  oledReinit();
}

// ------------------------------------------------------------------ 透传桥
static void bridgeMode(int txPin, int rxPin, uint32_t baud) {
  say("\r\n[BRIDGE] TX=GPIO%d RX=GPIO%d @ %lu bps\r\n",
      txPin, rxPin, (unsigned long)baud);
  say("  现在 COM4 直连 ECW, 你打的每个字都会发过去。按 ESC (0x1B) 退出。\r\n");
  say("  ------------------------------------------------\r\n");

  snprintf(hudMode, sizeof(hudMode), "BRIDGE");
  snprintf(hudLink, sizeof(hudLink), "LINK %d-%d", txPin, rxPin);
  hudDirty = true;
  hudRender();

  Serial1.end();
  delay(5);
  Serial1.begin(baud, SERIAL_8N1, rxPin, txPin);
  Serial1.setTimeout(0);

  uint32_t toEcw = 0, toPc = 0;
  bool quit = false;
  uint32_t lastHud = millis();
  while (!quit) {
    while (Serial.available()) {
      int c = Serial.read();
      if (c < 0) break;
      if (c == 0x1B) { quit = true; break; }
      Serial1.write((uint8_t)c);
      toEcw++;
    }
    while (Serial1.available()) {
      int c = Serial1.read();
      if (c < 0) break;
      Serial.write((uint8_t)c);
      toPc++;
    }
    if (millis() - lastHud > 1000) {          // 桥模式里也保持屏刷新
      lastHud = millis();
      char e[22];
      snprintf(e, sizeof(e), "TX%lu RX%lu", (unsigned long)toEcw, (unsigned long)toPc);
      strncpy(hudEvt, e, sizeof(hudEvt) - 1);
      hudDirty = true;
      hudRender();
    }
    delay(1);
  }

  Serial1.end();
  say("\r\n  ------------------------------------------------\r\n");
  say("[BRIDGE] 结束: PC->ECW %lu 字节, ECW->PC %lu 字节\r\n",
      (unsigned long)toEcw, (unsigned long)toPc);
  snprintf(hudMode, sizeof(hudMode), "READY");
  setEvt("BRIDGE TX%lu RX%lu", (unsigned long)toEcw, (unsigned long)toPc);
  oledReinit();
}

// ------------------------------------------------------------------ 命令
static void help() {
  say("\r\n==== EcwHud / 桥 + 状态屏 ====\r\n");
  say("  PROBE                        判断信号线有没有真的接到 ECW\r\n");
  say("  SCAN [毫秒]                  全引脚 x 全波特率监听 (默认 200)\r\n");
  say("  HUNT <baud> [秒]             单波特率轮询所有脚, 期间请复位 ECW\r\n");
  say("  LISTEN <pin> <baud> [秒]     盯一个脚看原始字节\r\n");
  say("  SYNC <tx> <rx> [秒]          发 0xA5 试下载模式 (2Mbps)\r\n");
  say("  BRIDGE <tx> <rx> <baud>      透传 COM4 <-> ECW, ESC 退出\r\n");
  say("  SEND <tx> <rx> <baud> <hex>  发字节再听 1 秒\r\n");
  say("  BUSC                         扫 I2C 总线, 结果同时上屏\r\n");
  say("  HUD ON | OFF | TEXT <str>    屏开关 / 全屏大字\r\n");
  say("  OLEDRST                      重新初始化屏\r\n");
  say("\r\n");
}

static size_t parseHex(const char *s, uint8_t *out, size_t maxOut) {
  size_t n = 0;
  int hi = -1;
  for (const char *p = s; *p; p++) {
    int v;
    char c = *p;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else if (c == ' ' || c == ',') continue;
    else return (size_t)-1;
    if (hi < 0) {
      hi = v;
    } else {
      if (n < maxOut) out[n] = (uint8_t)((hi << 4) | v);
      n++;
      hi = -1;
    }
  }
  return (hi < 0) ? n : (size_t)-1;
}

static bool hudBigText(const char *s) {
  if (!oled.online) return false;
  oled.clear();
  int n = (int)strlen(s);
  uint8_t scale = (n <= 10) ? 2 : 1;
  int w = n * 6 * scale;
  oled.text((OLED_W - w) / 2 < 0 ? 2 : (OLED_W - w) / 2, 20, s, scale);
  oled.online = oled.show();
  if (oled.online) hudOn = false;
  return oled.online;
}

static void handle(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;

  if (!strcasecmp(line, "HELP") || !strcasecmp(line, "?")) { help(); return; }
  if (!strcasecmp(line, "PROBE")) { probePins(); setEvt("PROBE DONE"); oledReinit(); return; }
  if (!strcasecmp(line, "BUSC"))  { busCheck(); return; }
  if (!strcasecmp(line, "OLEDRST")) { oledReinit(); say(oled.online ? "屏已恢复\r\n" : "屏仍未找到\r\n"); return; }

  if (!strncasecmp(line, "HUD", 3)) {
    const char *a = strchr(line, ' ');
    while (a && *a == ' ') a++;
    if (!a) { say("用法: HUD ON|OFF|TEXT <str>\r\n"); return; }
    if (!strcasecmp(a, "ON")) {
      hudOn = true;
      hudDirty = true;
      say(hudRender() ? "屏已开\r\n" : "屏不在线, 先 OLEDRST\r\n");
      return;
    }
    if (!strcasecmp(a, "OFF")) {
      hudOn = false;
      oled.clear();
      oled.online = oled.online && oled.show();
      say(oled.online ? "屏已关\r\n" : "屏不在线, 先 OLEDRST\r\n");
      return;
    }
    if (!strncasecmp(a, "TEXT", 4)) {
      const char *t = a + 4;
      while (*t == ' ') t++;
      if (!*t) { say("用法: HUD TEXT HELLO\r\n"); return; }
      if (hudBigText(t)) say("已显示: %s (发 HUD ON 恢复状态页)\r\n", t);
      else say("屏不在线, 先 OLEDRST\r\n");
      return;
    }
    say("不认识: %s\r\n", a);
    return;
  }

  if (!strncasecmp(line, "SCAN", 4)) {
    uint32_t ms = 200;
    const char *a = strchr(line, ' ');
    if (a) {
      int v = atoi(a + 1);
      if (v >= 50 && v <= 3000) ms = (uint32_t)v;
    }
    scanAll(ms);
    return;
  }

  if (!strncasecmp(line, "HUNT", 4)) {
    int baud = 0, sec = 20;
    const char *a = strchr(line, ' ');
    if (!a || sscanf(a + 1, "%d %d", &baud, &sec) < 1 || baud < 300) {
      say("用法: HUNT 115200 20\r\n");
      return;
    }
    if (sec < 3) sec = 3;
    if (sec > 120) sec = 120;
    hunt((uint32_t)baud, (uint32_t)sec);
    return;
  }

  if (!strncasecmp(line, "LISTEN", 6)) {
    int pin = -1, baud = 115200, sec = 5;
    const char *a = strchr(line, ' ');
    if (!a || sscanf(a + 1, "%d %d %d", &pin, &baud, &sec) < 2) {
      say("用法: LISTEN 18 115200 5\r\n");
      return;
    }
    if (sec < 1) sec = 1;
    if (sec > 60) sec = 60;
    listenPin(pin, (uint32_t)baud, (uint32_t)sec);
    return;
  }

  if (!strncasecmp(line, "SYNC", 4)) {
    int tx = 17, rx = 18, sec = 15;
    const char *a = strchr(line, ' ');
    if (a) {
      int got = sscanf(a + 1, "%d %d %d", &tx, &rx, &sec);
      if (got < 2) { say("用法: SYNC 17 18 15\r\n"); return; }
    }
    if (sec < 3) sec = 3;
    if (sec > 60) sec = 60;
    syncTry(tx, rx, (uint32_t)sec);
    return;
  }

  if (!strncasecmp(line, "BRIDGE", 6)) {
    int tx = 17, rx = 18, baud = 115200;
    const char *a = strchr(line, ' ');
    if (!a || sscanf(a + 1, "%d %d %d", &tx, &rx, &baud) < 3) {
      say("用法: BRIDGE 17 18 115200\r\n");
      return;
    }
    bridgeMode(tx, rx, (uint32_t)baud);
    return;
  }

  if (!strncasecmp(line, "SEND", 4)) {
    int tx = 17, rx = 18, baud = 115200;
    char hex[160] = {0};
    const char *a = strchr(line, ' ');
    if (!a || sscanf(a + 1, "%d %d %d %159s", &tx, &rx, &baud, hex) < 4) {
      say("用法: SEND 17 18 115200 55AA\r\n");
      return;
    }
    uint8_t buf[64];
    size_t n = parseHex(hex, buf, sizeof(buf));
    if (n == (size_t)-1 || n == 0) { say("十六进制格式不对, 例: 55AA0D0A\r\n"); return; }
    sendRaw(tx, rx, (uint32_t)baud, buf, n);
    say("已发 %u 字节, 监听 1 秒...\r\n", (unsigned)n);
    uint32_t got = capture(rx, (uint32_t)baud, 1000);
    if (got) { say("  收到 %lu 字节\r\n", (unsigned long)got); dumpCapture(got); }
    else     { say("  无回包\r\n"); }
    oledReinit();
    return;
  }

  say("不认识: %s  (发 HELP)\r\n", line);
}

// ------------------------------------------------------------------ 主流程
void setup() {
  Serial.begin(115200);
  delay(300);

  say("\r\n");
  say("==== EcwHud / 桥 + SSD1306 状态屏 ====\r\n");
  say("屏: SDA=GPIO%d SCL=GPIO%d (只能 3.3V)\r\n", OLED_SDA, OLED_SCL);
  say("桥: TX=GPIO17 RX=GPIO18\r\n");

  if (oled.begin((uint8_t)OLED_SDA, (uint8_t)OLED_SCL)) {
    say("屏已初始化, 地址 0x%02X\r\n", oled.addr);
    hudDirty = true;
    hudRender();
  } else {
    say("屏没找到 -> 检查 3.3V / GND / SCL->GPIO42 / SDA->GPIO41\r\n");
  }

  probePins();
  busCheck();          // 顺便看看 0x44 温湿度在不在
  setEvt("BOOT OK");
  hudRender();
  help();
}

void loop() {
  static char line[128];
  static size_t n = 0;
  static uint32_t lastHud = 0;
  static uint32_t lastRetry = 0;

  // 屏掉线了就每 3 秒重试一次(支持开机后再接线)
  if (!oled.online && millis() - lastRetry > 3000) {
    lastRetry = millis();
    if (oled.begin((uint8_t)OLED_SDA, (uint8_t)OLED_SCL)) {
      say("[屏] 重新连上 0x%02X\r\n", oled.addr);
      hudDirty = true;
      hudRender();
    }
  }

  // 每秒刷一次(时间在走, 必须刷)
  if (oled.online && hudOn && millis() - lastHud > 1000) {
    lastHud = millis();
    hudRender();
  }

  while (Serial.available()) {
    int ci = Serial.read();
    if (ci < 0) break;
    char c = (char)ci;
    if (c == '\r' || c == '\n') {
      if (n) { line[n] = 0; n = 0; handle(line); }
    } else if (n < sizeof(line) - 1) {
      line[n++] = c;
    }
  }
  delay(5);
}
