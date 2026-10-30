// Host versions of the T5 hardware glue that src/platform/extra_variants/t5s3_epaper/variant.cpp provides
// on the device (that file compiles empty here: it is all under T5_S3_EPAPER_PRO).
#ifdef T5_SCREENS_HOST
#include "configuration.h"

#include "graphics/niche/InkHUD/InkHUD.h"

#include "T5AppsApplet.h"
#include "T5MenuApplet.h"
#include "T5MenuTask.h"
#include "T5MessageModel.h"
#include "T5SystemLayer.h"

static bool hostTouchPref = true;
static bool hostTouchForced = false;
static bool hostBacklight = true;
static bool hostGate = false;

bool isTouchInputEnabled()
{
    return hostTouchPref && !hostTouchForced;
}
void setTouchInputEnabled(bool enabled, bool)
{
    hostTouchPref = enabled;
}
void toggleTouchInputEnabled()
{
    hostTouchPref = !hostTouchPref;
}
void t5BacklightSetUserEnabled(bool enabled)
{
    hostBacklight = enabled;
}
bool t5BacklightIsUserEnabled()
{
    return hostBacklight;
}
bool t5BacklightLoadPreference()
{
    return hostBacklight;
}
void t5BacklightToggleUser()
{
    hostBacklight = !hostBacklight;
}
void t5BacklightSetForcedByTimeout(bool) {}
void t5BacklightSetForcedBySleep(bool) {}
void t5TouchSetForcedByTimeout(bool forced)
{
    hostTouchForced = forced;
    hostGate = forced;
}
bool t5TouchIsForcedByTimeout()
{
    return hostTouchForced;
}

namespace T5UiGate
{
bool snapshotHeld()
{
    return hostGate;
}
void enterTimeout()
{
    t5TouchSetForcedByTimeout(true);
}
void userWake()
{
    t5TouchSetForcedByTimeout(false);
}
bool claimWake()
{
    return false;
}
} // namespace T5UiGate

void t5ScreenTimeoutApply() {}
void t5ScreenTimeoutArm() {}
bool t5ScreenTimeoutExpired()
{
    return false;
}
bool t5ScreenTimeoutDisabled()
{
    return config.display.screen_on_secs == 0;
}
void t5SetHomeCapButtonEventsEnabled(bool) {}
bool t5TouchPreferenceEnabled()
{
    return hostTouchPref;
}
bool t5TouchIsDown()
{
    return false;
}
uint32_t t5TouchDownAtMs()
{
    return 0;
}
void t5BootShortPress() {}
void t5BootLongPress() {}

T5Mode t5CurrentMode()
{
    return (NicheGraphics::InkHUD::InkHUD::getInstance()->persistence->settings.rotation % 2) ? T5Mode::CARRY : T5Mode::CONSOLE;
}

// Same sequence as the device's t5SetMode, minus the forced FULL and the save
void t5SetMode(T5Mode mode)
{
    using namespace NicheGraphics::InkHUD;
    auto *inkhud = InkHUD::getInstance();
    uint8_t &rotation = inkhud->persistence->settings.rotation;
    if (rotation == static_cast<uint8_t>(mode))
        return;
    rotation = static_cast<uint8_t>(mode);
    inkhud->persistence->settings.joystick.alignment = (4 - rotation) % 4;
    inkhud->updateLayout();
    if (auto *layer = T5SystemLayer::getIfBegun())
        layer->onModeChanged();
    T5MessageModel::onModeChanged();
    T5AppsApplet::onModeChanged();
    T5MenuApplet::onModeChanged();
    T5MenuTask::onModeChanged();
}

void t5ToggleMode()
{
    t5SetMode(t5CurrentMode() == T5Mode::CARRY ? T5Mode::CONSOLE : T5Mode::CARRY);
}
#endif
