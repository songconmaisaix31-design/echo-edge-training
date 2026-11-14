/*
 * ppg.h - 光电容积脉搏波（PPG）：采样、峰值检测、脉率与信号质量
 *
 * 器件现状（issue #3 的前提）:
 *   本批只有 ITR9909 —— 反射式红外对管 + LM393 比较器，只引出 DO 数字量。
 *   它给的是"有没有反光"的二值量，采不到脉搏波形，做不了脉率。
 *   真实 PPG 需要类似 MAX30102 的器件（I²C，红光/红外 LED + 光电二极管 + 16/18 位 ADC）。
 *
 * 所以这一层分两段，边界划在"原始采样"上:
 *   PpgSource     产生 25 位采样（mock），三种接触状态可切
 *   PpgProcessor  峰值检测 + RR 间期 + 脉率 + 信号质量   ← 这段是真代码
 *
 * 接上真器件时只换 PpgSource，PpgProcessor 和它的输出格式一行都不用改。
 * 换句话说：这一轮验证的是算法和链路，不是"脉搏真实值"。
 *
 * 采样率 50 Hz。峰值检测用"自适应中点阈值 + 不应期"，这是 PPG 里最常用的做法，
 * 不依赖固定幅度 —— 手指压力、肤色、环境光都会让绝对幅度差好几倍，固定阈值一定失效。
 */
#pragma once
#include <Arduino.h>
#include <math.h>

static const int PPG_FS = 50;                    // 采样率
static const uint32_t PPG_PERIOD_MS = 1000 / PPG_FS;

// ---------------------------------------------------------------- 接触状态
enum PpgScene : uint8_t {
  PPG_GOOD   = 0,   // 正常接触
  PPG_MOTION = 1,   // 移动 / 抖动
  PPG_POOR   = 2,   // 接触不良（幅度极低）
};

static const char *ppgSceneName(uint8_t s) {
  switch (s) {
    case PPG_GOOD:   return "GOOD";
    case PPG_MOTION: return "MOTION";
    case PPG_POOR:   return "POOR";
  }
  return "?";
}

// ---------------------------------------------------------------- 波形源（mock）
class PpgSource {
 public:
  uint8_t scene = PPG_GOOD;
  float hrTrue = 96.0f;       // 真实心率，用于对照算法估出来的值
  bool enabled = false;

  void reset(uint32_t now) {
    lastBeatMs = now;
    nextBeatMs = now;
    seed = 0x9E3779B9;
  }

  // 返回 0..4095（模拟 12 位 ADC 的原始计数）
  float sample(uint32_t ms) {
    // 呼吸性窦性心律不齐：吸气快、呼气慢，幅度约 ±3 bpm
    float hrNow = hrTrue + 2.5f * sinf((float)ms / 9000.0f);
    float period = 60000.0f / hrNow;

    if ((int32_t)(ms - nextBeatMs) >= 0) {
      lastBeatMs = ms;
      nextBeatMs = ms + (uint32_t)period;
    }

    float ph = (float)(ms - lastBeatMs) / period;   // 0..1 心搏相位
    if (ph < 0) ph = 0;

    // 收缩期主峰 + 舒张期重搏波，两个高斯叠出来的形状接近真实 PPG
    float sys = expf(-powf((ph - 0.18f) / 0.070f, 2.0f));
    float dic = 0.32f * expf(-powf((ph - 0.45f) / 0.100f, 2.0f));

    float amp = 520.0f;
    float noiseAmp = 6.0f;
    float artifact = 0.0f;

    switch (scene) {
      case PPG_POOR:
        amp = 60.0f;                    // 幅度掉到 1/8，埋进噪声
        noiseAmp = 55.0f;
        break;
      case PPG_MOTION:
        // 1.1 Hz 大幅低频漂移 + 更大噪声，模拟探头被蹭动
        artifact = 300.0f * sinf(6.2832f * 1.1f * (float)ms / 1000.0f)
                 + 180.0f * sinf(6.2832f * 3.7f * (float)ms / 1000.0f);
        noiseAmp = 40.0f;
        break;
      default:
        break;
    }

    float v = 2400.0f + artifact + amp * (sys + dic) + noise(noiseAmp);
    if (v < 0) v = 0;
    if (v > 4095) v = 4095;
    return v;
  }

 private:
  uint32_t lastBeatMs = 0;
  uint32_t nextBeatMs = 0;
  uint32_t seed = 0x9E3779B9;

  float noise(float amp) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return ((float)(seed & 0xFFFF) / 65535.0f - 0.5f) * 2.0f * amp;
  }
};

// ---------------------------------------------------------------- 峰值检测 + 脉率
class PpgProcessor {
 public:
  static const int RR_N = 8;

  float bpm = 0.0f;
  int quality = 0;            // 0..100
  bool valid = false;
  uint32_t beatCount = 0;
  uint32_t lastBeatMs = 0;
  float ppAmp = 0.0f;         // 峰峰值，判断接触好坏

  void reset() {
    dcInit = false;
    dc = 0; prevAc = 0;
    envMax = envMin = 0;
    ltAmp = 0;
    rrCnt = 0; rrIdx = 0;
    bpm = 0; quality = 0; valid = false;
    beatCount = 0; lastBeatMs = 0; ppAmp = 0;
    beatFlag = false;
  }

  // 每来一个采样调用一次，必须严格 20ms 一次（fs=50）
  void push(float raw, uint32_t ms) {
    if (!dcInit) { dc = raw; envMax = raw - dc; envMin = raw - dc; dcInit = true; }
    // DC 跟踪（高通）：alpha 对应约 0.5 Hz 截止，够滤掉呼吸和缓慢的接触压力变化
    dc += 0.02f * (raw - dc);
    float ac = raw - dc;

    // 连续包络：峰值缓慢衰减，而不是每 2 秒硬清零。
    // 硬清零会让 ppAmp 在窗口刚重置时接近 0，质量分随机掉到 0 —— 这是假象，
    // 会让人误以为"信号忽好忽坏"。（已踩过）
    envMax = (ac > envMax) ? ac : envMax * 0.995f;
    envMin = (ac < envMin) ? ac : envMin * 0.995f;
    ppAmp = envMax - envMin;

    // 长期幅度（很慢衰减）。用来判断"信号是不是突然变小了" → 接触失效。
    // 这一步不能省：接触不良时幅度只剩 1/8，绝对阈值挡不住噪声引起的误触发，
    // 会稳定地数出 2 倍脉率（收缩峰和重搏波各算一次）。
    if (ppAmp > ltAmp) ltAmp = ppAmp;
    else               ltAmp *= 0.998f;
    if (ltAmp < 100.0f) ltAmp = 100.0f;

    float thr = (envMax + envMin) * 0.5f;    // 中点阈值
    bool rising = (prevAc < thr) && (ac >= thr);
    prevAc = ac;

    // 自适应不应期：至少 300ms（上限 200bpm）；有 3 个 RR 之后收紧到中位数的 0.6 倍，
    // 防止把重搏波当成一次独立搏动
    uint32_t refractory = 300;
    if (rrCnt >= 3) {
      float med = medianRR();
      if (med > 0) {
        uint32_t r = (uint32_t)(med * 0.6f);
        if (r > refractory) refractory = r;
      }
    }

    bool ampOk = ppAmp >= 0.45f * ltAmp;     // 相对幅度门

    if (rising && ampOk && (ms - lastBeatMs) >= refractory) {
      if (lastBeatMs) {
        uint32_t rr = ms - lastBeatMs;
        if (rr >= 300 && rr <= 2000) {
          rrBuf[rrIdx] = (float)rr;
          rrIdx = (rrIdx + 1) % RR_N;
          if (rrCnt < RR_N) rrCnt++;
        }
      }
      lastBeatMs = ms;
      beatCount++;
      beatFlag = true;
    }

    // 2.5 秒没搏动 -> 判为无效（接触丢失）。bpm 保留最后值，但要靠 valid 字段说明它过期了
    if (lastBeatMs == 0 || (ms - lastBeatMs) > 2500) {
      valid = false;
      quality = 0;
      return;
    }

    quality = computeQuality();

    // "可用"不等于"有搏动"：质量太差时 bpm 仍在跳，但不能当读数用。
    // 屏上因此显示 ---bpm + q 值，这就是 issue #3 要的"低质量时显示信号状态"。
    if (rrCnt >= 3 && quality >= 40) {
      bpm = 60000.0f / medianRR();
      valid = true;
    } else {
      valid = false;
    }
  }

  // 屏上的节拍图标用：读一次就清
  bool takeBeat() {
    bool b = beatFlag;
    beatFlag = false;
    return b;
  }

 private:
  bool dcInit = false;
  bool beatFlag = false;
  float dc = 0, prevAc = 0;
  float envMax = 0, envMin = 0;
  float ltAmp = 0;            // 长期幅度，缓降
  float rrBuf[RR_N] = {0};
  int rrCnt = 0, rrIdx = 0;

  float meanRR() const {
    if (!rrCnt) return 0;
    float s = 0;
    for (int i = 0; i < rrCnt; i++) s += rrBuf[i];
    return s / rrCnt;
  }

  // 中位数比均值抗单个漏检/多检，PPG 里几乎必用
  float medianRR() const {
    if (!rrCnt) return 0;
    float t[RR_N];
    for (int i = 0; i < rrCnt; i++) t[i] = rrBuf[i];
    for (int i = 1; i < rrCnt; i++) {          // 插入排序，n<=8
      float k = t[i];
      int j = i - 1;
      while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
      t[j + 1] = k;
    }
    return t[rrCnt / 2];
  }

  int computeQuality() {
    if (rrCnt < 3) return 0;
    float m = meanRR();
    if (m <= 0) return 0;
    float var = 0;
    for (int i = 0; i < rrCnt; i++) {
      float d = rrBuf[i] - m;
      var += d * d;
    }
    var /= rrCnt;
    float cv = sqrtf(var) / m;                 // 变异系数

    int qr = (int)(100.0f * (1.0f - cv / 0.25f));   // cv 0 -> 100，cv 0.25 -> 0
    int qa = (int)(100.0f * ppAmp / 350.0f);        // 峰峰 350 计数算满
    int q = (qr < qa) ? qr : qa;
    if (q < 0) q = 0;
    if (q > 100) q = 100;
    return q;
  }
};
