// Force-included into every TU of env:native-t5-screens. On the device these come from
// variants/esp32s3/t5s3_epaper/variant.h, which can't be the host's variant.h (it carries ESP32 pins).
// Keep in step with that header's T5 glue declarations.
#pragma once
#ifdef __cplusplus
#include <cstdint>

bool isTouchInputEnabled();
void setTouchInputEnabled(bool enabled, bool showIndicator);
void toggleTouchInputEnabled();
void t5BacklightSetUserEnabled(bool enabled);
bool t5BacklightIsUserEnabled();
bool t5BacklightLoadPreference();
void t5BacklightToggleUser();
void t5BacklightSetForcedByTimeout(bool forced);
void t5BacklightSetForcedBySleep(bool forced);
void t5TouchSetForcedByTimeout(bool forced);
bool t5TouchIsForcedByTimeout();

namespace T5UiGate
{
bool snapshotHeld();
void enterTimeout();
void userWake();
bool claimWake();
} // namespace T5UiGate

void t5ScreenTimeoutApply();
void t5ScreenTimeoutArm();
bool t5ScreenTimeoutExpired();
bool t5ScreenTimeoutDisabled();
void t5SetHomeCapButtonEventsEnabled(bool enabled);
bool t5TouchPreferenceEnabled();
bool t5TouchIsDown();
uint32_t t5TouchDownAtMs();

void t5BootShortPress();
void t5BootLongPress();

enum class T5Mode : uint8_t {
    CARRY = 3,
    CONSOLE = 0,
};
T5Mode t5CurrentMode();
void t5SetMode(T5Mode mode);
void t5ToggleMode();
#endif

#define EPD_WIDTH 960
#define EPD_HEIGHT 540
#define HAS_TOUCHSCREEN 1
#ifndef BUTTON_PIN
#define BUTTON_PIN 0
#endif
