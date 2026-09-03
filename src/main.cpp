#include <Arduino.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <GfxRenderer.h>
#include <esp_pm.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <Preferences.h>
#include "sd_backup.h"

#include "config.h"
#include "ble_keyboard.h"
#include "input_handler.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ui_renderer.h"
#include "wifi_sync.h"
#include "imu_gyro.h"
#include "sticky_logo.h"
#include "sticky_sleep.h"

// Enum for sleep reasons
enum class SleepReason {
  POWER_LONGPRESS,
  IDLE_TIMEOUT,
  MENU_ACTION
};

// Forward declarations
void renderSleepScreen();
void enterDeepSleep(SleepReason reason);

// External variables
extern bool autoReconnectEnabled;

// --- Hardware objects ---
HalDisplay display;
GfxRenderer renderer(display);
HalGPIO gpio;


// --- Persistent settings (NVS) ---
static Preferences uiPrefs;

// --- Shared UI state ---
UIState currentState = UIState::MAIN_MENU;
int mainMenuSelection = 0;
int selectedFileIndex = 0;
int settingsSelection = 0;
int bluetoothDeviceSelection = 0;
int pairedKeyboardSelection = 0;
Orientation currentOrientation = Orientation::PORTRAIT;
bool gyroAuto = false;
int charsPerLine = 40;
bool screenDirty = true;

// Rename buffer
char renameBuffer[MAX_FILENAME_LEN] = "";
int renameBufferLen = 0;

// UI mode flags
bool darkMode = false;
bool cleanMode = false;
bool deleteConfirmPending = false;
WritingMode writingMode = WritingMode::NORMAL;
FontSize fontSize = FontSize::LARGE;
bool showWordCount = true;

// --- OTA App Detection ---
OtaAppEntry otaApps[MAX_OTA_APPS];
int otaAppCount = 0;

// Register this app's display name in shared NVS, keyed by OTA slot number.
static void registerOtaAppName(const char* name) {
  const esp_partition_t* self = esp_ota_get_running_partition();
  if (!self) return;
  int slot = self->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
  char key[16];
  snprintf(key, sizeof(key), "ota_%d", slot);
  Preferences prefs;
  prefs.begin("ota_names", false);
  prefs.putString(key, name);
  prefs.end();
  DBG_PRINTF("[OTA] Registered as \"%s\" in slot %d\n", name, slot);
}

// Scan all OTA partitions (except self), check for valid firmware, populate otaApps[].
static void detectOtaApps() {
  otaAppCount = 0;
  const esp_partition_t* running = esp_ota_get_running_partition();
  Preferences otaPrefs;
  otaPrefs.begin("ota_names", true);  // read-only

  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                    ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it != NULL && otaAppCount < MAX_OTA_APPS) {
    const esp_partition_t* part = esp_partition_get(it);
    if (part && part != running
        && part->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0
        && part->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_15) {

      esp_app_desc_t desc;
      if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
        int slot = part->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
        char key[16];
        snprintf(key, sizeof(key), "ota_%d", slot);
        String nvsName = otaPrefs.getString(key, "");

        OtaAppEntry& entry = otaApps[otaAppCount];
        if (nvsName.length() > 0) {
          strncpy(entry.name, nvsName.c_str(), sizeof(entry.name) - 1);
        } else {
          snprintf(entry.name, sizeof(entry.name), "OTA Slot %d", slot);
        }
        entry.name[sizeof(entry.name) - 1] = '\0';
        entry.partitionSubtype = part->subtype;
        otaAppCount++;
      }
    }
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  otaPrefs.end();
  DBG_PRINTF("[OTA] Detected %d additional app(s)\n", otaAppCount);
}

// Switch to another OTA app by index into otaApps[]. Non-static so input_handler can call it.
void switchToOtaApp(int index) {
  if (index < 0 || index >= otaAppCount) return;
  int subtype = otaApps[index].partitionSubtype;
  const esp_partition_t* target = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP,
      static_cast<esp_partition_subtype_t>(subtype), NULL);
  if (!target) {
    DBG_PRINTF("[OTA] Partition subtype %d not found!\n", subtype);
    return;
  }
  DBG_PRINTF("[OTA] Switching to \"%s\" (subtype %d)...\n", otaApps[index].name, subtype);
  esp_ota_set_boot_partition(target);
  esp_restart();
}

// --- Screen update ---
static void updateScreen() {
  if (!screenDirty) return;
  screenDirty = false;

  // Apply orientation
  static Orientation lastOrientation = Orientation::PORTRAIT;
  if (currentOrientation != lastOrientation) {
    GfxRenderer::Orientation gfxOrient = GfxRenderer::Portrait;
    switch (currentOrientation) {
      case Orientation::PORTRAIT:      gfxOrient = GfxRenderer::Portrait; break;
      case Orientation::LANDSCAPE_CW:  gfxOrient = GfxRenderer::LandscapeClockwise; break;
      case Orientation::PORTRAIT_INV:  gfxOrient = GfxRenderer::PortraitInverted; break;
      case Orientation::LANDSCAPE_CCW: gfxOrient = GfxRenderer::LandscapeCounterClockwise; break;
    }
    renderer.setOrientation(gfxOrient);
    gpio.setUiOrientation(static_cast<uint8_t>(currentOrientation));
    lastOrientation = currentOrientation;
  }

  // Auto-compute chars per line from font metrics so text always fills the screen
  {
    int sw = renderer.getScreenWidth();
    int textAreaWidth = sw - 20;  // 10px margins each side
    int avgCharW = renderer.getTextAdvanceX(editorFontId(fontSize), "abcdefghijklmnopqrstuvwxyz") / 26;
    if (avgCharW > 0) charsPerLine = textAreaWidth / avgCharW;
  }
  editorSetCharsPerLine(charsPerLine);

  switch (currentState) {
    case UIState::MAIN_MENU:         drawMainMenu(renderer, gpio); break;
    case UIState::FILE_BROWSER:      drawFileBrowser(renderer, gpio); break;
    case UIState::TEXT_EDITOR:       drawTextEditor(renderer, gpio); break;
    case UIState::RENAME_FILE:       drawRenameScreen(renderer, gpio); break;
    case UIState::SETTINGS:          drawSettingsMenu(renderer, gpio); break;
    case UIState::BLUETOOTH_SETTINGS: drawBluetoothSettings(renderer, gpio); break;
    case UIState::PAIRED_KEYBOARDS:   drawPairedKeyboardsMenu(renderer, gpio); break;
    case UIState::WIFI_SYNC:          drawSyncScreen(renderer, gpio); break;
    default: break;
  }
}

void setup() {
  DBG_INIT();
  DBG_PRINTLN("Sticky MicroNotes starting...");

  setCpuFrequencyMhz(80);

  gpio.begin();
  imuBegin();
  display.begin();

  renderer.setFadingFix(true);  // Power down display analog circuits after each refresh — reduces idle drain
  rendererSetup(renderer);

  // Load persisted UI settings from NVS early so startup screen uses saved orientation
  uiPrefs.begin("ui_prefs", false);
  currentOrientation = static_cast<Orientation>(uiPrefs.getUChar("orient", 0));
  darkMode = uiPrefs.getBool("darkMode", false);
  writingMode = static_cast<WritingMode>(uiPrefs.getUChar("writeMode", 0));
  fontSize = static_cast<FontSize>(uiPrefs.getUChar("fontSize", 2));
  showWordCount = uiPrefs.getBool("showWC", true);
  gyroAuto = uiPrefs.getBool("gyroAuto", false);

  // Apply saved orientation
  {
    GfxRenderer::Orientation gfxOrient = GfxRenderer::Portrait;
    switch (currentOrientation) {
      case Orientation::PORTRAIT:      gfxOrient = GfxRenderer::Portrait; break;
      case Orientation::LANDSCAPE_CW:  gfxOrient = GfxRenderer::LandscapeClockwise; break;
      case Orientation::PORTRAIT_INV:  gfxOrient = GfxRenderer::PortraitInverted; break;
      case Orientation::LANDSCAPE_CCW: gfxOrient = GfxRenderer::LandscapeCounterClockwise; break;
    }
    renderer.setOrientation(gfxOrient);
    gpio.setUiOrientation(static_cast<uint8_t>(currentOrientation));
  }

  editorInit();
  inputSetup();
  fileManagerSetup();

  // Restore UI prefs from SD backup if NVS was wiped by a firmware flash
  if (!uiPrefs.isKey("orient")) {
    static char uiBuf[128];
    if (sdReadFile("/sticky/ui_prefs.json", uiBuf, sizeof(uiBuf))) {
      int o  = jsonGetInt(uiBuf, "orient");
      int d  = jsonGetInt(uiBuf, "dark");
      int wm = jsonGetInt(uiBuf, "writeMode");
      int fs = jsonGetInt(uiBuf, "fontSize");
      int wc = jsonGetInt(uiBuf, "showWC");
      if (o  >= 0) { uiPrefs.putUChar("orient",    (uint8_t)o);  currentOrientation = static_cast<Orientation>(o); }
      if (d  >= 0) { uiPrefs.putBool("darkMode",   d != 0);      darkMode           = (d != 0); }
      if (wm >= 0) { uiPrefs.putUChar("writeMode", (uint8_t)wm); writingMode        = static_cast<WritingMode>(wm); }
      if (fs >= 0) { uiPrefs.putUChar("fontSize",  (uint8_t)fs); fontSize           = static_cast<FontSize>(fs); }
      if (wc >= 0) { uiPrefs.putBool("showWC",     wc != 0);     showWordCount      = (wc != 0); }
      // Re-apply orientation in case it changed
      GfxRenderer::Orientation gfxOrient = GfxRenderer::Portrait;
      switch (currentOrientation) {
        case Orientation::PORTRAIT:      gfxOrient = GfxRenderer::Portrait; break;
        case Orientation::LANDSCAPE_CW:  gfxOrient = GfxRenderer::LandscapeClockwise; break;
        case Orientation::PORTRAIT_INV:  gfxOrient = GfxRenderer::PortraitInverted; break;
        case Orientation::LANDSCAPE_CCW: gfxOrient = GfxRenderer::LandscapeCounterClockwise; break;
      }
      renderer.setOrientation(gfxOrient);
      gpio.setUiOrientation(static_cast<uint8_t>(currentOrientation));
      DBG_PRINTLN("UI prefs restored from SD backup");
    }
  }

  bleSetup();

  // Enable automatic light sleep between loop iterations.
  // CONFIG_PM_ENABLE and CONFIG_FREERTOS_USE_TICKLESS_IDLE are compiled into
  // ESP-IDF via sdkconfig.defaults (framework = arduino, espidf). BLE modem
  // sleep keeps the radio alive across sleep/wake cycles.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  esp_pm_config_esp32s3_t pm_config = {
    .max_freq_mhz = 80,
    .min_freq_mhz = 10,
    .light_sleep_enable = true
  };
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  esp_pm_config_esp32c3_t pm_config = {
    .max_freq_mhz = 80,
    .min_freq_mhz = 10,
    .light_sleep_enable = true
  };
#else
  esp_pm_config_t pm_config = {
    .max_freq_mhz = 80,
    .min_freq_mhz = 10,
    .light_sleep_enable = true
  };
#endif
  esp_err_t pm_err = esp_pm_configure(&pm_config);
  (void)pm_err;
  DBG_PRINTF("PM configure: %s\n", esp_err_to_name(pm_err));

  // Initialize auto-reconnect to enabled by default
  autoReconnectEnabled = true;

  // Register this app's name in shared NVS and detect other OTA apps
  registerOtaAppName("Sticky MicroNotes");
  detectOtaApps();

  DBG_PRINTLN("Sticky MicroNotes ready.");

  // The display needs one FULL_REFRESH after power-on to initialize its analog
  // circuits before FAST_REFRESH will work.
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);

  screenDirty = true;
}

// Enter deep sleep - matches crosspoint pattern
void enterDeepSleep(SleepReason reason) {
  DBG_PRINTLN("Entering deep sleep...");

  // Save first so a sleep from the editor cannot drop the last keystrokes.
  if (currentState == UIState::TEXT_EDITOR) {
    if (editorGetCurrentFile()[0] != '\0' && editorHasUnsavedChanges()) {
      saveCurrentFile(false);
    }
  }

  // Render the sleep screen before entering deep sleep
  renderSleepScreen();

  display.deepSleep();     // Power down display first
  gpio.startDeepSleep();   // Waits for power button release, then sleeps
  // Will not return - device is asleep
}

// Translate physical button presses to HID key codes
// NOTE: gpio.update() is called in loop() before this function
static void processPhysicalButtons() {
  static bool btnUpLast = false;
  static bool btnDownLast = false;
  static bool btnLeftLast = false;
  static bool btnRightLast = false;
  static bool btnBackLast = false;

  // Use isPressed() — persistent debounced state.  With one-shot scanning
  // (radio quiet during navigation), InputManager debounce works reliably.
  bool btnUp      = gpio.isPressed(HalGPIO::BTN_UP);
  bool btnDown    = gpio.isPressed(HalGPIO::BTN_DOWN);
  bool btnLeft    = gpio.isPressed(HalGPIO::BTN_LEFT);
  bool btnRight   = gpio.isPressed(HalGPIO::BTN_RIGHT);
  bool btnBack    = gpio.isPressed(HalGPIO::BTN_BACK);

  // Power button state machine for proper long/short press handling
  static bool powerHeld = false;
  static unsigned long powerPressStart = 0;
  static bool sleepTriggered = false;

  bool btnPower = gpio.isPressed(HalGPIO::BTN_POWER);

  if (btnPower && !powerHeld) {
    // Button just pressed
    powerHeld = true;
    sleepTriggered = false;
    powerPressStart = millis();
  }

  if (btnPower && powerHeld && !sleepTriggered) {
    if (millis() - powerPressStart > 3000) {
      sleepTriggered = true;
      enterDeepSleep(SleepReason::POWER_LONGPRESS);
      return; // Exit early to prevent further processing
    }
  }

  bool confirmTap = false;
  if (!btnPower && powerHeld) {
    // Button released
    unsigned long duration = millis() - powerPressStart;
    powerHeld = false;

    if (!sleepTriggered && duration > 50 && duration < 1000) {
      // Same physical key as Select. Only a short tap confirms.
      confirmTap = true;
    }
  }

  // Back button long-press for restart
  static bool backHeld = false;
  static unsigned long backPressStart = 0;
  static bool restartTriggered = false;

  if (btnBack && !backHeld) {
    backHeld = true;
    restartTriggered = false;
    backPressStart = millis();
  }

  if (btnBack && backHeld && !restartTriggered) {
    if (millis() - backPressStart > 5000) {
      restartTriggered = true;
      DBG_PRINTLN("BACK held for 5s — restarting device...");
      if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
        saveCurrentFile();
      }
      delay(100);
      ESP.restart();
    }
  }

  if (!btnBack && backHeld) {
    backHeld = false;
  }

  // Uniform bottom bar: Up | Down | Back | Select
  {
    int tx, ty;
    if (gpio.consumeTap(tx, ty)) {
      const int sw = (currentOrientation == Orientation::PORTRAIT ||
                      currentOrientation == Orientation::PORTRAIT_INV) ? 480 : 800;
      const int sh = (currentOrientation == Orientation::PORTRAIT ||
                      currentOrientation == Orientation::PORTRAIT_INV) ? 800 : 480;
      const int barH = 54;
      if (!(cleanMode && currentState == UIState::TEXT_EDITOR) &&
          ty >= sh - barH && tx >= 0 && tx < sw) {
        int slot = tx * 4 / sw;
        if (slot < 0) slot = 0;
        if (slot > 3) slot = 3;
        const uint8_t keys[4] = {HID_KEY_UP, HID_KEY_DOWN, HID_KEY_ESCAPE, HID_KEY_ENTER};
        enqueueKeyEvent(keys[slot], 0, true);
        enqueueKeyEvent(keys[slot], 0, false);
      }
    }
  }

  // Map physical buttons to HID key codes based on current UI state
  switch (currentState) {
    case UIState::MAIN_MENU:
      if ((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (confirmTap) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      break;

    case UIState::FILE_BROWSER:
      if (((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (confirmTap && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::TEXT_EDITOR: {
      // Key repeat state for held navigation/backspace keys
      static uint8_t repeatKey = 0;
      static unsigned long repeatStart = 0;
      static unsigned long lastRepeat = 0;
      const unsigned long REPEAT_DELAY = 400;
      const unsigned long REPEAT_RATE  = 80;

      auto fireKey = [](uint8_t k) {
        enqueueKeyEvent(k, 0, true);
        enqueueKeyEvent(k, 0, false);
      };

      // Map currently held button to HID key (0 = none)
      uint8_t heldKey = 0;
      if      (btnUp)    heldKey = HID_KEY_UP;
      else if (btnDown)  heldKey = HID_KEY_DOWN;
      else if (btnLeft)  heldKey = HID_KEY_LEFT;
      else if (btnRight) heldKey = HID_KEY_RIGHT;

      if (heldKey != repeatKey) {
        // Key changed — fire immediately on press
        if (heldKey != 0) fireKey(heldKey);
        repeatKey   = heldKey;
        repeatStart = millis();
        lastRepeat  = millis();
      } else if (heldKey != 0) {
        unsigned long now = millis();
        if (now - repeatStart > REPEAT_DELAY && now - lastRepeat > REPEAT_RATE) {
          fireKey(heldKey);
          lastRepeat = now;
        }
      }

      if (confirmTap) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        if (editorHasUnsavedChanges()) saveCurrentFile();
        currentState = UIState::FILE_BROWSER;
        screenDirty = true;
      }
      break;
    }

    case UIState::RENAME_FILE:
    case UIState::NEW_FILE:
      if (confirmTap) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::BLUETOOTH_SETTINGS:
      if (btnUp && !btnUpLast) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (btnDown && !btnDownLast) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnRight && !btnRightLast) {
        enqueueKeyEvent(HID_KEY_RIGHT, 0, true);  // Scan
        enqueueKeyEvent(HID_KEY_RIGHT, 0, false);
      }
      if (btnLeft && !btnLeftLast) {
        enqueueKeyEvent(HID_KEY_LEFT, 0, true);   // Disconnect
        enqueueKeyEvent(HID_KEY_LEFT, 0, false);
      }
      if (confirmTap) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::PAIRED_KEYBOARDS:
      if ((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (confirmTap) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::WIFI_SYNC:
      if ((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (confirmTap) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::SETTINGS:
      if ((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (confirmTap) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    default:
      break;
  }

  // Update last state
  btnUpLast = btnUp;
  btnDownLast = btnDown;
  btnLeftLast = btnLeft;
  btnRightLast = btnRight;
  btnBackLast = btnBack;
}

// Global variable for activity tracking
static unsigned long lastActivityTime = 0;
const unsigned long IDLE_TIMEOUT = 5UL * 60UL * 1000UL; // 5 minutes

void registerActivity() {
  lastActivityTime = millis();
}

// Function to render the sleep screen
static void drawStickyLogo(int x, int y) {
  const int rowb = (STICKY_LOGO_W + 7) / 8;
  for (int row = 0; row < STICKY_LOGO_H; row++) {
    for (int col = 0; col < STICKY_LOGO_W; col++) {
      uint8_t byte = pgm_read_byte(STICKY_LOGO + row * rowb + (col / 8));
      if (((byte >> (7 - (col & 7))) & 1) == 0) {
        renderer.drawPixel(x + col, y + row, true);
      }
    }
  }
}

static void draw1BitImage(const uint8_t* data, int w, int h, int x, int y) {
  const int rowb = (w + 7) / 8;
  for (int row = 0; row < h; row++) {
    for (int col = 0; col < w; col++) {
      uint8_t byte = pgm_read_byte(data + row * rowb + (col / 8));
      if (((byte >> (7 - (col & 7))) & 1) == 0) {
        renderer.drawPixel(x + col, y + row, true);
      }
    }
  }
}

void renderSleepScreen() {
  if (currentState == UIState::TEXT_EDITOR && editorGetLength() > 0) {
    drawEditorSleepPage(renderer, gpio);
    delay(500);
    return;
  }

  renderer.clearScreen();

  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  int logoW = STICKY_LOGO_W;
  int logoH = STICKY_LOGO_H;
  int logoX = (sw - logoW) / 2;
  if (logoX < 0) logoX = 0;
  int logoY = sh / 6;
  if (logoY + logoH > sh / 2) logoY = 16;
  drawStickyLogo(logoX, logoY);

  const char* title = "MicroNotes";
  int titleWidth = renderer.getTextAdvanceX(FONT_BODY, title);
  int titleX = (sw - titleWidth) / 2;
  int titleY = logoY + logoH + 16;
  renderer.drawText(FONT_BODY, titleX, titleY, title, true, EpdFontFamily::BOLD);

  const int afterTitle = 42;
  int footerY = sh - 36;
  int imgY = titleY + 28 + afterTitle;
  int imgW = STICKY_SLEEP_W;
  int imgH = STICKY_SLEEP_H;
  int imgX = (sw - imgW) / 2;
  if (imgX < 0) imgX = 0;
  if (imgY + imgH > footerY - 8) {
    imgW /= 2;
    imgH /= 2;
    imgX = (sw - imgW) / 2;
    if (imgX < 0) imgX = 0;
    const int rowb = (STICKY_SLEEP_W + 7) / 8;
    for (int row = 0; row < STICKY_SLEEP_H; row += 2) {
      for (int col = 0; col < STICKY_SLEEP_W; col += 2) {
        uint8_t byte = pgm_read_byte(STICKY_SLEEP + row * rowb + (col / 8));
        if (((byte >> (7 - (col & 7))) & 1) == 0) {
          renderer.drawPixel(imgX + col / 2, imgY + row / 2, true);
        }
      }
    }
  } else {
    draw1BitImage(STICKY_SLEEP, STICKY_SLEEP_W, STICKY_SLEEP_H, imgX, imgY);
  }

// Footer: "Hold Power to wake"
  
  // Perform a full display refresh to ensure the sleep screen is visible
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  
  // Small delay to ensure the display update is complete
  delay(500);
}

void loop() {
  // --- GPIO first: always poll buttons before anything else ---
  gpio.update();

  // Control auto-reconnect based on UI state
  static UIState lastState = UIState::MAIN_MENU;
  if (currentState == UIState::BLUETOOTH_SETTINGS) {
    autoReconnectEnabled = false;
    // On first entry to BT settings, do a one-shot scan
    if (lastState != UIState::BLUETOOTH_SETTINGS) {
      cancelPendingConnection();
      startDeviceScan();  // One-shot 5s scan, radio goes quiet after
    }
  } else {
    autoReconnectEnabled = true;
    if (lastState == UIState::BLUETOOTH_SETTINGS && isDeviceScanning()) {
      stopDeviceScan();
    }
  }
  lastState = currentState;

  // Process BLE (connection handling, scan completion detection)
  bleLoop();

  // Header "Connecting..." / "KB Connected" is only painted on redraw.
  {
    static BLEState lastBleShown = BLEState::DISCONNECTED;
    BLEState bleNow = getConnectionState();
    if (bleNow != lastBleShown) {
      lastBleShown = bleNow;
      screenDirty = true;
    }
  }

  // Process WiFi sync HTTP clients when active
  if (isWifiSyncActive()) wifiSyncLoop();

  // CRITICAL: Process buttons BEFORE checking wasAnyPressed() to avoid consuming button states
  processPhysicalButtons();
  int inputEventsProcessed = processAllInput(); // Assuming this returns number of events processed

  // Register activity AFTER button processing (don't consume button states prematurely)
  static unsigned long lastInputTime = 0;
  bool hadActivity = gpio.wasAnyPressed() || inputEventsProcessed > 0;
  if (hadActivity) {
    registerActivity();
    lastInputTime = millis();
  }

  // Auto-save: hybrid idle + hard cap for crash protection.
  // - Saves after 10s of no keystrokes (catches natural pauses between sentences)
  // - Hard cap every 2min during continuous typing (never lose more than 2min of work)
  static unsigned long lastAutoSaveMs = 0;
  if (currentState == UIState::TEXT_EDITOR
      && editorHasUnsavedChanges()
      && editorGetCurrentFile()[0] != '\0') {
    unsigned long now = millis();
    bool idleTrigger = (now - lastInputTime) > AUTO_SAVE_IDLE_MS
                    && (now - lastAutoSaveMs) > AUTO_SAVE_IDLE_MS;
    bool capTrigger  = (now - lastAutoSaveMs) > AUTO_SAVE_MAX_MS;
    if (idleTrigger || capTrigger) {
      lastAutoSaveMs = now;
      saveCurrentFile(false);  // Skip refreshFileList — file list unchanged by content update
    }
  }


  if (gyroAuto) {
    static unsigned long lastImuMs = 0;
    static Orientation pending = Orientation::PORTRAIT;
    static uint8_t pendingHits = 0;
    unsigned long now = millis();
    if (now - lastImuMs > 280) {
      lastImuMs = now;
      Orientation sensed;
      if (imuReadOrientation(sensed)) {
        if (sensed == pending) {
          if (pendingHits < 3) pendingHits++;
        } else {
          pending = sensed;
          pendingHits = 1;
        }
        if (pendingHits >= 2 && sensed != currentOrientation) {
          currentOrientation = sensed;
          screenDirty = true;
        }
      } else {
        pendingHits = 0;
      }
    }
  }

  // Periodically refresh sync screen to show status changes (every 2s)
  if (currentState == UIState::WIFI_SYNC) {
    static unsigned long lastSyncRefresh = 0;
    if (millis() - lastSyncRefresh > 2000) {
      screenDirty = true;
      lastSyncRefresh = millis();
    }
  }

  // Poll display refresh — non-blocking check of BUSY pin
  if (renderer.isRefreshing()) {
    renderer.pollRefresh();
  }

  // Don't start a new screen update while display is still refreshing
  if (screenDirty && !renderer.isRefreshing()) {
    updateScreen();
  }

  // Persist UI settings to NVS when they change (NVS write only on change, not every loop)
  static Orientation lastSavedOrientation = currentOrientation;
  static bool lastSavedDarkMode = darkMode;
  static WritingMode lastSavedWritingMode = writingMode;
  static FontSize lastSavedFontSize = fontSize;
  static bool lastSavedShowWordCount = showWordCount;
  static bool lastSavedGyroAuto = gyroAuto;
  if (currentOrientation != lastSavedOrientation || darkMode != lastSavedDarkMode
      || writingMode != lastSavedWritingMode || fontSize != lastSavedFontSize
      || showWordCount != lastSavedShowWordCount || gyroAuto != lastSavedGyroAuto) {
    uiPrefs.putUChar("orient", static_cast<uint8_t>(currentOrientation));
    uiPrefs.putBool("darkMode", darkMode);
    uiPrefs.putUChar("writeMode", static_cast<uint8_t>(writingMode));
    uiPrefs.putUChar("fontSize", static_cast<uint8_t>(fontSize));
    uiPrefs.putBool("showWC", showWordCount);
    uiPrefs.putBool("gyroAuto", gyroAuto);
    lastSavedOrientation = currentOrientation;
    lastSavedDarkMode = darkMode;
    lastSavedWritingMode = writingMode;
    lastSavedFontSize = fontSize;
    lastSavedShowWordCount = showWordCount;
    lastSavedGyroAuto = gyroAuto;
    // Keep SD backup in sync so settings survive a firmware flash
    static char uiBuf[128];
    snprintf(uiBuf, sizeof(uiBuf),
             "{\"orient\":%d,\"dark\":%d,\"writeMode\":%d,\"fontSize\":%d,\"showWC\":%d,\"gyro\":%d}",
             (int)currentOrientation, darkMode ? 1 : 0,
             (int)writingMode, (int)fontSize, showWordCount ? 1 : 0, gyroAuto ? 1 : 0);
    if (!SdMan.exists("/sticky")) SdMan.mkdir("/sticky");
    sdWriteFile("/sticky/ui_prefs.json", uiBuf);
  }

  // Check for idle timeout (skip while WiFi sync is active)
  if (!isWifiSyncActive() && millis() - lastActivityTime > IDLE_TIMEOUT) {
    enterDeepSleep(SleepReason::IDLE_TIMEOUT);
  }

  // Adaptive delay with recently-active window for button responsiveness.
  // BLE keystrokes wake from light sleep via modem interrupt (delay value irrelevant).
  // Physical buttons are polled, so the idle delay must be short enough to catch a
  // quick tap (~80-150ms). 50ms idle guarantees 1-2 samples per press.
  // Stay at fast polling for 2s after any activity for snappy consecutive presses.
  static constexpr unsigned long ACTIVE_WINDOW_MS = 2000;
  bool recentlyActive = (millis() - lastInputTime) < ACTIVE_WINDOW_MS;
  delay((hadActivity || screenDirty || recentlyActive) ? 10 : 50);
}
