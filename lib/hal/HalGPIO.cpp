#include <HalGPIO.h>
#include <Preferences.h>
#include <SPI.h>
#include <esp_sleep.h>

#include "Bq27220.h"

static void holdPowerRails() {
  pinMode(PWR_HOLD, OUTPUT);
  pinMode(PWR_LOCK, OUTPUT);
  digitalWrite(PWR_HOLD, HIGH);
  digitalWrite(PWR_LOCK, HIGH);

  pinMode(EN_BAT_CHG, OUTPUT);
  digitalWrite(EN_BAT_CHG, LOW);

  pinMode(EPD_PWR_EN, OUTPUT);
  digitalWrite(EPD_PWR_EN, HIGH);

  pinMode(SD_PWR_EN, OUTPUT);
  digitalWrite(SD_PWR_EN, HIGH);

  pinMode(TOUCH_EN, OUTPUT);
  digitalWrite(TOUCH_EN, HIGH);

  pinMode(CHARGE_STATE, INPUT);
}

void HalGPIO::begin() {
  holdPowerRails();
  inputMgr.begin();
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);
}

void HalGPIO::update() { inputMgr.update(); }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

void HalGPIO::setUiOrientation(uint8_t orientation) { inputMgr.setUiOrientation(orientation); }

bool HalGPIO::consumeTap(int& x, int& y) { return inputMgr.consumeTap(x, y); }


void HalGPIO::startDeepSleep() {
  while (inputMgr.isPressed(BTN_POWER)) {
    delay(50);
    inputMgr.update();
  }
  esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BTN_PIN_POWER), 0);
  esp_deep_sleep_start();
}

int HalGPIO::getBatteryPercentage() const {
  static Bq27220 gauge;
  static bool gaugeReady = false;
  static int cachedPct = -1;
  static unsigned long lastReadMs = 0;

  if (!gaugeReady) {
    gaugeReady = gauge.begin(Wire1, GAUGE_SDA, GAUGE_SCL, GAUGE_I2C_ADDR);
  }

  unsigned long now = millis();
  if (cachedPct < 0 || (now - lastReadMs) >= 15000) {
    const int soc = gauge.readSoc();
    if (soc >= 0) cachedPct = soc;
    lastReadMs = now;
  }
  return cachedPct < 0 ? 0 : cachedPct;
}

bool HalGPIO::isUsbConnected() const {
  return digitalRead(CHARGE_STATE) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const bool usbConnected = isUsbConnected();
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  if ((wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP && usbConnected) ||
      (wakeupCause == ESP_SLEEP_WAKEUP_EXT0 && resetReason == ESP_RST_DEEPSLEEP)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}
