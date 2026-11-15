/*
 * anim.h - 屏上那只会跳的心脏
 *
 * 全部用程序化图元画, 不用位图数组。理由有三个:
 *   1) 42x42 的一帧就是 220 字节, 五个状态各存三四帧要 3KB flash, 而程序化
 *      只要几百字节代码;
 *   2) 搏动靠改变半径 ±1px 就能做出像素画特有的"跳格"手感, 位图做不到这种
 *      离散变化;
 *   3) 以后换形状/换表情改几行就行, 不用回工具里重导素材。
 *
 * 坐标系相对精灵框左上角, 框 42x42。调用方给原点和相位。
 * 这里直接写帧缓冲而不是走 oled.px: 画眼睛、腮红、嘴角需要"擦"像素。
 */
#pragma once
#include <math.h>

#include "oled_ssd1306.h"

#define SPR_W 42
#define SPR_H 42

enum SpriteMode {
  SP_HEART,   // 四腔心: 一只会跳的心
  SP_RING,    // 左室短轴: 会收缩的圆环
  SP_SLEEP,   // 待分析: 睡着的心, 冒 Z
  SP_BUSY,    // 分析中: 小的心 + 绕圈的点
  SP_BROKEN,  // 断线: 裂开的心
};

// 图元函数都挂在当前设备上, 免得每个内部调用都多传一个参数
static Oled *spDev = nullptr;

static void spPx(int x, int y, bool on) {
  if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
  uint8_t *p = &spDev->fb[(y >> 3) * OLED_W + x];
  if (on) *p |= (uint8_t)(1 << (y & 7));
  else *p &= (uint8_t)~(1 << (y & 7));
}

static void spRect(int x, int y, int w, int h, bool on) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) spPx(x + i, y + j, on);
}

// 实心圆; r<=0 什么都不画
static void spDisc(int cx, int cy, float r, bool on) {
  if (r <= 0) return;
  int ri = (int)(r + 0.5f);
  for (int dy = -ri; dy <= ri; dy++) {
    float t = r * r - (float)dy * dy;
    if (t < 0) continue;
    int w = (int)(sqrtf(t) + 0.5f);
    for (int dx = -w; dx <= w; dx++) spPx(cx + dx, cy + dy, on);
  }
}

// 心形: 两个圆瓣 + 一个三角尖。k 是搏动档位(-1/0/+1), 直接改半径 ——
// 1px 一档的跳动比浮点缩放更像像素画。
static void heartShape(int k, bool on, int dx, int dy) {
  float r = 9.0f + (float)k;
  spDisc(14 + dx, 15 + dy, r, on);
  spDisc(27 + dx, 15 + dy, r, on);
  for (int y = 15; y <= 36; y++) {
    int t = y - 15;
    int half = 11 - (t * 11) / 21;
    int x0 = (y == 15) ? 9 : 20 - half;
    int w = (y == 15) ? 23 : half * 2 + 1;
    spRect(x0 + dx, y + dy, w, 1, on);
  }
}

static void faceOpen(bool lowConf, int dx, int dy) {
  spRect(15 + dx, 13 + dy, 3, 3, false);
  spRect(25 + dx, 13 + dy, 3, 3, false);
  spPx(16 + dx, 14 + dy, true);
  spPx(26 + dx, 14 + dy, true);
  spRect(10 + dx, 19 + dy, 2, 1, false);          // 腮红
  spRect(29 + dx, 19 + dy, 2, 1, false);
  if (lowConf) {                                   // 不确定的时候嘴角朝下
    spPx(19 + dx, 25 + dy, false);
    spPx(20 + dx, 24 + dy, false);
    spPx(21 + dx, 24 + dy, false);
    spPx(22 + dx, 25 + dy, false);
  } else {
    spPx(19 + dx, 24 + dy, false);
    spPx(20 + dx, 25 + dy, false);
    spPx(21 + dx, 25 + dy, false);
    spPx(22 + dx, 24 + dy, false);
  }
}

static void faceClosed(int dx, int dy) {
  spRect(15 + dx, 15 + dy, 4, 1, false);
  spRect(24 + dx, 15 + dy, 4, 1, false);
  spRect(10 + dx, 19 + dy, 2, 1, false);
  spRect(29 + dx, 19 + dy, 2, 1, false);
  spRect(19 + dx, 24 + dy, 3, 1, false);
}

static void sparkle(int x, int y) {
  spPx(x, y - 1, true);
  spPx(x, y + 1, true);
  spPx(x - 1, y, true);
  spPx(x + 1, y, true);
  spPx(x, y, true);
}

// phase: 0..7, 一个心动周期。低置信度时整体左右晃 1px, 像拿不定主意。
void animDraw(Oled &dev, SpriteMode mode, uint8_t phase, int ox, int oy, bool lowConf) {
  spDev = &dev;

  if (lowConf && mode != SP_BROKEN) {
    ox += (phase == 2 || phase == 3) ? 1 : ((phase == 6 || phase == 7) ? -1 : 0);
  }

  switch (mode) {
    case SP_HEART: {
      // 收缩快、舒张慢, 所以档位不是均匀的
      static const int8_t k[8] = {1, 1, 0, 0, -1, 0, 0, 0};
      heartShape(k[phase], true, ox, oy);
      if (phase == 3) faceClosed(ox, oy);          // 眨一下眼
      else faceOpen(lowConf, ox, oy);
      if (phase == 0 || phase == 1) sparkle(36 + ox, 8 + oy);
      break;
    }

    case SP_RING: {
      // 环壁厚度跟着相位走: 内径变小 = 心肌收缩
      static const int8_t in[8] = {10, 9, 8, 9, 10, 11, 11, 10};
      spDisc(20 + ox, 21 + oy, 19, true);
      spDisc(20 + ox, 21 + oy, (float)in[phase], false);
      spRect(16 + ox, 19 + oy, 2, 2, true);        // 腔里的两只眼睛
      spRect(23 + ox, 19 + oy, 2, 2, true);
      if (phase == 5) {                            // 眨眼
        spRect(16 + ox, 19 + oy, 2, 2, false);
        spRect(23 + ox, 19 + oy, 2, 2, false);
        spRect(15 + ox, 20 + oy, 4, 1, true);
        spRect(22 + ox, 20 + oy, 4, 1, true);
      }
      spRect(19 + ox, 25 + oy, 3, 1, true);        // 小嘴
      if (phase == 0) sparkle(38 + ox, 6 + oy);
      break;
    }

    case SP_SLEEP: {
      heartShape(0, true, ox, oy);
      faceClosed(ox, oy);
      // 冒 Z: 相位越大飘得越高
      char z[2] = {'Z', 0};
      int lift = phase & 3;
      dev.text(31 + ox, 3 + oy - lift, z, 1);
      if ((phase & 1) == 0) dev.text(36 + ox, 10 + oy - lift, z, 1);
      break;
    }

    case SP_BUSY: {
      // 心收小放在中间, 外面绕一圈彗尾点。半径必须大于心形的外接半径,
      // 否则点会落在白色心形上 —— 白底白点等于没画。
      heartShape(-5, true, ox, oy);
      static const int8_t cx[8] = {0, 13, 18, 13, 0, -13, -18, -13};
      static const int8_t cy[8] = {-18, -13, 0, 13, 18, 13, 0, -13};
      for (int i = 2; i >= 0; i--) {
        int j = (phase - i) & 7;
        if (i == 0) spRect(20 + cx[j] - 1 + ox, 21 + cy[j] - 1 + oy, 2, 2, true);
        else spPx(20 + cx[j] + ox, 21 + cy[j] + oy, true);
      }
      break;
    }

    case SP_BROKEN: {
      // 断线时整屏就这一张图, 八个相位画出同一张的话看着像死机 —— 现场没法
      // 分清"网断了"和"板子挂了"。所以让裂口慢慢张合, 并偶尔闭一下眼。
      int8_t beat = (int8_t)((phase < 3) ? 0 : -1);
      heartShape(beat, true, ox, oy);
      // 中间劈一道锯齿缝, 眼睛挖成 X
      int gape = ((phase & 4) != 0) ? 1 : 0;      // 后四个相位裂得更开
      for (int y = 8; y <= 36; y++) {
        int x = 20 + ((y / 3) & 1 ? 1 : -1);
        spPx(x + ox, y + oy, false);
        if (y > 12 && y < 32) spPx(x + (y & 1 ? 1 : -1) + ox, y + oy, false);
        if (gape) spPx(x + (y & 1 ? 2 : -2) + ox, y + oy, false);
      }
      bool blink = (phase == 5);                   // 闭一眼, 像"快没气了"
      spRect(15 + ox, 13 + oy, 3, 3, false);
      spRect(25 + ox, 13 + oy, 3, 3, false);
      if (blink) {
        spRect(15 + ox, 14 + oy, 3, 1, true);
        spRect(25 + ox, 14 + oy, 3, 1, true);
      } else {
        spPx(15 + ox, 13 + oy, true); spPx(17 + ox, 15 + oy, true);
        spPx(17 + ox, 13 + oy, true); spPx(15 + ox, 15 + oy, true);
        // 右眼框是 25..27, 写成 26..27 会挤成一个 2x2 实心块, 两只眼不对称
        spPx(25 + ox, 13 + oy, true); spPx(27 + ox, 15 + oy, true);
        spPx(27 + ox, 13 + oy, true); spPx(25 + ox, 15 + oy, true);
      }
      break;
    }
  }
}
