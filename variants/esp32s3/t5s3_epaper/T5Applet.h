#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

Base for the T5-only InkHUD applets.
Draws and hit-tests the six-destination nav bar: HOME · MSGS · NODES · MAP · MENU · APPS

*/

#pragma once

#include "configuration.h"

#include "graphics/niche/InkHUD/Applet.h"

namespace NicheGraphics::InkHUD
{

class T5Applet : public Applet
{
  public:
    T5Applet() { hideFromMenu = true; } // T5 screens stay active via T5HomeApplet::begin(), so the menu can't toggle them

    static constexpr uint16_t NAV_H = 88;
    static constexpr uint16_t CONSOLE_SLOT_W = 150; // Console: trailing mode slot, outside the six nav cells
    static constexpr uint16_t SIGNAL_W = 41;        // Four 8 px bars, 3 px apart
    static constexpr uint16_t SIGNAL_H = 22;
    static constexpr int16_t MARGIN = 16;       // The frozen side gutter, clear of the panel's rejected margin
    static constexpr int16_t CONSOLE_BAND = 63; // Console header band, above the first rule
    static constexpr const char *DASH = "\x97"; // Win-1253 em dash: the value we cannot supply

    static int8_t indexOf(const char *name); // Index in InkHUD::userApplets by registered name, or -1

    void onForeground() override; // Hides the shared battery icon: every T5 screen draws its own battery status

  protected:
    enum Destination : uint8_t { HOME, MSGS, NODES, MAP, MENU, APPS };

    // The nav cell this screen belongs to, drawn inverted. A deeper screen reports the destination it
    // was opened from, so the bar doesn't move on the way down: Node detail sits under NODES.
    virtual Destination currentDestination() = 0;

    bool isConsole() { return width() > height(); }
    uint16_t navWidth() { return isConsole() ? width() - CONSOLE_SLOT_W : width(); }
    int16_t navTop() { return height() - NAV_H; }

    void drawNav();
    bool handleNavTap(uint16_t x, uint16_t y); // True if the tap hit one of the six cells
    void printBold(int16_t x, int16_t y, const std::string &text, HorizontalAlignment ha = LEFT); // Top-aligned
    void drawSignal(int16_t left, int16_t bottom, SignalStrength signal); // Four bars, filled to the rating
    // Boxed label at the right edge of a header band, centred in it. Returns its left edge, so a slot
    // that is only sometimes filled still reserves its width
    int16_t boxedLabel(const char *text, int16_t bandH, bool draw = true);
    void strikeThrough(int16_t left, int16_t centerY, uint16_t w); // The 2 px unavailable rule, inset 14 px

    static bool heardRecently(const meshtastic_NodeInfoLite *node);
    static std::string hopsString(uint8_t hops);   // "direct", "1 hop", "3 hops"
    static std::string sinceString(uint32_t secs); // "now", "5 min", "3 h", "2 d"
    static std::string agoString(uint32_t secs);   // "now", "5 min ago", ...
    static std::string join(const std::string &a, const std::string &b);
    static std::string powerLabel(bool console, bool withVoltage = true); // Four states, no FULL

    int8_t destinationApplet(Destination d); // Applet index a destination opens, or -1 if unavailable
    void openDestination(Destination d);     // Shows it, or does nothing at all when it has no T5 screen yet
};

} // namespace NicheGraphics::InkHUD

#endif
