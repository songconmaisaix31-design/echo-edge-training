/*
 * mock_sources.h - 数据源级 mock（无实物器件的链路验证）
 *
 * 为什么放在"数据源"而不是"显示层":
 *   如果直接往屏幕上塞假读数，验证的是 printf，不是链路。这里 mock 出来的是
 *   "器件本该给出的原始量"，后面所有环节（量程换算、有效性判定、屏刷新、串口
 *   帧、电脑侧解析）全部走真实代码路径。换回真器件时只需摘掉这个头文件。
 *
 * 三条硬规则:
 *   1. 器件优先。真实器件一旦应答，mock 立刻让位，绝不覆盖真实读数。
 *   2. 每个值都带 MOCK 位。屏上打 "M" 角标、串口行尾标 [MOCK]、CSV 第 9 列置位。
 *      任何一个消费方都能分辨"这条数据是编的"。
 *   3. mock 只造采样，不造结论。凡是有算法的量（脉率、质量分），一律由正式
 *      算法从 mock 波形里算出来，算法本身的正确性可以被这轮验证覆盖到。
 *
 * 随机源用 xorshift32 固定种子，同一上电时序可复现，便于对比两次运行。
 */
#pragma once
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <strings.h>

// ---- mock 源位 ----
#define MOCK_TEMP   0x01
#define MOCK_RH     0x02
#define MOCK_PRES   0x04
#define MOCK_PPG    0x08
#define MOCK_VIB    0x10
#define MOCK_SERVO  0x20
#define MOCK_ENV    (MOCK_TEMP | MOCK_RH | MOCK_PRES)
// MOCK_ALL 只包含"本固件已经实现的源"。每补一个源就把它加进来，
// 避免 MOCK ON 打开一堆还没实现的位、在 STAT 里看着像已经支持。
#define MOCK_ALL    (MOCK_ENV | MOCK_PPG | MOCK_VIB)

class Mock {
 public:
  uint8_t mask = 0;

  // 场景基线，可用 MOCK BASE 命令改
  float tempBase = 24.0f;
  float rhBase   = 48.0f;
  float presBase = 1013.2f;

  bool on(uint8_t bit) const { return (mask & bit) != 0; }
  void enable(uint8_t bits)  { mask |= bits; }
  void disable(uint8_t bits) { mask &= (uint8_t)~bits; }
  void clear() { mask = 0; }

  // 按名字开关。返回改动的位，0 表示没认出来。
  uint8_t byName(const char *n, bool en) {
    uint8_t bit = 0;
    if      (!strcasecmp(n, "TEMP") || !strcasecmp(n, "T"))  bit = MOCK_TEMP;
    else if (!strcasecmp(n, "RH")   || !strcasecmp(n, "H"))  bit = MOCK_RH;
    else if (!strcasecmp(n, "PRES") || !strcasecmp(n, "P"))  bit = MOCK_PRES;
    else if (!strcasecmp(n, "PPG")  || !strcasecmp(n, "HR")) bit = MOCK_PPG;
    else if (!strcasecmp(n, "VIB"))                          bit = MOCK_VIB;
    else if (!strcasecmp(n, "SERVO"))                        bit = MOCK_SERVO;
    else if (!strcasecmp(n, "ENV"))                          bit = MOCK_ENV;
    else if (!strcasecmp(n, "ALL"))                          bit = MOCK_ALL;
    if (!bit) return 0;
    if (en) enable(bit); else disable(bit);
    return bit;
  }

  // 位以 "TEMP|RH|PRES" 形式打印，供 STAT / 屏角标使用
  const char *describe(char *buf, size_t n) const {
    buf[0] = 0;
    if (!mask) { snprintf(buf, n, "-"); return buf; }
    if (mask & MOCK_TEMP)  strncat(buf, "TEMP ", n - strlen(buf) - 1);
    if (mask & MOCK_RH)    strncat(buf, "RH ",   n - strlen(buf) - 1);
    if (mask & MOCK_PRES)  strncat(buf, "PRES ", n - strlen(buf) - 1);
    if (mask & MOCK_PPG)   strncat(buf, "PPG ",  n - strlen(buf) - 1);
    if (mask & MOCK_VIB)   strncat(buf, "VIB ",  n - strlen(buf) - 1);
    if (mask & MOCK_SERVO) strncat(buf, "SERVO ",n - strlen(buf) - 1);
    size_t l = strlen(buf);
    if (l) buf[l - 1] = 0;
    return buf;
  }

  // ---- 环境量生成 ----
  // 叠加三个慢变分量 + 噪声：慢漂移模拟室内空调循环，短周期分量防止曲线
  // 看起来像纯正弦（纯正弦一眼就能认出是编的，反而看不出链路是否在工作）
  float tempC() {
    float t = millis() / 1000.0f;
    return tempBase + 0.6f * sinf(t / 14.3f) + 0.15f * sinf(t / 3.1f) + noise(0.05f);
  }
  float rhPct() {
    float t = millis() / 1000.0f;
    float v = rhBase - 3.5f * sinf(t / 14.3f) + 0.8f * sinf(t / 5.7f) + noise(0.2f);
    return constrain(v, 0.0f, 100.0f);
  }
  float pressHpa() {
    float t = millis() / 1000.0f;
    return presBase + 1.2f * sinf(t / 47.0f) + noise(0.03f);
  }

  // xorshift32，固定种子
  float noise(float amp) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return ((float)(seed & 0xFFFF) / 65535.0f - 0.5f) * 2.0f * amp;
  }

 private:
  uint32_t seed = 0x2545F491;
};
