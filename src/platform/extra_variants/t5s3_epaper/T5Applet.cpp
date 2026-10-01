#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5Applet.h"

#include "PowerStatus.h"
#include "graphics/niche/InkHUD/SystemApplet.h"
#include "mesh/NodeDB.h"

using namespace NicheGraphics;

int8_t InkHUD::T5Applet::indexOf(const char *name)
{
    const std::vector<Applet *> &applets = InkHUD::getInstance()->userApplets;
    for (uint8_t i = 0; i < applets.size(); i++) {
        if (strcmp(applets[i]->name, name) == 0)
            return i;
    }
    return -1;
}

// The menu can re-enable the shared icon, which would overlap T5 headers: switched back off whenever a T5 screen shows
void InkHUD::T5Applet::onForeground()
{
    settings->optionalFeatures.batteryIcon = false;
    inkhud->getSystemApplet("BatteryIcon")->sendToBackground();
}

// Heard within the last 10 minutes: the HEARD 10M count, on every T5 screen
bool InkHUD::T5Applet::heardRecently(const meshtastic_NodeInfoLite *node)
{
    return node->last_heard && sinceLastSeen(node) < 10 * 60;
}

std::string InkHUD::T5Applet::hopsString(uint8_t hops)
{
    return hops == 0 ? "direct" : to_string(hops) + (hops == 1 ? " hop" : " hops");
}

std::string InkHUD::T5Applet::sinceString(uint32_t secs)
{
    if (secs < 60)
        return "now";
    if (secs < 60 * 60)
        return to_string(secs / 60) + " min";
    if (secs < 24 * 60 * 60)
        return to_string(secs / (60 * 60)) + " h";
    return to_string(secs / (24 * 60 * 60)) + " d";
}

std::string InkHUD::T5Applet::agoString(uint32_t secs)
{
    return secs < 60 ? sinceString(secs) : sinceString(secs) + " ago";
}

// Non-empty parts only, separated by a Win-1253 middle dot
std::string InkHUD::T5Applet::join(const std::string &a, const std::string &b)
{
    return a.empty() ? b : b.empty() ? a : a + " \xB7 " + b;
}

// The frozen power grammar (Lock and Power 06). Four real states plus "not initialised";
// there is deliberately no FULL state. Carry Home and the header show the short form,
// the Console Home segment the long one. Voltage appears only on battery.
std::string InkHUD::T5Applet::powerLabel(bool console, bool withVoltage)
{
    if (!powerStatus->isInitialized())
        return console ? std::string("BATT ") + DASH : "";

    if (!powerStatus->getHasBattery())
        return console ? "USB POWER" : "USB";

    const std::string percent = to_string(powerStatus->getBatteryChargePercent()) + " %";
    if (powerStatus->getHasUSB())
        return join(console ? "BATT " + percent : percent, powerStatus->getIsCharging() ? "CHG" : "USB");

    if (!console)
        return percent;
    if (!withVoltage)
        return "BATT " + percent; // Console drops the voltage before it lets the status line overflow
    char volts[12];
    snprintf(volts, sizeof(volts), "%.2f V", powerStatus->getBatteryVoltageMv() / 1000.0);
    return join("BATT " + percent, volts);
}

// Four bars, filled up to the shared getSignalStrength() rating, as NodeListApplet counts them
void InkHUD::T5Applet::drawSignal(int16_t left, int16_t bottom, SignalStrength signal)
{
    constexpr int16_t heights[] = {8, 13, 17, 22};
    for (uint8_t i = 0; i < 4; i++) {
        const int16_t x = left + i * 11;
        if (i <= signal)
            fillRect(x, bottom - heights[i], 8, heights[i], BLACK);
        else
            drawLine(x, bottom - 1, x + 7, bottom - 1, BLACK);
    }
}

#endif
