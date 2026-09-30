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

// Only Home and Nodes have a native T5 screen. The other four keep their cell, struck through, until
// their T5 destinations are built: pointing them at a stock InkHUD applet would drop the user into a
// surface with no T5 navigation and no way back (Redesign 8.4; Apps 05 retires the App Switcher on T5,
// Menu Settings 00 the shared menu).
int8_t InkHUD::T5Applet::destinationApplet(Destination d)
{
    const char *name = d == HOME ? "Home" : d == NODES ? "Nodes" : nullptr;
    if (!name)
        return -1;
    const int8_t i = indexOf(name);
    return (i >= 0 && inkhud->userApplets[i]->isActive()) ? i : -1;
}

// Six cells across navWidth(): the current one inverted, unavailable ones struck through
void InkHUD::T5Applet::drawNav()
{
    static constexpr const char *labels[] = {"HOME", "MSGS", "NODES", "MAP", "MENU", "APPS"};
    const Destination current = currentDestination();
    const int16_t top = navTop();
    const uint16_t navW = navWidth();

    drawLine(0, top - 1, width() - 1, top - 1, BLACK);
    setFont(fontSmall);
    for (uint8_t i = 0; i < 6; i++) {
        const int16_t left = i * navW / 6;
        const int16_t cellW = (i + 1) * navW / 6 - left;
        const int16_t centerX = left + cellW / 2;
        const int16_t centerY = top + NAV_H / 2;

        if (i == current) {
            fillRect(left, top, cellW, NAV_H, BLACK);
            setTextColor(WHITE);
            printThick(centerX, centerY, labels[i], 2, 1);
            setTextColor(BLACK);
            continue;
        }

        if (i > 0)
            drawLine(left, top, left, height() - 1, BLACK);
        printAt(centerX, centerY, labels[i], CENTER, MIDDLE);
        if (destinationApplet((Destination)i) < 0)
            strikeThrough(left, centerY, cellW);
    }

    if (isConsole())
        fillRect(navW, top, 2, NAV_H, BLACK); // Rule between the six cells and the mode slot
}

// An unavailable destination, and one already on screen, both ask for no update at all: nothing flickers
void InkHUD::T5Applet::openDestination(Destination d)
{
    const int8_t target = destinationApplet(d);
    if (target >= 0 && !inkhud->userApplets[target]->isForeground())
        inkhud->showApplet(target, EInk::UpdateTypes::UNSPECIFIED, true);
}

// Hit-tests the six cells only: the Console mode slot never resolves to a cell.
// An unavailable cell still consumes its tap, so the button fallback never sees it (Redesign 8.4)
bool InkHUD::T5Applet::handleNavTap(uint16_t x, uint16_t y)
{
    if (y < navTop() || x >= navWidth())
        return false;
    openDestination((Destination)(x * 6 / navWidth()));
    return true;
}

void InkHUD::T5Applet::printBold(int16_t x, int16_t y, const std::string &text, HorizontalAlignment ha)
{
    const int16_t w = getTextWidth(text);
    const int16_t centerX = ha == LEFT ? x + w / 2 : ha == RIGHT ? x - w / 2 : x;
    printThick(centerX, y + getFont().lineHeight() / 2, text, 2, 1);
}

// The one unavailable treatment: a 2 px rule through the label, inset 14 px (Redesign 8.4)
void InkHUD::T5Applet::strikeThrough(int16_t left, int16_t centerY, uint16_t w)
{
    fillRect(left + 14, centerY - 1, w - 28, 2, BLACK);
}

// A 2 px box with the frozen 14 / 8 px padding: the Nodes sort order and the Node detail favourite chip
int16_t InkHUD::T5Applet::boxedLabel(const char *text, int16_t bandH, bool draw)
{
    setFont(fontSmall);
    const int16_t w = getTextWidth(text) + 2 * 14 + 4, h = fontSmall.lineHeight() + 2 * 8 + 4;
    const int16_t left = width() - MARGIN - w, top = (bandH - h) / 2;
    if (draw) {
        drawRect(left, top, w, h, BLACK);
        drawRect(left + 1, top + 1, w - 2, h - 2, BLACK);
        printBold(left + w / 2, top + 2 + 8, text, CENTER);
    }
    return left;
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
