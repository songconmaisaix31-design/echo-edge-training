/*
 * EcwBridge - ESP32-S3 <-> ECW6700 桥连 / 联调固件
 *
 * 目标: ESP32-S3 通过 UART 与 ECW6700 通信(或把 ECW 的 console 透传到电脑),
 *       并且在不清楚"接在哪个脚、跑什么波特率"时能自己找出来。
 *
 * 设计要点:
 *   - 无动态分配, 所有缓冲静态; UART 用 Serial1 (不占用 Serial/UART0 日志口)
 *   - 所有串口操作带超时, 不会阻塞死等
 *   - 监听阶段可随时按 Ctrl-C? 不支持, 但每个阶段都有明确秒数上限
 *
 * 串口命令 (115200, COM4/CH343):
 *   HELP                         命令表
 *   PROBE                        引脚电平探测: 判断线有没有真的接到 ECW
 *   SCAN [每格毫秒]               全引脚 x 全波特率 监听(适合有周期输出的设备)
 *   HUNT <baud> [秒]             单波特率下轮询所有 RX 脚(边听边给 ECW 复位最好用)
 *   LISTEN <pin> <baud> [秒]     盯住一个脚看原始字节(ASCII + HEX)
 *   SYNC <tx> <rx> [秒]          按官方时序连续发 0xA5, 尝试让 ECW 进下载模式
 *   BRIDGE <tx> <rx> <baud>      透传: COM4 <-> ECW 串口 (按 ESC 退出)
 *   SEND <tx> <rx> <baud> <hex>  发一串十六进制字节, 然后听 1 秒
 *
 * 接线(默认示例, 按实际改命令参数):
 *   ESP32 GPIO17 -------- ECW 的 RX
 *   ESP32 GPIO18 -------- ECW 的 TX
 *   ESP32 GND    -------- ECW 的 GND      (必须共地, 否则一定收不到)
 *
 * 注意: 不要用 GPIO19/20(USB)、GPIO26~37(Flash/PSRAM)、GPIO43/44(UART0 日志口)。
 */

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ------------------------------------------------------------------ 输出
static void say(const char *fmt, ...) {
  char b[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  Serial.print(b);
}

// ------------------------------------------------------------------ 候选引脚
// 排除: 0=BOOT, 19/20=USB D-/D+, 26~37=Flash/八线PSRAM, 43/44=UART0 TX/RX
static const int CAND[] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
  15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47, 48
};
static const int CAND_N = (int)(sizeof(CAND) / sizeof(CAND[0]));

// 官方工具默认 2Mbps, 其余是常见 console 波特率
static const uint32_t BAUDS[] = {
  115200, 921600, 1500000, 2000000, 1000000, 460800, 230400, 9600
};
static const int BAUD_N = (int)(sizeof(BAUDS) / sizeof(BAUDS[0]));

static const uint32_t CAP_MAX = 256;
static uint8_t capBuf[CAP_MAX];
static uint32_t capLen = 0;

// ------------------------------------------------------------------ 抓字节
// 在 pin 上以 baud 监听 ms 毫秒, 收到的字节进 capBuf。返回收到的总字节数。
// tx 传 -1 表示只收不发。
static uint32_t capture(int pin, uint32_t baud, uint32_t ms) {
  capLen = 0;

  Serial1.end();
  delay(5);
  // 注意: 本版 Arduino-ESP32 的 begin() 返回 void, 不能判返回值
  Serial1.begin(baud, SERIAL_8N1, pin, -1);
  Serial1.setTimeout(0);
  delay(15);                       // 让 UART 稳定

  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    while (Serial1.available()) {
      int c = Serial1.read();
      if (c < 0) break;
      if (capLen < CAP_MAX) {
        capBuf[capLen] = (uint8_t)c;
      }
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
// ECW 的 TX 是推挽输出, 空闲时被它驱动到 3.3V。把 ESP32 的脚设成"输入+内部下拉",
// 如果这个脚真的连到 ECW 的 TX, 外部驱动会压过内部下拉, 读成 HIGH。
// 这是判断"线到底接没接上"最直接的办法, 不需要任何协议握手。
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

  // 对照点: 开发板 BOOT 脚 GPIO0 有 10k 板载上拉, 必须读 HIGH
  pinMode(0, INPUT_PULLDOWN);
  delay(4);
  int ctl = digitalRead(0);
  pinMode(0, INPUT);
  say("  [对照] GPIO0(板载上拉) 读=%d -> %s\r\n", ctl,
      ctl == HIGH ? "方法有效" : "方法失效, 结论不可信");
}

// ------------------------------------------------------------------ 全量扫描
static void scanAll(uint32_t dwellMs) {
  say("\r\n[SCAN] 全引脚 x 全波特率监听, 每格 %lums\r\n", (unsigned long)dwellMs);
  say("  预计耗时 %lu 秒\r\n",
      (unsigned long)((uint32_t)CAND_N * BAUD_N * (dwellMs + 25) / 1000));

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
      }
    }
    say("  ... %lu bps 扫完\r\n", (unsigned long)baud);
  }

  say(hits ? "  扫描结束, 有数据\r\n" : "  扫描结束, 全静默\r\n");
}

// ------------------------------------------------------------------ 边听边复位
static void hunt(uint32_t baud, uint32_t sec) {
  say("\r\n[HUNT] %lu bps, 轮流监听 %d 个脚, 持续 %lu 秒\r\n",
      (unsigned long)baud, CAND_N, (unsigned long)sec);
  say("  >>> 现在给 ECW 断电再上电, 或按 ECW 的 RST <<<\r\n");

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
      }
    }
    round++;
    if ((round % 3) == 0) {
      say("  ... 第 %d 轮, 剩余 %lu 秒 (继续复位 ECW)\r\n", round,
          (unsigned long)(sec - (millis() - t0) / 1000));
    }
  }
  say("  HUNT 结束, 共 %d 轮\r\n", round);
}

// ------------------------------------------------------------------ 单脚监听
static void listenPin(int pin, uint32_t baud, uint32_t sec) {
  say("\r\n[LISTEN] GPIO%d @ %lu bps, %lu 秒\r\n",
      pin, (unsigned long)baud, (unsigned long)sec);

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
      // 实时回显, 方便看持续输出
      char ch = (char)c;
      if (ch == '\n' || ch == '\r' || (ch >= 0x20 && ch < 0x7F)) {
        Serial.write(ch);
        streaming = true;
      } else {
        char hx[8];
        snprintf(hx, sizeof(hx), "<%02X>", (uint8_t)c);
        Serial.print(hx);
        streaming = true;
      }
    }
    delay(1);
  }
  Serial1.end();
  say("\r\n  监听结束%s\r\n", streaming ? "" : " (一个字节都没收到)");
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
// eswin_tool 的做法: 复位瞬间以 2Mbps 连续发 0xA5, 芯片在 bootrom 里认这个字节。
static void syncTry(int txPin, int rxPin, uint32_t sec) {
  const uint32_t baud = 2000000;
  say("\r\n[SYNC] TX=GPIO%d RX=GPIO%d @ %lu bps, 连续发 0xA5, %lu 秒\r\n",
      txPin, rxPin, (unsigned long)baud, (unsigned long)sec);
  say("  >>> 同时按 ECW 的 RST, 或断电重上 <<<\r\n");

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
}

// ------------------------------------------------------------------ 透传桥
static void bridgeMode(int txPin, int rxPin, uint32_t baud) {
  say("\r\n[BRIDGE] TX=GPIO%d RX=GPIO%d @ %lu bps\r\n",
      txPin, rxPin, (unsigned long)baud);
  say("  现在 COM4 直连 ECW, 你打的每个字都会发过去。按 ESC (0x1B) 退出。\r\n");
  say("  ------------------------------------------------\r\n");

  Serial1.end();
  delay(5);
  Serial1.begin(baud, SERIAL_8N1, rxPin, txPin);
  Serial1.setTimeout(0);

  uint32_t toEcw = 0, toPc = 0;
  bool quit = false;
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
    delay(1);
  }

  Serial1.end();
  say("\r\n  ------------------------------------------------\r\n");
  say("[BRIDGE] 结束: PC->ECW %lu 字节, ECW->PC %lu 字节\r\n",
      (unsigned long)toEcw, (unsigned long)toPc);
}

// ------------------------------------------------------------------ 命令
static void help() {
  say("\r\n==== EcwBridge / ESP32-S3 <-> ECW6700 ====\r\n");
  say("  PROBE                        判断信号线有没有真的接到 ECW\r\n");
  say("  SCAN [毫秒]                  全引脚 x 全波特率监听 (默认 200)\r\n");
  say("  HUNT <baud> [秒]             单波特率轮询所有脚, 期间请复位 ECW\r\n");
  say("  LISTEN <pin> <baud> [秒]     盯一个脚看原始字节\r\n");
  say("  SYNC <tx> <rx> [秒]          发 0xA5 试下载模式 (2Mbps)\r\n");
  say("  BRIDGE <tx> <rx> <baud>      透传 COM4 <-> ECW, ESC 退出\r\n");
  say("  SEND <tx> <rx> <baud> <hex>  发字节再听 1 秒, 例: SEND 17 18 115200 55AA\r\n");
  say("\r\n");
}

// SEND 的 hex 解析: "55AA0D0A" -> bytes
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

static void handle(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;

  if (!strcasecmp(line, "HELP") || !strcasecmp(line, "?")) { help(); return; }
  if (!strcasecmp(line, "PROBE")) { probePins(); return; }

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
      if (got < 2) {
        say("用法: SYNC 17 18 15\r\n");
        return;
      }
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
    if (n == (size_t)-1 || n == 0) {
      say("十六进制格式不对, 例: 55AA0D0A\r\n");
      return;
    }
    if (!sendRaw(tx, rx, (uint32_t)baud, buf, n)) {
      say("UART 打不开\r\n");
      return;
    }
    say("已发 %u 字节, 监听 1 秒...\r\n", (unsigned)n);
    uint32_t got = capture(rx, (uint32_t)baud, 1000);
    if (got) { say("  收到 %lu 字节\r\n", (unsigned long)got); dumpCapture(got); }
    else     { say("  无回包\r\n"); }
    return;
  }

  say("不认识: %s  (发 HELP)\r\n", line);
}

// ------------------------------------------------------------------ 主流程
void setup() {
  Serial.begin(115200);
  delay(200);

  say("\r\n");
  say("==== EcwBridge / ESP32-S3 <-> ECW6700 ====\r\n");
  say("候选脚 %d 个, 波特率 %d 档\r\n", CAND_N, BAUD_N);
  say("发 HELP 看命令, 发 PROBE 先确认线接上了没\r\n");

  // 上电先做一次探测, 省得每次手动发
  probePins();
  help();
}

void loop() {
  static char line[128];
  static size_t n = 0;

  while (Serial.available()) {
    int ci = Serial.read();
    if (ci < 0) break;
    char c = (char)ci;
    if (c == '\r' || c == '\n') {
      if (n) {
        line[n] = 0;
        n = 0;
        handle(line);
      }
    } else if (n < sizeof(line) - 1) {
      line[n++] = c;
    }
  }
  delay(5);
}
