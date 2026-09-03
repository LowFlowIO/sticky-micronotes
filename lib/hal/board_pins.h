#pragma once

// Hardware pin map for Seeed reTerminal Sticky
// (ESP32-S3R8, SSD1677 800x480, 3 buttons + GT911).


// Shared SPI bus: e-paper + MicroSD
#define EPD_SCLK 13
#define EPD_MOSI 14
#define EPD_CS 15
#define EPD_DC 16
#define EPD_RST 17
#define EPD_BUSY 18
#define EPD_PWR_EN 47
#define SPI_MISO 12

#define SD_CS 8
#define SD_PWR_EN 10
#define SD_DET 11
#define SD_SPI_HZ 20000000
#define EPD_SPI_HZ 20000000

// Physical buttons (active-low, 10k pull-up)
#define BTN_PIN_POWER 4   // AI / Power / OK — short = confirm, hold = sleep
#define BTN_PIN_UP 5
#define BTN_PIN_DOWN 6

// Power rails — must be driven HIGH immediately at boot or the board dies
#define PWR_HOLD 45
#define PWR_LOCK 46
#define EN_BAT_CHG 39  // BQ25616 /CE, active-low
#define CHARGE_STATE 40

// BQ27220 fuel gauge on Wire1 (sensor bus)
#define GAUGE_SDA 1
#define GAUGE_SCL 0
#define GAUGE_I2C_ADDR 0x55

// GT911 touch on Wire (own bus)
#define TOUCH_SDA 3
#define TOUCH_SCL 2
#define TOUCH_INT 21
#define TOUCH_RST 41
#define TOUCH_EN 42
#define TOUCH_I2C_ADDR 0x5D

#define BUZZER_PIN 48

