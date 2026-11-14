/*
 * oled_ssd1306.h - SSD1306 128x64 I2C 最小驱动 (无动态分配 / 无第三方库)
 *
 * 只为 5x7 等宽字体 + 直线/矩形服务, 够画状态页就行。
 * 帧缓冲 1KB 静态常驻, 每次 show() 全屏推 8 页, 400kHz 下约 25ms。
 *
 * 关键: 控制字节 0x00 = 后面是命令, 0x40 = 后面是显存数据。
 *       弄错了会把命令当数据写进显存, 屏不亮。
 */
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <string.h>

#define OLED_W     128
#define OLED_H     64
#define OLED_PAGES (OLED_H / 8)

// 5x7 字库: 每字符 5 字节, 每字节一列, bit0 = 该列最上面的像素
// '%' 排在最后: 插在中间要同步改前面所有下标, 得不偿失
static const char OLED_CHARSET[] = " -./:0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ%";
static const uint8_t OLED_FONT[][5] = {
  {0x00,0x00,0x00,0x00,0x00}, // ' '
  {0x08,0x08,0x08,0x08,0x08}, // '-'
  {0x00,0x60,0x60,0x00,0x00}, // '.'
  {0x40,0x30,0x08,0x06,0x01}, // '/'
  {0x00,0x36,0x36,0x00,0x00}, // ':'
  {0x3E,0x51,0x49,0x45,0x3E}, // 0
  {0x00,0x42,0x7F,0x40,0x00}, // 1
  {0x42,0x61,0x51,0x49,0x46}, // 2
  {0x40,0x49,0x49,0x49,0x37}, // 3
  {0x18,0x14,0x12,0x7F,0x10}, // 4
  {0x27,0x45,0x45,0x45,0x39}, // 5
  {0x3C,0x4A,0x49,0x49,0x30}, // 6
  {0x01,0x71,0x09,0x05,0x03}, // 7
  {0x36,0x49,0x49,0x49,0x36}, // 8
  {0x06,0x49,0x49,0x29,0x1E}, // 9
  {0x7C,0x12,0x11,0x12,0x7C}, // A
  {0x7F,0x49,0x49,0x49,0x36}, // B
  {0x3E,0x41,0x41,0x41,0x41}, // C
  {0x7F,0x41,0x41,0x22,0x1C}, // D
  {0x7F,0x49,0x49,0x49,0x41}, // E
  {0x7F,0x09,0x09,0x09,0x01}, // F
  {0x3E,0x41,0x49,0x49,0x3A}, // G
  {0x7F,0x08,0x08,0x08,0x7F}, // H
  {0x00,0x41,0x7F,0x41,0x00}, // I
  {0x20,0x40,0x41,0x3F,0x01}, // J
  {0x7F,0x08,0x14,0x22,0x41}, // K
  {0x7F,0x40,0x40,0x40,0x40}, // L
  {0x7F,0x02,0x04,0x02,0x7F}, // M
  {0x7F,0x02,0x04,0x08,0x7F}, // N
  {0x3E,0x41,0x41,0x41,0x3E}, // O
  {0x7F,0x09,0x09,0x09,0x06}, // P
  {0x3E,0x41,0x51,0x21,0x5E}, // Q
  {0x7F,0x09,0x19,0x29,0x46}, // R
  {0x46,0x49,0x49,0x49,0x31}, // S
  {0x01,0x01,0x7F,0x01,0x01}, // T
  {0x3F,0x40,0x40,0x40,0x3F}, // U
  {0x1F,0x20,0x40,0x20,0x1F}, // V
  {0x7F,0x20,0x18,0x20,0x7F}, // W
  {0x63,0x14,0x08,0x14,0x63}, // X
  {0x03,0x04,0x78,0x04,0x03}, // Y
  {0x61,0x51,0x49,0x45,0x43}, // Z
  {0x23,0x13,0x08,0x64,0x62}, // %  左上左下两点 + 斜杠 + 右上右下两点
};

class Oled {
public:
  uint8_t fb[OLED_W * OLED_PAGES];
  uint8_t addr = 0x3C;
  bool online = false;

  // 扫总线, 返回命中的 OLED 地址(优先 0x3C/0x3D), 没有返回 -1
  int scan(uint8_t sda, uint8_t scl, uint32_t hz = 400000) {
    Wire.end();
    delay(2);
    Wire.begin(sda, scl, hz);
    Wire.setClock(hz);
    delay(2);
    int found = -1;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        if (a == 0x3C || a == 0x3D) return a;
        if (found < 0) found = a;
      }
    }
    return found;
  }

  bool begin(uint8_t sda, uint8_t scl, uint32_t hz = 400000) {
    int a = scan(sda, scl, hz);
    if (a < 0) { online = false; return false; }
    addr = (uint8_t)a;
    online = initSeq();
    if (!online) {                       // 高频不通就降到 100kHz
      Wire.setClock(100000);
      online = initSeq();
    }
    return online;
  }

  bool cmd1(uint8_t c) {
    Wire.beginTransmission(addr);
    Wire.write((uint8_t)0x00);
    Wire.write(c);
    return Wire.endTransmission() == 0;
  }

  bool initSeq() {
    static const uint8_t seq[] = {
      0xAE, 0x20, 0x00, 0xB0, 0xC8, 0x00, 0x10, 0x40,
      0x81, 0xCF, 0xA1, 0xA6, 0xA8, 0x3F, 0xA4, 0xD3, 0x00,
      0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB, 0x40,
      0x8D, 0x14, 0xAF
    };
    Wire.beginTransmission(addr);
    Wire.write((uint8_t)0x00);
    for (size_t i = 0; i < sizeof(seq); i++) Wire.write(seq[i]);
    if (Wire.endTransmission() != 0) return false;
    memset(fb, 0, sizeof(fb));
    return show();
  }

  bool show() {
    for (int page = 0; page < OLED_PAGES; page++) {
      if (!cmd1((uint8_t)(0xB0 | page))) return false;
      if (!cmd1(0x00)) return false;
      if (!cmd1(0x10)) return false;
      const uint8_t *p = fb + page * OLED_W;
      for (int i = 0; i < OLED_W; i += 16) {
        Wire.beginTransmission(addr);
        Wire.write((uint8_t)0x40);
        for (int k = 0; k < 16; k++) Wire.write(p[i + k]);
        if (Wire.endTransmission() != 0) return false;
      }
    }
    return true;
  }

  void clear() { memset(fb, 0, sizeof(fb)); }

  void px(int x, int y) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    fb[(y >> 3) * OLED_W + x] |= (uint8_t)(1u << (y & 7));
  }
  void hline(int x, int y, int w) { for (int i = 0; i < w; i++) px(x + i, y); }

  void glyph(int x, int y, char c, uint8_t scale) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    const char *p = strchr(OLED_CHARSET, c);
    int gi = p ? (int)(p - OLED_CHARSET) : 0;
    for (int col = 0; col < 5; col++) {
      uint8_t bits = OLED_FONT[gi][col];
      for (int row = 0; row < 7; row++) {
        if (!(bits & (1u << row))) continue;
        for (int dy = 0; dy < scale; dy++)
          for (int dx = 0; dx < scale; dx++) px(x + col * scale + dx, y + row * scale + dy);
      }
    }
  }

  void text(int x, int y, const char *s, uint8_t scale = 1) {
    int cx = x;
    for (; *s; s++) {
      glyph(cx, y, *s, scale);
      cx += 6 * scale;
      if (cx > OLED_W) break;
    }
  }
};
