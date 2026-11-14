/*
 * env_sensors.h - 环境传感器层（GXHT30C 温湿度 + BMP388 气压）
 *
 * 设计约束（对应 issue #2 验收标准第 4 条）:
 *   - 三项读数各自独立的有效性标志, 任一传感器掉线只影响自己那一项
 *   - 掉线的传感器每 2 秒自动重新探测, 接回来自动恢复, 不需要复位
 *   - 所有 I2C 传输带超时, 不会因为器件把 SDA 拉死而卡住主循环
 *
 * 两个器件的来历:
 *   GXHT30C  0x44  厂商驱动 modules/temperature-humidity/drivers/common/gxht30c.c
 *   BMP388   0x76  厂商驱动 modules/barometer/drivers/common/bmp388.c
 *
 * 注意: 气压这颗不是 Bosch 的 BMP388。厂商注释原文 "TOKMAS BMP388, NOT Bosch BMP388",
 *       CHIP_ID 读 0x0D 期望 0x11、PWR_CTRL 写 0x08 —— 都不是 Bosch 的寄存器映射。
 *       而且 module.json 里 tested_on_hardware_this_delivery = false, 即厂商没在实物上跑过。
 *       所以下面这套初始化序列是"照抄 + 留存疑", 现场用 REGS 命令 dump 寄存器判定。
 */
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------- CRC-8
// GXHT30C: poly 0x31, init 0xFF, 不反射, 结果不异或
static uint8_t crc8_sensirion(const uint8_t *data, size_t len) {
  uint8_t crc = 0xFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// ---------------------------------------------------------------- GXHT30C
class Gxht30c {
 public:
  static const uint8_t ADDR = 0x44;

  bool present = false;      // 最近一次探测结果
  uint8_t lastErr = 0;       // 0=ok 1=写失败 2=读不到 3=CRC错 4=NACK
  uint32_t okCount = 0, errCount = 0;

  // 只发一个空写, 看有没有 ACK。不会真的写寄存器。
  bool probe() {
    Wire.beginTransmission(ADDR);
    present = (Wire.endTransmission() == 0);
    return present;
  }

  // 单次测量: 0x2400 = 高重复性, 关闭 clock stretching。转换约 15ms。
  bool read(float &tempC, float &rhPct) {
    lastErr = 0;

    Wire.beginTransmission(ADDR);
    Wire.write((uint8_t)0x24);
    Wire.write((uint8_t)0x00);
    if (Wire.endTransmission() != 0) { lastErr = 1; errCount++; present = false; return false; }

    delay(20);   // datasheet 最长 15.5ms, 留余量

    uint8_t n = Wire.requestFrom((int)ADDR, 6);
    if (n != 6) { lastErr = 2; errCount++; present = false; return false; }

    uint8_t d[6];
    for (uint8_t i = 0; i < 6; i++) d[i] = (uint8_t)Wire.read();

    // 温度在 d[0..2], 湿度在 d[3..5], 每个字后面跟一个 CRC
    if (crc8_sensirion(d, 2) != d[2] || crc8_sensirion(d + 3, 2) != d[5]) {
      lastErr = 3; errCount++; return false;
    }

    uint16_t rawT = (uint16_t)((d[0] << 8) | d[1]);
    uint16_t rawH = (uint16_t)((d[3] << 8) | d[4]);

    // 量程 −45~130 ℃、0~100 %RH, 16 位线性
    tempC = -45.0f + 175.0f * (float)rawT / 65535.0f;
    rhPct = 100.0f * (float)rawH / 65535.0f;

    present = true;
    okCount++;
    return true;
  }
};

// ---------------------------------------------------------------- BMP388 (TOKMAS)
class Bmp388Tokmas {
 public:
  static const uint8_t ADDR = 0x76;
  static const uint8_t CHIP_ID = 0x11;   // 厂商定义, 从 0x0D 读

  bool present = false;
  bool calibrated = false;
  uint8_t chipId = 0;
  uint8_t lastErr = 0;
  uint32_t okCount = 0, errCount = 0;

  int32_t c0 = 0, c1 = 0, c00 = 0, c10 = 0, c01 = 0, c11 = 0;
  int32_t c20 = 0, c21 = 0, c30 = 0, c31 = 0, c40 = 0;

  bool probe() {
    uint8_t id = 0;
    if (!rd(0x0D, &id, 1)) { present = false; return false; }
    chipId = id;
    present = (id == CHIP_ID);
    return present;
  }

  bool begin() {
    if (!probe()) return false;

    wr(0x0C, 0x09);          // 厂商序列: 软复位
    delay(20);
    if (!waitStatus(0xC0)) { lastErr = 1; return false; }

    uint8_t b[21];
    if (!rd(0x10, b, 21)) { lastErr = 2; return false; }

    c0  = sx(((uint32_t)b[0] << 4) | (b[1] >> 4), 12);
    c1  = sx(((uint32_t)(b[1] & 0x0F) << 8) | b[2], 12);
    c00 = sx(((uint32_t)b[3] << 12) | ((uint32_t)b[4] << 4) | (b[5] >> 4), 20);
    c10 = sx(((uint32_t)(b[5] & 0x0F) << 16) | ((uint32_t)b[6] << 8) | b[7], 20);
    c01 = be16(b + 8);
    c11 = be16(b + 10);
    c20 = be16(b + 12);
    c21 = be16(b + 14);
    c30 = be16(b + 16);
    c31 = sx(((uint32_t)b[18] << 4) | (b[19] >> 4), 12);
    c40 = sx(((uint32_t)(b[19] & 0x0F) << 8) | b[20], 12);

    // 厂商序列: OSR/IIR=0, ODR=0, CONFIG=0, PWR=7
    // 存疑: 标准 Bosch BMP388 的 PWR_CTRL 在 0x1B, 0x08 是只读 STATUS。
    //       若实测读不到数据, 用 REGS 命令看 0x1B 是否为 0 再决定。
    wr(0x06, 0x00);
    wr(0x07, 0x00);
    wr(0x09, 0x00);
    wr(0x08, 0x07);

    calibrated = true;
    lastErr = 0;
    return true;
  }

  // 读一次, 输出 Pa。厂商对有效范围做了 300~1100 hPa 的合理性检查。
  bool read(float &pressureHpa, float &tempC) {
    lastErr = 0;
    if (!calibrated && !begin()) { errCount++; present = false; return false; }

    if (!waitStatus(0x30)) { lastErr = 3; errCount++; present = false; return false; }

    uint8_t b[6];
    if (!rd(0x00, b, 6)) { lastErr = 4; errCount++; present = false; return false; }

    int32_t rp = sx(((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2], 24);
    int32_t rt = sx(((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) | b[5], 24);

    double p = (double)rp / 524288.0;
    double t = (double)rt / 524288.0;

    double tc = (double)c0 * 0.5 + (double)c1 * t;
    double pa = (double)c00
              + p * ((double)c10 + p * ((double)c20 + p * ((double)c30 + p * (double)c40)))
              + t * ((double)c01 + p * ((double)c11 + p * ((double)c21 + p * (double)c31)));

    if (!isfinite(tc) || !isfinite(pa) || tc < -40.0 || tc > 85.0 || pa < 30000.0 || pa > 110000.0) {
      lastErr = 5; errCount++;
      return false;
    }

    pressureHpa = (float)(pa / 100.0);
    tempC = (float)tc;
    present = true;
    okCount++;
    return true;
  }

  // 寄存器快照, 现场判定厂商序列对不对
  void dumpRegs(void (*out)(const char *fmt, ...)) {
    uint8_t v;
    for (uint8_t r = 0x00; r <= 0x20; r++) {
      if (rd(r, &v, 1)) out("  0x%02X = 0x%02X\r\n", r, v);
      else            { out("  0x%02X = <读失败>\r\n", r); return; }
    }
  }

 private:
  static int32_t sx(uint32_t v, unsigned bits) {
    return (v & (1u << (bits - 1))) ? (int32_t)v - (int32_t)(1u << bits) : (int32_t)v;
  }
  static int32_t be16(const uint8_t *b) { return sx(((uint32_t)b[0] << 8) | b[1], 16); }

  bool wr(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
  }

  bool rd(uint8_t reg, uint8_t *d, size_t n) {
    Wire.beginTransmission(ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;   // 不发 STOP, 保持总线
    if (Wire.requestFrom((int)ADDR, (int)n) != (int)n) return false;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)Wire.read();
    return true;
  }

  bool waitStatus(uint8_t mask) {
    uint8_t st = 0;
    for (int i = 0; i <= 200; i++) {
      if (!rd(0x08, &st, 1)) return false;
      if ((st & mask) == mask) return true;
      delay(10);
    }
    return false;
  }
};

// ---------------------------------------------------------------- 组合
struct EnvState {
  bool tempValid = false;
  bool rhValid = false;
  bool pressValid = false;
  float tempC = 0.0f;
  float rhPct = 0.0f;
  float pressHpa = 0.0f;
  uint32_t stampMs = 0;
  uint32_t seq = 0;
};

class EnvHub {
 public:
  Gxht30c th;
  Bmp388Tokmas baro;
  EnvState state;

  // 上次成功/失败的时间, 用于"掉线每 2 秒重试"
  uint32_t thNextTry = 0;
  uint32_t baroNextTry = 0;

  static const uint32_t RETRY_MS = 2000;

  void begin() {
    th.probe();
    if (th.present) th.present = th.present;   // 探测到就等第一次 read
    baro.probe();
    if (baro.present) baro.begin();
  }

  // 采一轮。单项失败只清该项, 其他照常刷新。
  void poll() {
    uint32_t now = millis();

    // ---- 温湿度 ----
    if (th.present) {
      float t, h;
      if (th.read(t, h)) {
        state.tempC = t;
        state.rhPct = h;
        state.tempValid = true;
        state.rhValid = true;
      } else {
        state.tempValid = false;
        state.rhValid = false;
        thNextTry = now + RETRY_MS;
      }
    } else if (now >= thNextTry) {
      thNextTry = now + RETRY_MS;
      th.probe();
    }

    // ---- 气压 ----
    if (baro.present) {
      float p, it;
      if (baro.read(p, it)) {
        state.pressHpa = p;
        state.pressValid = true;
      } else {
        state.pressValid = false;
        baro.calibrated = false;      // 下次重新 init
        baroNextTry = now + RETRY_MS;
      }
    } else if (now >= baroNextTry) {
      baroNextTry = now + RETRY_MS;
      baro.probe();
      if (baro.present) baro.begin();
    }

    state.stampMs = now;
    state.seq++;
  }

  bool anyValid() const { return state.tempValid || state.rhValid || state.pressValid; }
};
