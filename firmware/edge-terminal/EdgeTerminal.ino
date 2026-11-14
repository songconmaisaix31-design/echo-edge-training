/*
 * EdgeTerminal - 儿童心超终端主固件（ESP32-S3）
 *
 * 对应仓库 issue #2 / #4 / #5 / #6 的终端侧实现。
 *
 * 一、屏幕（issue #2）
 *   SSD1306 128x64, I2C 0x3C, SDA=GPIO41 / SCL=GPIO42
 *   三行读数各自独立有效标志: 任一传感器掉线只影响自己那一项, 其余继续刷新
 *   掉线的传感器每 2 秒自动重探, 接回来自动恢复, 不需要复位
 *   串口同步输出人读行 + 机器可读 ENV 行
 *
 * 二、执行器（issue #4 / #5）
 *   振动马达  GPIO4
 *   语音 SYN6288E  UART2 9600 8N1, TX=GPIO5 RX=GPIO6
 *   舵机 x2   GPIO7 / GPIO15, 50Hz PWM, 独立外部电源
 *
 * 三、电脑联动（issue #6）
 *   电脑发 "HINT <n>" 或 "HINT <文本>", 终端显示 + 播报 + 振动, 并回 "ACK,<seq>"
 *   回执带序号, 用于统计发送数与到达数
 *
 * 四、红外（issue #3 的替代, 不能当心跳用）
 *   ITR9909 只有 LM393 比较输出(DO), 没有模拟通道, 采不到脉搏波。
 *   这里只做"有/无遮挡"检测, 真实脉率需要另接 PPG 器件(如 MAX30102)。
 *
 * 引脚禁区: 43/44=UART0 调试口, 26~37=Flash/PSRAM, 19/20=USB, 41/42=屏 I2C, 17/18=ECW 桥
 *
 * 串口 115200 8N1, 命令以 \r 或 \n 结尾
 */

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "oled_ssd1306.h"
#include "env_sensors.h"

// 上电默认启用的 mock 位。
// 本批实物只有屏和 TTS，环境三项没有器件，所以默认开环境三项，让链路有数据可跑。
// 器件一旦真的上线，mock 自动让位（见 EnvHub::poll 里的 everSeen 判定）；
// 想验证"传感器掉线该项显示不可用"，发 MOCK OFF 即可。
#define DEFAULT_MOCK MOCK_ENV

// ------------------------------------------------------------------ 引脚
static const int PIN_OLED_SDA = 41;
static const int PIN_OLED_SCL = 42;
static const int PIN_VIB      = 4;
static const int PIN_TTS_TX   = 5;
static const int PIN_TTS_RX   = 6;
static const int PIN_SERVO_1  = 7;
static const int PIN_SERVO_2  = 15;
static const int PIN_IR_DO    = 1;

// ------------------------------------------------------------------ 全局
static Oled oled;
static EnvHub env;
static Mock mockSrc;                     // 无实物器件时的数据源替身
static bool oledOk = false;

static char hintLine[24] = "READY";      // 屏底部显示的最近提示
static uint32_t hintSeqIn = 0;           // 收到的提示数
static uint32_t hintAcked = 0;           // 已回执的提示数
static bool autoStream = true;           // 周期输出开关
static uint32_t lastPoll = 0, lastStream = 0, lastDraw = 0;
static uint8_t irLast = 0;

// ------------------------------------------------------------------ 输出
static void say(const char *fmt, ...) {
  char b[224];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  Serial.print(b);
}

// ------------------------------------------------------------------ 预置提示语
// SYN6288E 只认 GB2312/GBK，Arduino 里是 UTF-8，运行时转码不现实 → 预置
struct Phrase { const char *label; const uint8_t *gb; uint8_t len; };

static const uint8_t P1[] = {0xCD,0xBC,0xCF,0xF1,0xC7,0xE5,0xCE,0xFA,0xBF,0xC9,0xD2,0xD4,0xB2,0xC9,0xBC,0xAF};
static const uint8_t P2[] = {0xC7,0xEB,0xB5,0xF7,0xD5,0xFB,0xCC,0xBD,0xCD,0xB7,0xCE,0xBB,0xD6,0xC3};
static const uint8_t P3[] = {0xD0,0xC5,0xBA,0xC5,0xD6,0xCA,0xC1,0xBF,0xB2,0xBB,0xD7,0xE3};
static const uint8_t P4[] = {0xB2,0xC9,0xBC,0xAF,0xCD,0xEA,0xB3,0xC9};
static const uint8_t P5[] = {0xC7,0xEB,0xB1,0xA3,0xB3,0xD6,0xBE,0xB2,0xD6,0xB9};
static const uint8_t P6[] = {0xCC,0xBD,0xCD,0xB7,0xD2,0xD1,0xBE,0xCD,0xCE,0xBB};
static const uint8_t P7[] = {0xC7,0xEB,0xD7,0xA2,0xD2,0xE2};
static const uint8_t P8[] = {0xBB,0xB7,0xBE,0xB3,0xB6,0xC1,0xCA,0xFD,0xBE,0xCD,0xD0,0xF7};

static const Phrase PHRASES[] = {
  {"图像清晰可以采集", P1, sizeof(P1)},
  {"请调整探头位置",   P2, sizeof(P2)},
  {"信号质量不足",     P3, sizeof(P3)},
  {"采集完成",         P4, sizeof(P4)},
  {"请保持静止",       P5, sizeof(P5)},
  {"探头已就位",       P6, sizeof(P6)},
  {"请注意",           P7, sizeof(P7)},
  {"环境读数就绪",     P8, sizeof(P8)},
};
static const int PHRASE_N = (int)(sizeof(PHRASES) / sizeof(PHRASES[0]));

// 屏上用的简短英文标签（5x7 字库没有中文）
static const char *PHRASE_LABEL[] = {
  "IMG OK", "ADJUST PROBE", "QUALITY LOW", "CAPTURE DONE",
  "HOLD STILL", "PROBE READY", "ATTENTION", "ENV READY"
};

// ------------------------------------------------------------------ 语音
// 帧: 0xFD | lenH | lenL | 0x01(合成播放) | 0x00(GB2312) | data... | XOR
// len = 1(命令) + 1(编码) + data 长度; XOR 覆盖 0xFD 到 data 末尾
static void ttsSpeakRaw(const uint8_t *gb, uint8_t n) {
  uint16_t len = (uint16_t)(n + 2);
  uint8_t frame[64];
  if (n > 56) n = 56;

  frame[0] = 0xFD;
  frame[1] = (uint8_t)(len >> 8);
  frame[2] = (uint8_t)(len & 0xFF);
  frame[3] = 0x01;
  frame[4] = 0x00;
  memcpy(frame + 5, gb, n);

  uint8_t x = 0;
  for (int i = 0; i < 5 + n; i++) x ^= frame[i];
  frame[5 + n] = x;

  Serial2.write(frame, 6 + n);
  Serial2.flush();
  say("[TTS] 已发 %d 字节 (data=%d, xor=0x%02X)\r\n", 6 + n, n, x);
}

static void ttsSpeak(int idx) {
  if (idx < 1 || idx > PHRASE_N) { say("提示语序号 1..%d\r\n", PHRASE_N); return; }
  ttsSpeakRaw(PHRASES[idx - 1].gb, PHRASES[idx - 1].len);
}

// ------------------------------------------------------------------ 振动
static void vibPulse(uint32_t ms) {
  if (ms > 3000) ms = 3000;
  digitalWrite(PIN_VIB, HIGH);
  delay(ms);
  digitalWrite(PIN_VIB, LOW);
  say("[VIB] %lu ms\r\n", (unsigned long)ms);
}

// ------------------------------------------------------------------ 舵机
// 50Hz, 16 位分辨率: 一个周期 20ms = 65536
// 0.5ms -> 1638, 1.5ms -> 4915, 2.5ms -> 8192
// 手上暂时没有舵机, 所以改成"首次发命令才初始化 PWM", 免得开机就占着引脚。
static bool servoInited1 = false, servoInited2 = false;

static void servoWrite(int ch, float deg) {
  if (deg < 0) deg = 0;
  if (deg > 180) deg = 180;
  int pin = (ch == 2) ? PIN_SERVO_2 : PIN_SERVO_1;
  bool &ini = (ch == 2) ? servoInited2 : servoInited1;
  if (!ini) { ledcAttach(pin, 50, 16); ini = true; }
  float ms = 0.5f + (deg / 180.0f) * 2.0f;
  uint32_t duty = (uint32_t)(ms / 20.0f * 65535.0f);
  ledcWrite(pin, duty);
  say("[SERVO%d] %.1f deg -> %.2f ms (duty %lu)\r\n",
      (ch == 2) ? 2 : 1, deg, ms, (unsigned long)duty);
}

// ------------------------------------------------------------------ 屏
static void drawBadge(int x, int y, bool ok) {
  if (ok) {
    oled.hline(x, y, 6); oled.hline(x, y + 1, 6); oled.hline(x, y + 2, 6);
    oled.hline(x, y + 3, 6); oled.hline(x, y + 4, 6);
  } else {
    oled.hline(x, y, 6);
    oled.hline(x, y + 4, 6);
    oled.px(x, y + 1); oled.px(x + 5, y + 1);
    oled.px(x + 2, y + 2); oled.px(x + 3, y + 2);
    oled.px(x, y + 3); oled.px(x + 5, y + 3);
  }
}

// mock 值不能和真实值长得一样，否则截图流出去分不清真假。打一个 M 角标。
static void drawFlag(int x, int y, bool ok, bool isMock) {
  if (isMock) { oled.text(x, y, "M", 1); return; }
  drawBadge(x, y, ok);
}

static void drawEnvPage() {
  if (!oledOk) return;

  char line[24];
  oled.clear();

  unsigned long s = millis() / 1000;
  snprintf(line, sizeof(line), "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  oled.text(0, 0, "ENV", 1);
  oled.text(OLED_W - (int)strlen(line) * 6, 0, line, 1);
  oled.hline(0, 9, OLED_W);

  // 温度
  bool mT = (env.state.mockMask & MOCK_TEMP) != 0;
  if (env.state.tempValid) snprintf(line, sizeof(line), "TEMP %5.1f C", env.state.tempC);
  else                     snprintf(line, sizeof(line), "TEMP   --   ");
  oled.text(0, 13, line, 1);
  drawFlag(116, 13, env.state.tempValid, mT);

  // 湿度
  bool mH = (env.state.mockMask & MOCK_RH) != 0;
  if (env.state.rhValid) snprintf(line, sizeof(line), "RH   %5.1f %%", env.state.rhPct);
  else                   snprintf(line, sizeof(line), "RH     --   ");
  oled.text(0, 23, line, 1);
  drawFlag(116, 23, env.state.rhValid, mH);

  // 气压
  bool mP = (env.state.mockMask & MOCK_PRES) != 0;
  if (env.state.pressValid) snprintf(line, sizeof(line), "PRES %6.1f hPa", env.state.pressHpa);
  else                      snprintf(line, sizeof(line), "PRES    --     ");
  oled.text(0, 33, line, 1);
  drawFlag(116, 33, env.state.pressValid, mP);

  oled.hline(0, 42, OLED_W);

  // 底部: 最近一条电脑提示
  oled.text(0, 46, hintLine, 1);

  // 串口在线指示 + 红外
  if (irLast) oled.text(0, 56, "IR: NEAR", 1);
  else        oled.text(0, 56, "IR: ---", 1);
  snprintf(line, sizeof(line), "IN%lu", (unsigned long)hintSeqIn);
  oled.text(OLED_W - (int)strlen(line) * 6, 56, line, 1);

  oled.show();
}

static void setHintLine(const char *s) {
  strncpy(hintLine, s, sizeof(hintLine) - 1);
  hintLine[sizeof(hintLine) - 1] = 0;
}

// ------------------------------------------------------------------ 提示动作
// 电脑发来的提示: 屏 + 语音 + 振动, 然后回执
static void fireHint(int idx, const char *text) {
  hintSeqIn++;

  if (idx >= 1 && idx <= PHRASE_N) {
    setHintLine(PHRASE_LABEL[idx - 1]);
    ttsSpeak(idx);
  } else if (text && *text) {
    setHintLine(text);
    say("[HINT] 文本提示仅上屏, 不播报(需 GB2312 预置)\r\n");
  }

  vibPulse(150);
  drawEnvPage();

  hintAcked++;
  say("ACK,%lu\r\n", (unsigned long)hintSeqIn);
}

// ------------------------------------------------------------------ 环境读数输出
static void printEnvHuman() {
  char t[16], h[16], p[16], mk[24];
  snprintf(t, sizeof(t), env.state.tempValid  ? "%.2f" : "--", env.state.tempValid  ? env.state.tempC  : 0.0f);
  snprintf(h, sizeof(h), env.state.rhValid    ? "%.2f" : "--", env.state.rhValid    ? env.state.rhPct  : 0.0f);
  snprintf(p, sizeof(p), env.state.pressValid ? "%.2f" : "--", env.state.pressValid ? env.state.pressHpa : 0.0f);
  mk[0] = 0;
  if (env.state.mockMask) {
    char names[20] = "";
    if (env.state.mockMask & MOCK_TEMP) strcat(names, "T ");
    if (env.state.mockMask & MOCK_RH)   strcat(names, "RH ");
    if (env.state.mockMask & MOCK_PRES) strcat(names, "P ");
    snprintf(mk, sizeof(mk), "[MOCK:%s] ", names);
  }
  say("[ENV] #%lu t=%s C  rh=%s %%  p=%s hPa  %s%s%s%s\r\n",
      (unsigned long)env.state.seq, t, h, p, mk,
      env.state.tempValid ? "" : "[TEMP 离线] ",
      env.state.rhValid ? "" : "[RH 离线] ",
      env.state.pressValid ? "" : "[PRES 离线]");
}

// 机器可读, 固定字段顺序, 电脑侧直接 split
//   ENV,<t>,<rh>,<p>,<tv>,<rv>,<pv>,<stamp_ms>,<mock_mask>
// 第 9 列是 mock 位图（1=TEMP 2=RH 4=PRES），0 表示三项都来自实物。
// 老解析器读前 8 列不受影响。
static void printEnvCsv() {
  say("ENV,%.2f,%.2f,%.2f,%d,%d,%d,%lu,%u\r\n",
      env.state.tempValid ? env.state.tempC : 0.0f,
      env.state.rhValid ? env.state.rhPct : 0.0f,
      env.state.pressValid ? env.state.pressHpa : 0.0f,
      env.state.tempValid ? 1 : 0,
      env.state.rhValid ? 1 : 0,
      env.state.pressValid ? 1 : 0,
      (unsigned long)env.state.stampMs,
      (unsigned)env.state.mockMask);
}

// ------------------------------------------------------------------ I2C 扫描
static void i2cScan() {
  say("\r\n[SCAN] SDA=%d SCL=%d\r\n", PIN_OLED_SDA, PIN_OLED_SCL);
  int n = 0;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      const char *who = "";
      if (a == 0x3C || a == 0x3D) who = " SSD1306 屏";
      else if (a == 0x44)         who = " GXHT30C 温湿度";
      else if (a == 0x76)         who = " BMP388 气压";
      else if (a == 0x77)         who = " BMP388 气压(备用地址)";
      say("  0x%02X%s\r\n", a, who);
      n++;
    }
  }
  if (!n) say("  零命中 -> 检查 3V3 / GND / SDA=41 / SCL=42\r\n");
  else    say("  共 %d 个器件\r\n", n);
}

// ------------------------------------------------------------------ MOCK 控制
static void mockStatus() {
  char b[40];
  mockSrc.describe(b, sizeof(b));
  say("\r\n[MOCK] 启用位: %s\r\n", b);
  say("  温度  : %s\r\n",
      (!env.thEverSeen && mockSrc.on(MOCK_TEMP)) ? "MOCK 接管"
      : (env.thEverSeen ? "实物曾经在线 -> 不接管" : "无数据"));
  say("  湿度  : %s\r\n",
      (!env.thEverSeen && mockSrc.on(MOCK_RH)) ? "MOCK 接管"
      : (env.thEverSeen ? "实物曾经在线 -> 不接管" : "无数据"));
  say("  气压  : %s\r\n",
      (!env.baroEverSeen && mockSrc.on(MOCK_PRES)) ? "MOCK 接管"
      : (env.baroEverSeen ? "实物曾经在线 -> 不接管" : "无数据"));
  say("  基线  : T=%.2f C  RH=%.2f %%  P=%.2f hPa\r\n",
      mockSrc.tempBase, mockSrc.rhBase, mockSrc.presBase);
  say("  规则  : 实物上线即让位; 曾上线后掉线则如实显示离线, 不用 mock 兜底\r\n");
  say("  标记  : 屏上打 M 角标, 串口行标 [MOCK:..], CSV 第 9 列置位\r\n");
}

static void mockCmd(char *a) {
  while (a && *a == ' ') a++;
  if (!a || !*a) { mockStatus(); return; }

  char verb[16];
  char *sp = strchr(a, ' ');
  size_t vl = sp ? (size_t)(sp - a) : strlen(a);
  if (vl >= sizeof(verb)) vl = sizeof(verb) - 1;
  memcpy(verb, a, vl);
  verb[vl] = 0;
  char *rest = sp ? sp + 1 : NULL;
  while (rest && *rest == ' ') rest++;

  if (!strcasecmp(verb, "BASE")) {
    float t = 0, h = 0, p = 0;
    int got = rest ? sscanf(rest, "%f %f %f", &t, &h, &p) : 0;
    if (got >= 1 && t != 0) mockSrc.tempBase = t;
    if (got >= 2 && h != 0) mockSrc.rhBase = h;
    if (got >= 3 && p != 0) mockSrc.presBase = p;
    say("[MOCK] 基线 -> T=%.2f RH=%.2f P=%.2f\r\n",
        mockSrc.tempBase, mockSrc.rhBase, mockSrc.presBase);
    mockStatus();
    return;
  }

  if (!strcasecmp(verb, "ON"))  { mockSrc.enable(MOCK_ALL); say("[MOCK] 全部打开\r\n");  mockStatus(); return; }
  if (!strcasecmp(verb, "OFF")) { mockSrc.clear();          say("[MOCK] 全部关闭\r\n");  mockStatus(); return; }

  bool en = true;
  if (rest && !strcasecmp(rest, "OFF")) en = false;
  uint8_t bit = mockSrc.byName(verb, en);
  if (!bit) { say("不认识: MOCK %s (TEMP/RH/PRES/ENV/ALL ON|OFF, BASE t rh p)\r\n", verb); return; }

  say("[MOCK] %s %s\r\n", verb, en ? "打开" : "关闭");
  mockStatus();
}

// ------------------------------------------------------------------ 命令
static void help() {
  say("\r\n==== EdgeTerminal ====\r\n");
  say("  READ              立即采一轮并打印\r\n");
  say("  AUTO ON|OFF       周期输出开关 (默认 ON, 1 Hz)\r\n");
  say("  SCAN              I2C 总线扫描\r\n");
  say("  REGS              BMP388 寄存器快照 (判定厂商序列对不对)\r\n");
  say("  STAT              状态汇总\r\n");
  say("  MOCK              查看 mock 状态\r\n");
  say("  MOCK ON|OFF       环境三项 mock 全开/全关\r\n");
  say("  MOCK TEMP ON|OFF  单源开关 (TEMP/RH/PRES/ENV/ALL)\r\n");
  say("  MOCK BASE t rh p  改 mock 基线, 例如 MOCK BASE 36.5 45 1010\r\n");
  say("  HINT <n>          触发第 n 条提示 (1..8): 屏+播报+振动+回执\r\n");
  say("  HINT <text>       只上屏不播报 (英文)\r\n");
  say("  TTS <n>           只播报第 n 条\r\n");
  say("  PHRASES           列出预置提示语\r\n");
  say("  VIB <ms>          振动一次 (本批硬件无马达, 仅验证 GPIO 时序)\r\n");
  say("  SERVO <1|2> <deg> 舵机到角度 (本批硬件无舵机, 仅验证 PWM)\r\n");
  say("  IR                读红外 DO\r\n");
  say("  PINS              当前引脚分配\r\n");
  say("\r\n");
  for (int i = 0; i < PHRASE_N; i++) {
    say("  %d = %s (%s, %u 字节)\r\n", i + 1, PHRASES[i].label,
        PHRASE_LABEL[i], (unsigned)PHRASES[i].len);
  }
  say("\r\n");
}

static void stat() {
  char b[40];
  say("\r\n---- STAT ----\r\n");
  say("屏        : %s (0x%02X)\r\n", oledOk ? "在线" : "离线", oled.addr);
  say("温湿度    : %s  采样 %lu 次 / 失败 %lu 次 (err=%u)%s\r\n",
      env.th.present ? "在线" : "离线",
      (unsigned long)env.th.okCount, (unsigned long)env.th.errCount, env.th.lastErr,
      (!env.th.present && !env.thEverSeen && mockSrc.on(MOCK_TEMP)) ? "  <- mock 接管" : "");
  say("气压      : %s  chipId=0x%02X  采样 %lu / 失败 %lu (err=%u)%s\r\n",
      env.baro.present ? "在线" : "离线", env.baro.chipId,
      (unsigned long)env.baro.okCount, (unsigned long)env.baro.errCount, env.baro.lastErr,
      (!env.baro.present && !env.baroEverSeen && mockSrc.on(MOCK_PRES)) ? "  <- mock 接管" : "");
  say("温度值    : %s\r\n", env.state.tempValid ? "有效" : "无效");
  say("湿度值    : %s\r\n", env.state.rhValid ? "有效" : "无效");
  say("气压值    : %s\r\n", env.state.pressValid ? "有效" : "无效");
  say("MOCK      : %s\r\n", mockSrc.describe(b, sizeof(b)));
  say("提示收/回 : %lu / %lu\r\n", (unsigned long)hintSeqIn, (unsigned long)hintAcked);
  say("红外 DO   : %d\r\n", irLast);
  say("运行      : %lu s\r\n", (unsigned long)(millis() / 1000));
  say("---------------\r\n");
}

static void pins() {
  say("\r\n---- PINS ----\r\n");
  say("OLED  SDA=%d SCL=%d\r\n", PIN_OLED_SDA, PIN_OLED_SCL);
  say("振动  GPIO%d\r\n", PIN_VIB);
  say("TTS   TX=%d RX=%d @9600\r\n", PIN_TTS_TX, PIN_TTS_RX);
  say("舵机1 GPIO%d  舵机2 GPIO%d\r\n", PIN_SERVO_1, PIN_SERVO_2);
  say("红外  DO=GPIO%d\r\n", PIN_IR_DO);
  say("ECW   桥 TX=17 RX=18 (本固件不占用)\r\n");
  say("--------------\r\n");
}

static void handle(char *line) {
  while (*line == ' ') line++;
  if (!*line) return;

  char *arg = strchr(line, ' ');
  if (arg) { *arg = 0; arg++; while (*arg == ' ') arg++; }

  if (!strcasecmp(line, "HELP") || !strcasecmp(line, "?")) { help(); return; }
  if (!strcasecmp(line, "READ"))  { env.poll(); printEnvHuman(); printEnvCsv(); drawEnvPage(); return; }
  if (!strcasecmp(line, "SCAN"))  { i2cScan(); return; }
  if (!strcasecmp(line, "STAT"))  { stat(); return; }
  if (!strcasecmp(line, "PINS"))  { pins(); return; }
  if (!strcasecmp(line, "PHRASES")) { help(); return; }
  if (!strcasecmp(line, "MOCK"))  { mockCmd(arg); env.poll(); drawEnvPage(); printEnvHuman(); return; }
  if (!strcasecmp(line, "REGS"))  { env.baro.dumpRegs(say); return; }
  if (!strcasecmp(line, "IR"))    { irLast = (uint8_t)digitalRead(PIN_IR_DO); say("IR DO = %d\r\n", irLast); return; }

  if (!strcasecmp(line, "AUTO")) {
    if (arg && !strcasecmp(arg, "OFF")) { autoStream = false; say("周期输出已关\r\n"); }
    else                                { autoStream = true;  say("周期输出已开\r\n"); }
    return;
  }

  if (!strcasecmp(line, "HINT")) {
    if (!arg || !*arg) { say("用法: HINT 1  或  HINT IMG OK\r\n"); return; }
    int n = atoi(arg);
    if (n >= 1 && n <= PHRASE_N) fireHint(n, NULL);
    else                          fireHint(0, arg);
    return;
  }

  if (!strcasecmp(line, "TTS")) {
    int n = arg ? atoi(arg) : 0;
    ttsSpeak(n);
    return;
  }

  if (!strcasecmp(line, "VIB")) {
    vibPulse(arg ? (uint32_t)atoi(arg) : 150);
    return;
  }

  if (!strcasecmp(line, "SERVO")) {
    int ch = 1; float deg = 90;
    if (arg) sscanf(arg, "%d %f", &ch, &deg);
    servoWrite(ch, deg);
    return;
  }

  say("不认识: %s (发 HELP)\r\n", line);
}

// ------------------------------------------------------------------ 主流程
void setup() {
  Serial.begin(115200);
  delay(300);

  say("\r\n\r\n==== EdgeTerminal / 儿童心超终端 ====\r\n");

  // --- 执行器先初始化, 保证上电即处于安全状态 ---
  pinMode(PIN_VIB, OUTPUT);
  digitalWrite(PIN_VIB, LOW);

  pinMode(PIN_IR_DO, INPUT);

  Serial2.begin(9600, SERIAL_8N1, PIN_TTS_RX, PIN_TTS_TX);
  say("[TTS] UART2 9600 8N1, TX=GPIO%d RX=GPIO%d\r\n", PIN_TTS_TX, PIN_TTS_RX);

  // 舵机不在本批硬件内, 不在此处初始化 PWM —— 等 SERVO 命令来了再说

  // --- I2C 总线: 屏 + 温湿度 + 气压 共用 ---
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, 400000);
  Wire.setClock(400000);

  oledOk = oled.begin((uint8_t)PIN_OLED_SDA, (uint8_t)PIN_OLED_SCL);
  say("[OLED] %s, 地址 0x%02X\r\n", oledOk ? "已初始化" : "未找到", oled.addr);

  if (!oledOk) {
    i2cScan();   // 屏都没有, 先把总线现状打出来
  }

  env.mock = &mockSrc;
  mockSrc.enable(DEFAULT_MOCK);
  env.begin();
  env.poll();
  say("[ENV] 温湿度=%s 气压=%s\r\n",
      env.th.present ? "在线" : "离线",
      env.baro.present ? "在线" : "离线");
  if (mockSrc.mask) {
    char b[40];
    say("[MOCK] 以下数据源无实物, 由 mock 接管: %s\r\n", mockSrc.describe(b, sizeof(b)));
    say("[MOCK] 屏上打 M 角标 / 串口标 [MOCK:..] / CSV 第 9 列置位; MOCK OFF 可关闭\r\n");
  }

  setHintLine("READY");
  drawEnvPage();

  printEnvHuman();
  printEnvCsv();
  help();
}

void loop() {
  static char line[128];
  static size_t n = 0;

  uint32_t now = millis();

  // 采样: 1 Hz
  if (now - lastPoll >= 1000) {
    lastPoll = now;
    env.poll();

    uint8_t ir = (uint8_t)digitalRead(PIN_IR_DO);
    if (ir != irLast) {
      irLast = ir;
      say("[IR] %s\r\n", ir ? "NEAR(有遮挡)" : "FAR(无遮挡)");
    }

    drawEnvPage();

    if (autoStream) {
      lastStream = now;
      printEnvHuman();
      printEnvCsv();
    }
  }

  // 屏掉线时每 3 秒重试
  static uint32_t lastOledTry = 0;
  if (!oledOk && now - lastOledTry >= 3000) {
    lastOledTry = now;
    oledOk = oled.begin((uint8_t)PIN_OLED_SDA, (uint8_t)PIN_OLED_SCL);
    if (oledOk) say("[OLED] 重新连上 0x%02X\r\n", oled.addr);
  }

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
}
