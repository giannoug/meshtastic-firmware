#include "pins_arduino.h"

// Display (E-Ink) ED047TC1 - 8bit parallel
#define EPD_WIDTH 960
#define EPD_HEIGHT 540

#define CANNED_MESSAGE_MODULE_ENABLE 1
#define USE_VIRTUAL_KEYBOARD 1

#if defined(T5_S3_EPAPER_PRO_V1)
#define BOARD_BL_EN 40
#else
#define BOARD_BL_EN 11
#endif

#define I2C_SDA SDA
#define I2C_SCL SCL

#define HAS_TOUCHSCREEN 1
#define TOUCH_POLL_INTERVAL_IDLE 25
#define TOUCH_POLL_INTERVAL_ACTIVE 15
#define TOUCH_POLL_INTERVAL_RELEASE 20
#define TOUCH_POLL_INTERVAL_ACTIVE_FAST 8
#define TOUCH_POLL_INTERVAL_RELEASE_FAST 8
#define GT911_PIN_SDA SDA
#define GT911_PIN_SCL SCL
#if defined(T5_S3_EPAPER_PRO_V1)
#define GT911_PIN_INT 15
#define GT911_PIN_RST 41
#else
#define GT911_PIN_INT 3
#define GT911_PIN_RST 9
#endif
// Do not use touch as a light-sleep wake source on T5-S3.
// Wake should come from physical buttons/radio/timer only.
#define SCREEN_TOUCH_INT GT911_PIN_INT

// Touch control helpers for this variant
bool isTouchInputEnabled();
void setTouchInputEnabled(bool enabled, bool showIndicator);
void toggleTouchInputEnabled();

// Backlight control helpers for this variant (non-latching behavior)
void t5BacklightSetUserEnabled(bool enabled);
bool t5BacklightIsUserEnabled();
void t5BacklightToggleUser();
void t5BacklightSetForcedByTimeout(bool forced);
void t5BacklightSetForcedBySleep(bool forced);

// Touch timeout/wake helpers for this variant
void t5TouchSetForcedByTimeout(bool forced);
bool t5TouchIsForcedByTimeout();

// The authoritative T5 UI gate.
//
// The display timeout is the only thing that freezes the T5 UI, and this is the only place that says so.
// It is deliberately NOT any of these, and must never be inferred from them:
//   - PowerFSM state: an incoming message, a BLE pairing request or a USB connect all leave DARK
//     without the user having touched the device
//   - the manual touch preference (TOUCH OFF): the user's own setting, and the app keeps rendering live
//   - the light-sleep touch gate: a consequence of the timeout gate, not a second state
//   - the front-light user preference: independent of both
//
// The touch timeout gate stays the single underlying truth; these are names for it, not a second flag.
namespace T5UiGate
{
// Snapshot semantics are in force: the base screen is frozen from the moment the gate was set.
bool snapshotHeld();

// The display timeout gated the UI. Called from PowerFSM's DARK entry action.
void enterTimeout();

// Real user input reached the device. Called only from PowerFSM's EVENT_PRESS / EVENT_INPUT transition
// actions, never from a state entry: entering ON is not by itself evidence that the user did anything.
void userWake();

// Claim the most recent wake, if no input path has claimed it yet.
//
// userWake() records a wake each time it actually releases the gate, which only ever happens as the
// direct result of physical user input. Every T5 press path calls this straight after delivering its
// power event: true means this press is the one that woke the display, so the press only wakes and is
// not also acted on. One atomic test-and-clear, so exactly one press is ever consumed per wake - no
// timing window, no PowerFSM state read back afterwards, and a slow e-paper refresh in between cannot
// change the answer.
//
// Every path that can wake a gated T5 must call this, so that a wake is always claimed by the press
// which caused it and never by an unrelated later press. Those paths are the side key and BOOT; touch
// and the capacitive Home key are gated off while asleep and cannot wake the device at all.
bool claimWake();
} // namespace T5UiGate

// Gate GT911 capacitive-home callback delivery until InkHUD startup is complete.
void t5SetHomeCapButtonEventsEnabled(bool enabled);

#ifdef MESHTASTIC_INCLUDE_NICHE_GRAPHICS
// InkHUD Carry (portrait, 508x928) / Console (landscape, 928x508) mode. Value is the InkHUD rotation.
// Console uses rotation 0 (not 2) so the side key sits on the right edge.
enum class T5Mode : uint8_t {
    CARRY = 3,
    CONSOLE = 0,
};
T5Mode t5CurrentMode();
void t5SetMode(T5Mode mode);
void t5ToggleMode();
#endif

#define PCF8563_RTC 0x51
#define HAS_RTC 1
#define PCF8563_INT 2

#define USE_POWERSAVE
#define SLEEP_TIME 120

// GPS
#if !defined(T5_S3_EPAPER_PRO_V1)
#define GPS_RX_PIN 44
#define GPS_TX_PIN 43
#endif

#if defined(T5_S3_EPAPER_PRO_V1)
#define BUTTON_PIN 48
#define PIN_BUTTON2 0
#define ALT_BUTTON_PIN PIN_BUTTON2
#else
#define BUTTON_PIN 0
#define BOARD_PCA9535_ADDR 0x20
#define BOARD_PCA9535_INT 38
#define BOARD_PCA9535_BUTTON_MASK 0x04
#endif
// SD card
#define HAS_SDCARD
#define SDCARD_CS SPI_CS
#define SD_SPI_FREQUENCY 75000000U

// battery charger BQ25896
#define HAS_PPM 1
#define XPOWERS_CHIP_BQ25896

// battery quality management BQ27220
#define HAS_BQ27220 1
#define BQ27220_I2C_SDA SDA
#define BQ27220_I2C_SCL SCL
#define BQ27220_DESIGN_CAPACITY 1500

// LoRa
#define USE_SX1262
#define USE_SX1268

#define LORA_SCK SCK
#define LORA_MISO MISO
#define LORA_MOSI MOSI
#define LORA_CS 46

#define LORA_DIO0 -1
#if defined(T5_S3_EPAPER_PRO_V1)
#define LORA_RESET 43
#define LORA_DIO1 3  // SX1262 IRQ
#define LORA_DIO2 44 // SX1262 BUSY
#define LORA_DIO3
#else
#define LORA_RESET 1
#define LORA_DIO1 10 // SX1262 IRQ
#define LORA_DIO2 47 // SX1262 BUSY
#define LORA_DIO3
#endif

#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_BUSY LORA_DIO2
#define SX126X_RESET LORA_RESET
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_DIO3_TCXO_VOLTAGE 2.4
