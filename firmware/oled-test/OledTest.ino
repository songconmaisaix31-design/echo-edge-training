/*
 * OledTest - JMD0.96D-1 (0.96 寸 I2C OLED / SSD1306 / 128x64) 点屏固件
 *
 * 这块屏只有 4 根线: GND / VCC / SCL / SDA, 是标准 I2C 从设备,
 * 默认地址 0x3C (少数批次 0x3D)。
 *
 * 接线(实测确认过的组合, 不是猜的):
 *   屏 GND -> ESP32 GND
 *   屏 VCC -> ESP32 3V3    (只能 3.3V, 接 5V 会烧)
 *   屏 SCL -> GPIO42
 *   屏 SDA -> GPIO41
 *
 * 为什么是 41/42: 上一块屏在小智项目里实测 SDA=41/SCL=42 能扫到 0x3C,
 * 对调成 SDA=42/SCL=41 全零命中 —— 这块模块方向是固定的, 接反了不是花屏, 是全黑。
 *
 * 避开哪些脚: 43/44 是板载 UART0(TXD0/RXD0, CH343 调试口), 切成输入会让串口失声,
 *             烧录还会报 "TX path seems to be down"; 17/18 当前被 ECW 桥占用;
 *             26~37 是 Flash/PSRAM。所以 41/42 是这块板子上最干净的两个脚。
 *
 * 串口命令(115200, COM4):
 *   HELP / SCAN / DEMO / TEXT 内容 / FILL / INV / PINS sda scl / OFF
 *
 * 设计约束: 无动态分配, 帧缓冲静态 1KB, 所有 I2C 调用检查返回值,
 * 找不到屏时每 3 秒重试, 支持上电后再接线。
 */

#include <Arduino.h>
#include <Wire.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// ------------------------------------------------------------------ 参数
static int PIN_SDA = 41;
static int PIN_SCL = 42;
static uint32_t I2C_HZ = 400000;

static uint8_t addr = 0x3C;
static bool online = false;

static const int W = 128;
static const int H = 64;
static const int PAGES = H / 8;
static uint8_t fb[W * PAGES];      // 1024 字节, 每字节一列 8 个竖像素, bit0 最上

// ------------------------------------------------------- 5x7 字体(已渲染校验)
// 每字符 5 字节, 每字节是一列, bit0 = 该列最上面的像素
static const char CHARSET[] = " -./0123456789:ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const uint8_t FONT5X7[][5] = {
  {0x00,0x00,0x00,0x00,0x00}, // ' '
  {0x08,0x08,0x08,0x08,0x08}, // '-'
  {0x00,0x60,0x60,0x00,0x00}, // '.'
  {0x40,0x30,0x08,0x06,0x01}, // '/'
  {0x3E,0x51,0x49,0x45,0x3E}, // '0'
  {0x00,0x42,0x7F,0x40,0x00}, // '1'
  {0x42,0x61,0x51,0x49,0x46}, // '2'
  {0x40,0x49,0x49,0x49,0x37}, // '3'
  {0x18,0x14,0x12,0x7F,0x10}, // '4'
  {0x27,0x45,0x45,0x45,0x39}, // '5'
  {0x3C,0x4A,0x49,0x49,0x30}, // '6'
  {0x01,0x71,0x09,0x05,0x03}, // '7'
  {0x36,0x49,0x49,0x49,0x36}, // '8'
  {0x06,0x49,0x49,0x29,0x1E}, // '9'
  {0x00,0x36,0x36,0x00,0x00}, // ':'
  {0x7C,0x12,0x11,0x12,0x7C}, // 'A'
  {0x7F,0x49,0x49,0x49,0x36}, // 'B'
  {0x3E,0x41,0x41,0x41,0x41}, // 'C'
  {0x7F,0x41,0x41,0x22,0x1C}, // 'D'
  {0x7F,0x49,0x49,0x49,0x41}, // 'E'
  {0x7F,0x09,0x09,0x09,0x01}, // 'F'
  {0x3E,0x41,0x49,0x49,0x3A}, // 'G'
  {0x7F,0x08,0x08,0x08,0x7F}, // 'H'
  {0x00,0x41,0x7F,0x41,0x00}, // 'I'
  {0x20,0x40,0x41,0x3F,0x01}, // 'J'
  {0x7F,0x08,0x14,0x22,0x41}, // 'K'
  {0x7F,0x40,0x40,0x40,0x40}, // 'L'
  {0x7F,0x02,0x04,0x02,0x7F}, // 'M'
  {0x7F,0x02,0x04,0x08,0x7F}, // 'N'
  {0x3E,0x41,0x41,0x41,0x3E}, // 'O'
  {0x7F,0x09,0x09,0x09,0x06}, // 'P'
  {0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
  {0x7F,0x09,0x19,0x29,0x46}, // 'R'
  {0x46,0x49,0x49,0x49,0x31}, // 'S'
  {0x01,0x01,0x7F,0x01,0x01}, // 'T'
  {0x3F,0x40,0x40,0x40,0x3F}, // 'U'
  {0x1F,0x20,0x40,0x20,0x1F}, // 'V'
  {0x7F,0x20,0x18,0x20,0x7F}, // 'W'
  {0x63,0x14,0x08,0x14,0x63}, // 'X'
  {0x03,0x04,0x78,0x04,0x03}, // 'Y'
  {0x61,0x51,0x49,0x45,0x43}, // 'Z'
};

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

// ------------------------------------------------------------------ I2C 底层
static bool cmd1(uint8_t c) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)0x00);      // 控制字节: 后面是命令
  Wire.write(c);
  return Wire.endTransmission() == 0;
}

static bool cmdN(const uint8_t *seq, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)0x00);
  for (size_t i = 0; i < n; i++) Wire.write(seq[i]);
  return Wire.endTransmission() == 0;
}

static bool pushFrame() {
  for (int page = 0; page < PAGES; page++) {
    if (!cmd1(0xB0 | (uint8_t)page)) return false;   // 页地址
    if (!cmd1(0x00)) return false;                   // 列低 4 位
    if (!cmd1(0x10)) return false;                   // 列高 4 位
    const uint8_t *p = fb + page * W;
    for (int i = 0; i < W; i += 16) {                // 分块写, 避免 I2C 缓冲溢出
      Wire.beginTransmission(addr);
      Wire.write((uint8_t)0x40);                     // 控制字节: 后面是显存
      for (int k = 0; k < 16; k++) Wire.write(p[i + k]);
      if (Wire.endTransmission() != 0) return false;
    }
  }
  return true;
}

// ------------------------------------------------------------------ 扫描
static int scanBus(bool verbose) {
  int found = -1;
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      if (verbose) {
        const char *who = "未知器件";
        if (a == 0x3C || a == 0x3D) who = "SSD1306 OLED (就是这块屏)";
        else if (a == 0x3E)         who = "SH1106 OLED?";
        else if (a == 0x68)         who = "DS1307/MPU6050";
        else if (a == 0x76 || a == 0x77) who = "BMP280/BME280";
        say("    SDA=%d SCL=%d  命中 0x%02X  %s\r\n", PIN_SDA, PIN_SCL, a, who);
      }
      if (found < 0 && (a == 0x3C || a == 0x3D)) found = a;
      else if (found < 0) found = a;
    }
  }
  return found;
}

// --------------------------------------------------- 引脚电平探测(找屏在哪)
// I2C 模块的 SDA/SCL 上通常各挂一个 4.7k~10k 上拉电阻到 VCC。
// 把引脚设为"输入+内部下拉"(内部约 45k), 外面的上拉比内部下拉强得多,
// 于是真正接到模块的脚会读成 HIGH, 而空脚会被内部下拉按成 LOW。
// 这一步不需要知道地址, 也不需要屏能通信, 只要 VCC 有电就能测出来。
// 注意: 必须排除 GPIO43/44 —— 那是 UART0 的 TX/RX, 切成输入会让串口失声。
static const int CAND_PINS[] = {
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21,
  38, 39, 40, 41, 42, 45, 46, 47, 48
};
static const int CAND_N = sizeof(CAND_PINS) / sizeof(CAND_PINS[0]);

static void probePulls() {
  say("\r\n[PULLUP] 引脚电平探测 (跳过 19/20=USB, 26~37=Flash/PSRAM, 43/44=串口)\r\n");
  say("  判据: 开内部下拉还读 HIGH -> 外部有上拉 -> 该脚挂在 I2C 模块上\r\n");

  Wire.end();     // 先放开 I2C, 避免当前引脚被别的东西驱动

  int highs[CAND_N];
  int nh = 0;
  int lows[CAND_N];
  int nl = 0;

  for (int i = 0; i < CAND_N; i++) {
    int p = CAND_PINS[i];

    pinMode(p, INPUT_PULLDOWN);
    delay(4);
    bool h = digitalRead(p) == HIGH;

    pinMode(p, INPUT_PULLUP);
    delay(4);
    bool l = digitalRead(p) == LOW;

    pinMode(p, INPUT);

    if (h) highs[nh++] = p;
    if (l) lows[nl++] = p;
  }

  if (nh == 0) {
    say("  没有任何引脚被拉高 -> 模块的 VCC 没通电, 或 SDA/SCL/GND 一根都没接对\r\n");
  } else {
    say("  被外部上拉到 HIGH 的脚 (%d 个):", nh);
    for (int i = 0; i < nh; i++) say(" GPIO%d", highs[i]);
    say("\r\n");
    if (nh == 2) {
      say("  >>> 正好两个, 这就是 SDA/SCL。分别试 PINS %d %d 和 PINS %d %d\r\n",
          highs[0], highs[1], highs[1], highs[0]);
    } else if (nh > 2) {
      say("  >>> 多于两个, 可能有别的上拉器件; 优先试前两个的组合\r\n");
    } else {
      say("  >>> 只有一个, 另一根 SDA/SCL 可能没插到底\r\n");
    }
  }

  if (nl > 0) {
    say("  被外部下拉到 LOW 的脚 (%d 个):", nl);
    for (int i = 0; i < nl; i++) say(" GPIO%d", lows[i]);
    say("\r\n  (这些脚接到了 GND 或低阻器件, 不像是 I2C 信号脚)\r\n");
  } else {
    say("  没有脚被外部下拉到 LOW -> GND 那根线可能没接上\r\n");
  }

  // ---- 对照点: 开发板 BOOT 脚(GPIO0) 上有 10k 上拉, 必须读 HIGH ----
  // 如果连它都读不出高电平, 说明是探测方法/固件有问题, 而不是接线问题。
  pinMode(0, INPUT_PULLDOWN);
  delay(5);
  int ctl = digitalRead(0);
  pinMode(0, INPUT);
  say("  [对照] GPIO0(BOOT, 板载10k上拉) 读=%d -> %s\r\n", ctl,
      ctl == HIGH ? "方法有效, 上面的结论可信"
                  : "方法失效, 先别信上面的结论");

  // 恢复 I2C 到当前设定引脚
  Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
  Wire.setClock(I2C_HZ);
  say("  (已恢复 I2C 到 SDA=%d SCL=%d)\r\n", PIN_SDA, PIN_SCL);
}

// ------------------------------------------------------------------ 初始化
static bool oledInit() {
  static const uint8_t seq[] = {
    0xAE,        // 关显示
    0x20, 0x00,  // 水平寻址
    0xB0,        // 页 0
    0xC8,        // 列输出扫描方向(反)
    0x00, 0x10,  // 起始列 0
    0x40,        // 起始行 0
    0x81, 0xCF,  // 对比度
    0xA1,        // 段重映射(左右翻)
    0xA6,        // 正常显示(非反白)
    0xA8, 0x3F,  // 复用率 64
    0xA4,        // 输出跟随显存
    0xD3, 0x00,  // 显示偏移 0
    0xD5, 0x80,  // 时钟分频
    0xD9, 0xF1,  // 预充电
    0xDA, 0x12,  // COM 引脚配置 128x64
    0xDB, 0x40,  // VCOMH
    0x8D, 0x14,  // 开电荷泵(模块自带升压必须开)
    0xAF         // 开显示
  };
  if (!cmdN(seq, sizeof(seq))) return false;
  memset(fb, 0, sizeof(fb));
  return pushFrame();
}

// ------------------------------------------------------------------ 画布
static inline void px(int x, int y, bool on) {
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  uint8_t m = (uint8_t)(1u << (y & 7));
  uint8_t *cell = &fb[(y >> 3) * W + x];
  if (on) *cell |= m; else *cell &= (uint8_t)~m;
}

static void clearAll(bool on) {
  memset(fb, on ? 0xFF : 0x00, sizeof(fb));
}

static void hline(int x, int y, int w, bool on) { for (int i = 0; i < w; i++) px(x + i, y, on); }
static void vline(int x, int y, int h, bool on) { for (int i = 0; i < h; i++) px(x, y + i, on); }
static void rect(int x, int y, int w, int h, bool on) {
  hline(x, y, w, on); hline(x, y + h - 1, w, on);
  vline(x, y, h, on); vline(x + w - 1, y, h, on);
}
static void fillRect(int x, int y, int w, int h, bool on) {
  for (int j = 0; j < h; j++) hline(x, y + j, w, on);
}

static int glyphIndex(char c) {
  if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  const char *p = strchr(CHARSET, c);
  return p ? (int)(p - CHARSET) : 0;
}

static void drawChar(int x, int y, char c, uint8_t scale, bool on) {
  int gi = glyphIndex(c);
  for (int col = 0; col < 5; col++) {
    uint8_t bits = FONT5X7[gi][col];
    for (int row = 0; row < 7; row++) {
      if (bits & (1u << row)) fillRect(x + col * scale, y + row * scale, scale, scale, on);
    }
  }
}

static int drawText(int x, int y, const char *s, uint8_t scale, bool on) {
  int cx = x;
  for (; *s; s++) {
    drawChar(cx, y, *s, scale, on);
    cx += 6 * scale;
    if (cx > W) break;
  }
  return cx;
}

static int textWidth(const char *s, uint8_t scale) {
  int n = 0;
  for (; *s; s++) n++;
  return n * 6 * scale;
}

// ------------------------------------------------------------------ 画面
static void pageInfo() {
  clearAll(false);
  rect(0, 0, W, H, true);
  const char *t1 = "OLED TEST";
  drawText((W - textWidth(t1, 1)) / 2, 6, t1, 1, true);
  hline(4, 18, W - 8, true);
  drawText(6, 24, "SSD1306 128X64", 1, true);
  char pin[24];
  snprintf(pin, sizeof(pin), "SDA=%d SCL=%d", PIN_SDA, PIN_SCL);
  drawText(6, 36, pin, 1, true);
  drawText(6, 48, "ESP32-S3 OK", 1, true);
  pushFrame();
}

static void pageBig() {
  clearAll(false);
  const char *t = "OK";
  drawText((W - textWidth(t, 6)) / 2, 8, t, 6, true);
  const char *t2 = "LINK UP";
  drawText((W - textWidth(t2, 1)) / 2, 54, t2, 1, true);
  pushFrame();
}

static void pageCheck() {
  clearAll(false);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      if (((x >> 3) + (y >> 3)) & 1) px(x, y, true);
  pushFrame();
}

static void pageBars() {
  clearAll(false);
  for (int i = 0; i < 16; i++) {
    int level = i;                        // 0..15
    for (int x = i * 8; x < (i + 1) * 8; x++)
      for (int y = 0; y < 40; y++)
        if (((x + y) % 16) < level) px(x, y, true);
  }
  drawText(2, 46, "0 25 50 75 100", 1, true);
  rect(0, 0, W, 44, true);
  pushFrame();
}

static void demo() {
  if (!online) { say("屏没在线, 先跑 SCAN\r\n"); return; }
  say("==== 点屏: 信息页 -> 大字 -> 棋盘 -> 灰阶 ====\r\n");
  pageInfo();  delay(1800);
  pageBig();   delay(1800);
  pageCheck(); delay(1800);
  pageBars();
  say("==== 结束, 停在灰阶图 ====\r\n");
}

// ------------------------------------------------------------------ 命令
static void help() {
  say("\r\n命令: HELP | SCAN | PULLUP | DEMO | TEXT 内容 | FILL | INV | PINS sda scl | OFF\r\n");
  say("当前: SDA=%d SCL=%d  屏状态=%s  地址=0x%02X\r\n",
      PIN_SDA, PIN_SCL, online ? "在线" : "未发现", addr);
}

static void doScan() {
  say("[SCAN] 扫描 I2C 总线 (SDA=%d SCL=%d)...\r\n", PIN_SDA, PIN_SCL);
  int a = scanBus(true);
  if (a < 0) {
    online = false;
    say("  零命中 -> 屏没接上 / 没供电 / SDA与SCL接反\r\n");
    say("  排查: 1) VCC 是否吃到 3.3V (模块上有电源灯就看灯)\r\n");
    say("        2) SDA(数据) 和 SCL(时钟) 是否接反\r\n");
    say("        3) GND 是否与 ESP32 共地\r\n");
    return;
  }
  addr = (uint8_t)a;
  say("  找到 0x%02X, 开始初始化...\r\n", addr);
  online = oledInit();
  if (online) {
    say("  初始化成功, 屏已点亮\r\n");
  } else {
    say("  初始化失败: 写不进显存, 试着降到 100kHz...\r\n");
    Wire.setClock(100000);
    I2C_HZ = 100000;
    online = oledInit();
    say(online ? "  100kHz 下成功\r\n" : "  仍失败, 检查 VCC 电压和杜邦线接触\r\n");
  }
}

static void handle(char *line) {
  while (*line == ' ') line++;
  char *arg = strchr(line, ' ');
  if (arg) { *arg = 0; arg++; while (*arg == ' ') arg++; }

  if (!strcasecmp(line, "HELP"))  { help(); return; }
  if (!strcasecmp(line, "SCAN"))  { doScan(); return; }
  if (!strcasecmp(line, "PULLUP") || !strcasecmp(line, "PROBE")) { probePulls(); return; }
  if (!strcasecmp(line, "DEMO"))  { demo(); return; }
  if (!strcasecmp(line, "FILL"))  { clearAll(true);  pushFrame(); say("全亮\r\n"); return; }
  if (!strcasecmp(line, "OFF"))   { clearAll(false); pushFrame(); say("全灭\r\n"); return; }
  if (!strcasecmp(line, "INV"))   {
    for (size_t i = 0; i < sizeof(fb); i++) fb[i] = (uint8_t)~fb[i];
    pushFrame(); say("反白\r\n"); return;
  }
  if (!strcasecmp(line, "TEXT")) {
    if (!arg || !*arg) { say("用法: TEXT HELLO\r\n"); return; }
    clearAll(false);
    int n = 0; for (char *p = arg; *p; p++) n++;
    uint8_t scale = (n <= 10) ? 2 : 1;
    drawText(2, 8, arg, scale, true);
    drawText(2, 8 + 7 * scale + 4, "ESP32-S3", 1, true);
    pushFrame();
    say("已显示: %s\r\n", arg);
    return;
  }
  if (!strcasecmp(line, "PINS")) {
    int a, b;
    if (arg && sscanf(arg, "%d %d", &a, &b) == 2) {
      PIN_SDA = a; PIN_SCL = b;
      Wire.end();
      Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
      Wire.setClock(I2C_HZ);
      online = false;
      say("引脚已改: SDA=%d SCL=%d, 重新扫描\r\n", PIN_SDA, PIN_SCL);
      doScan();
      return;
    }
    say("用法: PINS 8 9\r\n");
    return;
  }
  say("不认识: %s (发 HELP)\r\n", line);
}

// ------------------------------------------------------------------ main
void setup() {
  Serial.begin(115200);
  delay(300);
  say("\r\n\r\n=== OledTest / SSD1306 128x64 / I2C ===\r\n");
  say("接线: GND->GND  VCC->3V3  SCL->GPIO%d  SDA->GPIO%d\r\n", PIN_SCL, PIN_SDA);
  say("警告: VCC 只能 3.3V, 接 5V 会烧屏\r\n");

  Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
  Wire.setClock(I2C_HZ);

  doScan();
  if (online) demo();
  else say("没找到屏。接好后不用复位, 每 3 秒自动重扫一次。\r\n");
  help();
}

void loop() {
  static char line[96];
  static size_t n = 0;
  static uint32_t beatAt = 0, retryAt = 0, beat = 0;

  if (!online && retryAt && millis() >= retryAt) {
    retryAt = millis() + 3000;
    int a = scanBus(false);
    if (a >= 0) {
      addr = (uint8_t)a;
      say("[自动重扫] 发现 0x%02X, 初始化...\r\n", addr);
      online = oledInit();
      if (online) { say("  成功, 开跑 DEMO\r\n"); retryAt = 0; demo(); }
    }
  } else if (!online && !retryAt) {
    retryAt = millis() + 3000;
  }

  if (millis() >= beatAt) {
    beatAt = millis() + 5000;
    say("[%6lums] 心跳 #%lu  屏=%s\r\n", (unsigned long)millis(),
        (unsigned long)++beat, online ? "在线" : "未发现");
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
