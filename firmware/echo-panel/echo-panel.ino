/*
 * EchoPanel - Echo Edge 床旁终端面板
 *
 * 职责单一: 把后端最近一次切面推理的结果同步到 128x64 OLED 上。
 * 不做推理、不做采集、不碰 ECW 桥 —— 这台设备就是一个显示器。
 *
 * 三个设计取舍, 写在这里免得以后有人"优化"掉:
 *
 * 1) 轮询而不是长连接。模型一次推理是秒级事件, 屏刷新也是秒级, WebSocket
 *    带来的实时性收益在这个时间尺度上等于零, 却要多维护一套断线重连与
 *    ping 保活。1 秒一次 GET 对 FastAPI 的压力可以忽略。
 *
 * 2) WiFi 凭据进 NVS, 不写进源码。串口配一次, 之后掉电重连都自己来。
 *    源码里留空串, 仓库就不会带出热点密码。
 *
 * 3) 全屏重绘而不是局部刷新。SSD1306 全帧 25ms, 1 秒一次占比 2.5%,
 *    但换来的是渲染代码没有"残影"类 bug。局部刷新省下的这点时间不值得。
 */
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <string.h>

#include "oled_ssd1306.h"
#include "panel_assets.h"

// ------------------------------------------------------------------ 硬件
static const int OLED_SDA = 41;
static const int OLED_SCL = 42;

// 走 I2C 时屏幕刷新约 25ms, 轮询周期留出足够余量
static const uint32_t POLL_OK_MS = 1000;
static const uint32_t POLL_FAIL_MS = 5000;

static Oled oled;
static bool oledOk = false;

// ------------------------------------------------------------------ 数据
// 类型定义必须排在所有函数之前: Arduino 会把函数原型插到第一个函数定义
// 上方, Snapshot 若定义在后面, 原型里的 Snapshot* 就成了未知类型。
enum PanelState { ST_EMPTY, ST_READY, ST_STALE, ST_RUNNING, ST_FAILED };

struct Snapshot {
  PanelState state = ST_EMPTY;
  long seq = 0;
  long ageS = -1;
  long caseId = -1;
  char view[8] = "";
  char label[24] = "";
  char precision[8] = "";
  int frames = 0;
  float conf = 0;
  float a4c = 0;
  float psax = 0;
};

// ------------------------------------------------------------------ 配置
struct Config {
  char ssid[33];
  char pass[65];
  char url[96];
};

static Config cfg;
static Preferences prefs;

static void loadConfig() {
  memset(&cfg, 0, sizeof(cfg));
  prefs.begin("panel", true);
  prefs.getString("ssid", cfg.ssid, sizeof(cfg.ssid));
  prefs.getString("pass", cfg.pass, sizeof(cfg.pass));
  prefs.getString("url", cfg.url, sizeof(cfg.url));
  prefs.end();
  if (!cfg.url[0]) {
    // 默认指向本机后端; 端口与 Start-EchoEdge 的 ApiPort 一致
    strncpy(cfg.url, "http://172.20.10.2:8036/api/v1/panel/state", sizeof(cfg.url) - 1);
  }
}

static void saveConfig() {
  prefs.begin("panel", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("url", cfg.url);
  prefs.end();
}

// ------------------------------------------------------------------ 数据
static Snapshot snap;
static Snapshot shown;              // 上次真正画到屏上的内容
static bool dirty = true;

static uint32_t lastPoll = 0;
static uint32_t pollInterval = POLL_OK_MS;
static uint32_t failStreak = 0;
static uint32_t okCount = 0;
static uint32_t failCount = 0;
static int lastCode = 0;
static uint32_t lastOkMs = 0;

// ------------------------------------------------------------------ 输出
static void say(const char *fmt, ...) {
  char line[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  Serial.print(line);
}

// ------------------------------------------------------------------ JSON
// 面向本接口的固定字段名做定向扫描。不引入 ArduinoJson: 响应体是我们自己
// 定的, 字段名固定, 一个 strstr 就够; 引库会多 20KB flash 和一次堆分配。

static const char *findKey(const char *json, const char *key) {
  char pat[28];
  snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char *p = strstr(json, pat);
  if (!p) return nullptr;
  p += strlen(pat);
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

static bool jsonString(const char *json, const char *key, char *out, size_t cap) {
  const char *p = findKey(json, key);
  if (!p || *p != '"') return false;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
  out[i] = 0;
  return true;
}

static bool jsonNumber(const char *json, const char *key, double *out) {
  const char *p = findKey(json, key);
  if (!p) return false;
  if (*p == 'n') return false;                 // null
  char *end = nullptr;
  double v = strtod(p, &end);
  if (end == p) return false;
  *out = v;
  return true;
}

static bool jsonIsNull(const char *json, const char *key) {
  const char *p = findKey(json, key);
  return p && *p == 'n';
}

static void parseSnapshot(const char *json, Snapshot *s) {
  *s = Snapshot();

  char state[16] = "";
  jsonString(json, "state", state, sizeof(state));
  if (!strcmp(state, "ready")) s->state = ST_READY;
  else if (!strcmp(state, "stale")) s->state = ST_STALE;
  else if (!strcmp(state, "running")) s->state = ST_RUNNING;
  else if (!strcmp(state, "failed")) s->state = ST_FAILED;
  else s->state = ST_EMPTY;

  double v;
  if (jsonNumber(json, "seq", &v)) s->seq = (long)v;
  if (jsonNumber(json, "ageS", &v)) s->ageS = (long)v;
  else if (jsonIsNull(json, "ageS")) s->ageS = -1;
  if (jsonNumber(json, "case", &v)) s->caseId = (long)v;
  if (jsonNumber(json, "confidence", &v)) s->conf = (float)v;
  if (jsonNumber(json, "a4c", &v)) s->a4c = (float)v;
  if (jsonNumber(json, "psax", &v)) s->psax = (float)v;
  if (jsonNumber(json, "frames", &v)) s->frames = (int)v;

  jsonString(json, "view", s->view, sizeof(s->view));
  jsonString(json, "label", s->label, sizeof(s->label));
  jsonString(json, "precision", s->precision, sizeof(s->precision));
}

// ------------------------------------------------------------------ HTTP
static bool fetchState(char *buf, size_t cap, int *codeOut) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!cfg.url[0]) return false;

  HTTPClient http;
  // 首次连接要等 DHCP 后的 ARP 落地, 实测会摸到 600ms 附近; 给到 1000ms,
  // 但也不能再大 —— loop 是同步的, 对端挂掉时这段就是屏幕的卡顿时间。
  http.setConnectTimeout(1000);
  http.setTimeout(1200);
  if (!http.begin(cfg.url)) return false;
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  *codeOut = code;
  if (code != 200) { http.end(); return false; }

  WiFiClient *stream = http.getStreamPtr();
  // 按 Content-Length 读满就收工。
  // 曾经的写法是"读到连接关闭或 1500ms 兜底", 但服务端默认 keep-alive,
  // body 收完之后 available() 一直是 0 而 connected() 仍为真, 于是每轮
  // 请求都白白空转 1.5 秒 —— 实测单次耗时 1.7s, 其中 1.5s 是这段等待。
  int len = http.getSize();
  size_t n = 0;
  uint32_t t0 = millis();
  uint32_t lastData = t0;
  while (n < cap - 1 && millis() - t0 < 2000) {
    if (len > 0 && n >= (size_t)len) break;
    int avail = stream->available();
    if (avail <= 0) {
      if (!http.connected()) break;
      // 连接还开着却已静默 300ms: 要么是分块传输的间隙, 要么就是没有
      // Content-Length。两种都不值得再等, 已经拿到的字节先交出去。
      if (millis() - lastData > 300) break;
      delay(2);
      continue;
    }
    size_t want = (size_t)avail;
    if (want > cap - 1 - n) want = cap - 1 - n;
    if (len > 0 && want > (size_t)len - n) want = (size_t)len - n;
    n += stream->readBytes(buf + n, want);
    lastData = millis();
  }
  buf[n] = 0;
  http.end();
  return n > 0;
}

// ------------------------------------------------------------------ 绘制
static void rectBox(int x, int y, int w, int h, bool fill) {
  if (fill) {
    for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++) oled.px(x + i, y + j);
  } else {
    for (int i = 0; i < w; i++) { oled.px(x + i, y); oled.px(x + i, y + h - 1); }
    for (int j = 0; j < h; j++) { oled.px(x, y + j); oled.px(x + w - 1, y + j); }
  }
}

static void hline(int x0, int y, int x1) {
  for (int x = x0; x <= x1; x++) oled.px(x, y);
}

// 11x11 扇形标记, 位图来自 panel_assets.h
static void drawLogo(int x, int y) {
  for (int row = 0; row < 11; row++) {
    uint8_t lo = LOGO_BITS[row * 2];
    uint8_t hi = LOGO_BITS[row * 2 + 1];
    for (int c = 0; c < 8; c++)
      if (lo & (1 << c)) oled.px(x + c, y + row);
    for (int c = 0; c < 3; c++)
      if (hi & (1 << c)) oled.px(x + 8 + c, y + row);
  }
}

static int microIndex(char ch) {
  if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
  const char *p = strchr(MICRO_CHARSET, ch);
  return p ? (int)(p - MICRO_CHARSET) : 0;
}

static int microWidth(const char *s) {
  int n = (int)strlen(s);
  return n > 0 ? n * 4 - 1 : 0;
}

static void microText(int x, int y, const char *s) {
  int cx = x;
  for (; *s; s++) {
    uint16_t bits = MICRO_BITS[microIndex(*s)];
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 3; col++)
        if (bits & (1u << (row * 3 + col))) oled.px(cx + col, y + row);
    cx += 4;
  }
}

static int cjkIndex(const char *p) {
  for (int i = 0; i < CJK_GLYPHS; i++) {
    const char *g = CJK_CHARSET + i * 3;
    if ((unsigned char)g[0] == (unsigned char)p[0] &&
        (unsigned char)g[1] == (unsigned char)p[1] &&
        (unsigned char)g[2] == (unsigned char)p[2]) return i;
  }
  return -1;
}

// 返回渲染宽度, 便于调用方做居中
static int cjkText(int x, int y, const char *utf8) {
  int cx = x;
  const unsigned char *p = (const unsigned char *)utf8;
  while (*p) {
    if (*p < 0x80) { p++; continue; }          // 表里只有汉字, 跳过 ASCII
    if (!p[1] || !p[2]) break;
    int idx = cjkIndex((const char *)p);
    if (idx >= 0) {
      const uint8_t *g = CJK_BITS + idx * 32;
      for (int row = 0; row < 16; row++) {
        uint16_t bits = ((uint16_t)g[row * 2] << 8) | g[row * 2 + 1];
        for (int c = 0; c < 16; c++)
          if (bits & (1u << (15 - c))) oled.px(cx + c, y + row);
      }
    }
    cx += 16;
    p += 3;
  }
  return cx - x;
}

static int cjkWidth(const char *utf8) {
  int n = 0;
  const unsigned char *p = (const unsigned char *)utf8;
  while (*p) {
    if (*p >= 0x80) { n++; p += 3; } else p++;
  }
  return n * 16;
}

static void drawWifi(int x, int y, int level) {
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2;
    int bx = x + i * 4;
    int by = y + 8 - h;
    if (i < level) rectBox(bx, by, 2, h, true);
    else rectBox(bx, by, 2, h, false);
  }
}

static int wifiLevel() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  long rssi = WiFi.RSSI();
  if (rssi > -55) return 4;
  if (rssi > -65) return 3;
  if (rssi > -75) return 2;
  if (rssi > -85) return 1;
  return 1;
}

static const char *statusWord() {
  switch (snap.state) {
    case ST_READY: return "LIVE";
    case ST_STALE: return "STALE";
    case ST_RUNNING: return "BUSY";
    case ST_FAILED: return "ERR";
    default: return WiFi.status() == WL_CONNECTED ? "IDLE" : "NO NET";
  }
}

static const char *statusHint() {
  switch (snap.state) {
    case ST_RUNNING: return "ANALYZING";
    case ST_FAILED: return "INFERENCE FAILED";
    default:
      return WiFi.status() == WL_CONNECTED ? "NO RESULT YET" : "CHECK NETWORK";
  }
}

static const char *stateLabel() {
  if (snap.label[0]) return snap.label;
  switch (snap.state) {
    case ST_RUNNING: return "分析中";
    case ST_FAILED: return "待判定";
    default:
      return WiFi.status() == WL_CONNECTED ? "待分析" : "无连线";
  }
}

// 顶部状态行: 品牌 + 小字 + 链格
static void drawChrome() {
  drawLogo(3, 1);
  oled.text(19, 3, "ECHO EDGE", 1);
  drawWifi(110, 2, wifiLevel());

  microText(19, 12, "PEDIATRIC ECHO");

  char right[24];
  if (snap.ageS >= 0 && (snap.state == ST_READY || snap.state == ST_STALE)) {
    // 超过一分钟就换成分钟, 不然 "3600S" 会挤出屏幕
    if (snap.ageS >= 60) snprintf(right, sizeof(right), "%s %ldM", statusWord(), snap.ageS / 60);
    else snprintf(right, sizeof(right), "%s %ldS", statusWord(), snap.ageS);
  } else {
    snprintf(right, sizeof(right), "%s", statusWord());
  }
  microText(OLED_W - 4 - microWidth(right), 12, right);

  hline(3, 18, 125);
}

static void drawData() {
  bool valid = (snap.state == ST_READY || snap.state == ST_STALE) && snap.view[0];

  if (!valid) {
    // 没有结论时不摆代号、不画空进度条 —— 空条看着像故障
    const char *lab = stateLabel();
    cjkText((OLED_W - cjkWidth(lab)) / 2, 26, lab);
    const char *hint = statusHint();
    microText((OLED_W - microWidth(hint)) / 2, 50, hint);
    return;
  }

  // 代号最多 4 字符(PSAX)。3 字符用 3 倍字, 4 字符降到 2 倍, 否则会和右边
  // 的中文名撞上 —— 128px 一行放不下 "PSAX" + "左室短轴" 都用 3 倍字。
  int n = (int)strlen(snap.view);
  int scale = (n <= 3) ? 3 : 2;
  int vw = n * 6 * scale;
  oled.text(6, scale == 3 ? 21 : 25, snap.view, (uint8_t)scale);
  // 后端的 label 可能缺失(模型返回 null 时), 用本地兜底词, 别在屏上留空
  cjkText(6 + vw + 2, 23, snap.label[0] ? snap.label : stateLabel());

  // 置信度条
  const int bx = 6, by = 44, bw = 94, bh = 8;
  rectBox(bx, by, bw, bh, false);
  int filled = (int)((bw - 2) * snap.conf);
  if (filled > 0) rectBox(bx + 1, by + 1, filled, bh - 2, true);

  char pct[8];
  // 带百分号: 光秃秃的 100 贴在进度条右边, 会被读成条上的刻度值而不是概率。
  // 4 个字符 24px, 从 101 起正好收在 125, 不压右边框。
  snprintf(pct, sizeof(pct), "%d%%", (int)(snap.conf * 100 + 0.5f));
  oled.text(101, 44, pct, 1);

  // 细节: 次概率 / 采样帧数 / 数值精度
  const char *second = strcmp(snap.view, "A4C") == 0 ? "PSAX" : "A4C";
  float other = strcmp(snap.view, "A4C") == 0 ? snap.psax : snap.a4c;
  char detail[40];
  snprintf(detail, sizeof(detail), "%s %.2f   %dF   %s",
           second, other, snap.frames, snap.precision[0] ? snap.precision : "--");
  microText(6, 55, detail);
}

static void render(bool force) {
  if (!oledOk) return;
  if (!force && !dirty) return;
  oled.clear();
  drawChrome();
  drawData();
  oled.show();
  dirty = false;
  shown = snap;
}

// ------------------------------------------------------------------ WiFi
enum WifiPhase { WF_IDLE, WF_CONNECTING };
static WifiPhase wifiPhase = WF_IDLE;
static uint32_t wifiTryAt = 0;
static uint32_t wifiStarted = 0;

static void wifiKick() {
  if (!cfg.ssid[0]) return;
  // 反复连同一个热点时, 先断开再连比直接 begin 更容易从失败态恢复
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);       // 关省电: 打开后首包延迟能到 200ms 以上
  WiFi.begin(cfg.ssid, cfg.pass);
  wifiPhase = WF_CONNECTING;
  wifiStarted = millis();
  wifiTryAt = millis() + 15000;
  say("[WIFI] 连接 %s ...\r\n", cfg.ssid);
}

static void wifiTick() {
  uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiPhase != WF_IDLE) {
      wifiPhase = WF_IDLE;
      say("[WIFI] 已连接 %s  信号 %ld dBm  IP %s\r\n",
          WiFi.SSID().c_str(), (long)WiFi.RSSI(), WiFi.localIP().toString().c_str());
      dirty = true;
    }
    return;
  }
  if (wifiPhase == WF_CONNECTING) {
    if (now - wifiStarted > 12000) {          // 单次尝试上限, 不在这里死等
      wifiPhase = WF_IDLE;
      wifiTryAt = now + 3000;
      say("[WIFI] 超时, 3 秒后重试\r\n");
      dirty = true;
    }
    return;
  }
  if (cfg.ssid[0] && (int32_t)(now - wifiTryAt) >= 0) wifiKick();
}

// ------------------------------------------------------------------ 轮询
static bool sameAs(const Snapshot &a, const Snapshot &b) {
  // 逐字段比而不是 memcmp: 结构体有 padding, 字节级比较会在填充位上产生
  // 假"变化", 每轮都触发重绘。
  return a.state == b.state && a.seq == b.seq && a.ageS == b.ageS &&
         a.caseId == b.caseId && a.frames == b.frames &&
         a.conf == b.conf && a.a4c == b.a4c && a.psax == b.psax &&
         !strcmp(a.view, b.view) && !strcmp(a.label, b.label) &&
         !strcmp(a.precision, b.precision);
}

static void pollOnce() {
  static char body[640];
  int code = 0;
  uint32_t t0 = millis();

  if (fetchState(body, sizeof(body), &code)) {
    Snapshot next;
    parseSnapshot(body, &next);
    if (!sameAs(next, snap)) dirty = true;
    snap = next;
    okCount++;
    failStreak = 0;
    pollInterval = POLL_OK_MS;
    lastOkMs = millis();
  } else {
    failCount++;
    failStreak++;
    lastCode = code;
    // 连续失败后退避, 避免断网时每秒打一次 TCP 握手
    pollInterval = (failStreak >= 3) ? POLL_FAIL_MS : POLL_OK_MS;
    if (failStreak == 3) {
      say("[HTTP] 连续失败, 退避到 %lu ms (last code=%d)\r\n",
          (unsigned long)pollInterval, code);
      dirty = true;
    }
  }

  uint32_t dt = millis() - t0;
  if (dt > 400) say("[HTTP] 本次耗时 %lu ms code=%d\r\n", (unsigned long)dt, code);
}

// ------------------------------------------------------------------ 命令
static void printStatus() {
  say("\r\n---- PANEL ----\r\n");
  say("屏      : %s (0x%02X) SDA=%d SCL=%d\r\n", oledOk ? "在线" : "离线", oled.addr, OLED_SDA, OLED_SCL);
  say("WiFi    : %s  ssid=%s  rssi=%ld  ip=%s\r\n",
      WiFi.status() == WL_CONNECTED ? "已连接" : "未连接",
      cfg.ssid, (long)WiFi.RSSI(), WiFi.localIP().toString().c_str());
  say("后端    : %s\r\n", cfg.url);
  say("状态    : %s  seq=%ld  age=%lds  case=%ld\r\n",
      statusWord(), snap.seq, snap.ageS, snap.caseId);
  say("结论    : %s / %s  conf=%.3f  frames=%d  %s\r\n",
      snap.view[0] ? snap.view : "-", stateLabel(), snap.conf, snap.frames,
      snap.precision[0] ? snap.precision : "-");
  say("轮询    : 成功 %lu / 失败 %lu, 当前间隔 %lu ms, 最近 code=%d\r\n",
      (unsigned long)okCount, (unsigned long)failCount,
      (unsigned long)pollInterval, lastCode);
  say("运行    : %lu s  堆 %lu B\r\n", (unsigned long)(millis() / 1000), (unsigned long)ESP.getFreeHeap());
  say("---------------\r\n");
}

static void help() {
  say("\r\n==== EchoPanel ====\r\n");
  say("  WIFI <ssid> <pass>   设置热点并连接 (存 NVS, 掉电保留)\r\n");
  say("  WIFIOFF              擦除已存凭据\r\n");
  say("  URL <url>            设置快照地址\r\n");
  say("  POLL                 立即拉一次\r\n");
  say("  STATUS               状态汇总\r\n");
  say("  RENDER               强制重绘\r\n");
  say("  DEMO <state>         本地演示: ready psax idle running offline\r\n");
  say("  DUMP                 导出帧缓冲 (十六进制, 用于核对屏上内容)\r\n");
  say("  SCAN                 WiFi 扫描\r\n");
  say("  HELP                 本帮助\r\n");
  say("===================\r\n");
}

// 演示模式: 不依赖网络, 用来单独验证屏与字模
static bool demoMode = false;

static void demoState(const char *name) {
  demoMode = true;
  snap = Snapshot();
  if (!strcmp(name, "ready")) {
    snap.state = ST_READY; snap.seq = 1; snap.ageS = 2; snap.caseId = 12;
    strcpy(snap.view, "A4C"); strcpy(snap.label, "四腔心"); strcpy(snap.precision, "FP32");
    snap.conf = 0.92f; snap.a4c = 0.92f; snap.psax = 0.08f; snap.frames = 32;
  } else if (!strcmp(name, "psax")) {
    snap.state = ST_READY; snap.seq = 2; snap.ageS = 5; snap.caseId = 12;
    strcpy(snap.view, "PSAX"); strcpy(snap.label, "左室短轴"); strcpy(snap.precision, "FP32");
    snap.conf = 0.997f; snap.a4c = 0.003f; snap.psax = 0.997f; snap.frames = 8;
  } else if (!strcmp(name, "running")) {
    snap.state = ST_RUNNING; snap.caseId = 12;
  } else if (!strcmp(name, "offline")) {
    snap.state = ST_EMPTY;
  } else {
    snap.state = ST_EMPTY; snap.caseId = 12;
  }
  dirty = true;
  render(true);
  say("[DEMO] %s\r\n", name);
}

static void handle(char *line) {
  while (*line == ' ') line++;
  char *arg = strchr(line, ' ');
  if (arg) *arg++ = 0;

  if (!strcasecmp(line, "HELP") || !strcmp(line, "?")) { help(); return; }
  if (!strcasecmp(line, "STATUS")) { printStatus(); return; }
  if (!strcasecmp(line, "RENDER")) { render(true); say("已重绘\r\n"); return; }
  if (!strcasecmp(line, "DUMP")) {
    // 把 1KB 帧缓冲原样倒出来。屏在实物上, 光靠 STATUS 只能知道画了什么数据、
    // 不知道落成什么样; 有了这段, 版式问题(重叠/裁切)可以在电脑上还原成图看。
    say("FBDUMP %d %d\r\n", OLED_W, OLED_H);
    for (int p = 0; p < OLED_PAGES; p++) {
      for (int i = 0; i < OLED_W; i++) say("%02X", oled.fb[p * OLED_W + i]);
      say("\r\n");
    }
    say("FBEND\r\n");
    return;
  }
  if (!strcasecmp(line, "POLL")) {
    demoMode = false;                 // 退出演示, 回到真实数据
    pollOnce(); render(true); say("已拉取\r\n"); return;
  }
  if (!strcasecmp(line, "SCAN")) {
    say("扫描中...\r\n");
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++)
      say("  %2d  %-28s %4d dBm %s\r\n", i + 1, WiFi.SSID(i).c_str(),
          (int)WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "开放" : "加密");
    WiFi.scanDelete();
    say("共 %d 个\r\n", n);
    return;
  }
  if (!strcasecmp(line, "WIFI")) {
    if (!arg || !*arg) { say("用法: WIFI <ssid> <pass>\r\n"); return; }
    // 支持带引号的 SSID: 热点名里常有空格和中文
    char ssid[33] = "", pass[65] = "";
    if (*arg == '"') {
      arg++;
      char *end = strrchr(arg, '"');
      if (!end) { say("引号没闭合\r\n"); return; }
      *end = 0;
      strncpy(ssid, arg, sizeof(ssid) - 1);
      char *p = end + 1;
      while (*p == ' ') p++;
      strncpy(pass, p, sizeof(pass) - 1);
    } else {
      char *space = strchr(arg, ' ');
      if (space) {
        *space = 0;
        strncpy(pass, space + 1, sizeof(pass) - 1);
      }
      strncpy(ssid, arg, sizeof(ssid) - 1);
    }
    strncpy(cfg.ssid, ssid, sizeof(cfg.ssid) - 1);
    strncpy(cfg.pass, pass, sizeof(cfg.pass) - 1);
    saveConfig();
    say("[WIFI] 已保存 ssid=%s pass=%s\r\n", cfg.ssid, pass[0] ? "***" : "(空)");
    demoMode = false;
    wifiKick();
    return;
  }
  if (!strcasecmp(line, "WIFIOFF")) {
    cfg.ssid[0] = 0; cfg.pass[0] = 0;
    saveConfig();
    WiFi.disconnect(true, false);
    say("[WIFI] 凭据已擦除\r\n");
    dirty = true;
    return;
  }
  if (!strcasecmp(line, "URL")) {
    if (!arg || !*arg) { say("用法: URL http://host:port/api/v1/panel/state\r\n"); return; }
    strncpy(cfg.url, arg, sizeof(cfg.url) - 1);
    saveConfig();
    say("[URL] %s\r\n", cfg.url);
    return;
  }
  if (!strcasecmp(line, "DEMO")) {
    demoState(arg ? arg : "ready");
    return;
  }
  say("不认识: %s (发 HELP)\r\n", line);
}

// ------------------------------------------------------------------ 入口
void setup() {
  Serial.begin(115200);
  delay(120);
  say("\r\n\r\n=== EchoPanel ===\r\n");

  loadConfig();
  say("[CFG] ssid=%s url=%s\r\n", cfg.ssid[0] ? cfg.ssid : "(未设置)", cfg.url);

  Wire.begin(OLED_SDA, OLED_SCL, 400000);
  oledOk = oled.begin(OLED_SDA, OLED_SCL, 400000);
  say("[OLED] %s (0x%02X)\r\n", oledOk ? "在线" : "未找到", oled.addr);
  if (!oledOk) say("[OLED] 检查 3V3/GND/SDA=41/SCL=42\r\n");

  // 先画一屏"启动中", 用户能立刻确认设备活着
  if (oledOk) {
    oled.clear();
    drawLogo(3, 1);
    oled.text(19, 3, "ECHO EDGE", 1);
    microText(19, 15, "PEDIATRIC ECHO");
    const char *msg = cfg.ssid[0] ? "CONNECTING" : "NO WIFI CONFIG";
    microText((OLED_W - microWidth(msg)) / 2, 40, msg);
    oled.show();
  }

  if (cfg.ssid[0]) wifiKick();
  else say("[WIFI] 未配置。发: WIFI \"热点名\" 密码\r\n");
}

void loop() {
  static char line[96];
  static size_t n = 0;

  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    char ch = (char)c;
    if (ch == '\r' || ch == '\n') {
      if (n) { line[n] = 0; n = 0; handle(line); }
    } else if (n < sizeof(line) - 1) {
      line[n++] = ch;
    }
  }

  wifiTick();

  // 演示模式粘住原画面, 直到收到 POLL 或 WIFI 才回到真实数据
  if (!demoMode && WiFi.status() == WL_CONNECTED && millis() - lastPoll >= pollInterval) {
    lastPoll = millis();
    pollOnce();
  }

  render(false);
  delay(10);
}
