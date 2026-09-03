#pragma once

#include <Arduino.h>

class InputManager {
 public:
  InputManager();
  void begin();
  uint8_t getState();
  void update();

  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;

  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;


  bool isPowerButtonPressed() const;
  static const char* getButtonName(uint8_t buttonIndex);

  void setUiOrientation(uint8_t orientation);
  bool consumeTap(int& x, int& y);

 private:

  uint8_t currentState;
  uint8_t lastState;
  uint8_t pressedEvents;
  uint8_t releasedEvents;
  unsigned long lastDebounceTime;
  unsigned long buttonPressStart;
  unsigned long buttonPressFinish;

  static constexpr unsigned long DEBOUNCE_DELAY = 5;
  static const char* BUTTON_NAMES[];
};
