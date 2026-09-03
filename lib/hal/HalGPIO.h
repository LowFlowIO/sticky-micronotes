#pragma once

#include <Arduino.h>
#include <InputManager.h>
#include "board_pins.h"

class HalGPIO {
  InputManager inputMgr;

 public:
  HalGPIO() = default;

  void begin();

  void update();
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  void setUiOrientation(uint8_t orientation);
  bool consumeTap(int& x, int& y);


  void startDeepSleep();

  int getBatteryPercentage() const;
  bool isUsbConnected() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };
  WakeupReason getWakeupReason() const;

  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};
