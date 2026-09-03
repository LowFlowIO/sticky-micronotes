#pragma once

#include <Arduino.h>
#include <Wire.h>

// Minimal BQ27220 reader (RelativeStateOfCharge at 0x2C).
// Sticky wires the gauge on Wire1: SDA=GPIO1, SCL=GPIO0, addr 0x55.
class Bq27220 {
 public:
  bool begin(TwoWire& wire, uint8_t sda, uint8_t scl, uint8_t addr = 0x55) {
    _wire = &wire;
    _addr = addr;
    _wire->begin(sda, scl, 400000);
    return readSoc() >= 0;
  }

  // Returns 0-100, or -1 on I2C failure.
  int readSoc() {
    if (!_wire) return -1;
    _wire->beginTransmission(_addr);
    _wire->write(0x2C);  // RelativeStateOfCharge
    if (_wire->endTransmission(false) != 0) return -1;
    if (_wire->requestFrom(static_cast<int>(_addr), 2) < 1) return -1;
    const int lo = _wire->read();
    int hi = 0;
    if (_wire->available()) hi = _wire->read();
    (void)hi;
    if (lo < 0) return -1;
    if (lo > 100) return 100;
    return lo;
  }

 private:
  TwoWire* _wire = nullptr;
  uint8_t _addr = 0x55;
};
