#include "imu_gyro.h"

#include <Wire.h>
#include <board_pins.h>

static constexpr uint8_t LSM6_ADDR = 0x6A;
static constexpr uint8_t REG_WHO = 0x0F;
static constexpr uint8_t REG_CTRL1_XL = 0x10;
static constexpr uint8_t REG_CTRL3_C = 0x12;
static constexpr uint8_t REG_OUTX_L_XL = 0x28;
static bool ready = false;

static bool wr(uint8_t reg, uint8_t val) {
  Wire1.beginTransmission(LSM6_ADDR);
  Wire1.write(reg);
  Wire1.write(val);
  return Wire1.endTransmission() == 0;
}

static bool rd(uint8_t reg, uint8_t* buf, size_t n) {
  Wire1.beginTransmission(LSM6_ADDR);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom((int)LSM6_ADDR, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire1.read();
  return true;
}

bool imuBegin() {
  if (ready) return true;
  Wire1.begin(GAUGE_SDA, GAUGE_SCL);
  delay(5);
  uint8_t who = 0;
  if (!rd(REG_WHO, &who, 1)) return false;
  // LSM6DS3 / LSM6DS3TR-C WHO_AM_I is 0x69
  if (who != 0x69 && who != 0x6A) return false;
  wr(REG_CTRL3_C, 0x04);     // IF_INC
  wr(REG_CTRL1_XL, 0x40);    // 104 Hz, ±2g
  ready = true;
  return true;
}

bool imuReadOrientation(Orientation& out) {
  if (!ready && !imuBegin()) return false;
  uint8_t raw[6];
  if (!rd(REG_OUTX_L_XL, raw, 6)) return false;
  int16_t ax = (int16_t)(raw[0] | (raw[1] << 8));
  int16_t ay = (int16_t)(raw[2] | (raw[3] << 8));
  int16_t az = (int16_t)(raw[4] | (raw[5] << 8));

  int absx = ax < 0 ? -ax : ax;
  int absy = ay < 0 ? -ay : ay;
  int absz = az < 0 ? -az : az;

  // ~0.55g in ±2g (16-bit, 0.061 mg/LSB) ≈ 9000
  const int minG = 9000;
  const int dominate = 2500;

  if (absz >= absx && absz >= absy) {
    // Flat on the table — leave orientation alone.
    return false;
  }

  if (absx >= absy && absx >= minG && (absx - absy) > dominate) {
    out = (ax > 0) ? Orientation::LANDSCAPE_CW : Orientation::LANDSCAPE_CCW;
    return true;
  }
  if (absy >= absx && absy >= minG && (absy - absx) > dominate) {
    out = (ay > 0) ? Orientation::PORTRAIT_INV : Orientation::PORTRAIT;
    return true;
  }
  return false;
}
