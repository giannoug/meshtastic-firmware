#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

Base for the T5-only InkHUD applets.
Draws and hit-tests the six-destination nav bar: HOME · MSGS · NODES · MAP · MENU · APPS

*/

#pragma once

#include "configuration.h"

#include "./T5Nav.h"
#include "./T5Text.h"

#include "graphics/niche/InkHUD/Applet.h"

namespace NicheGraphics::InkHUD
{

class T5Applet : public T5NavDraw<Applet>
{
  public:
    T5Applet() { hideFromMenu = true; } // T5 screens stay active via T5HomeApplet::begin(), so the menu can't toggle them

    // Geometry shared with the T5 system applets, which cannot inherit this class. T5Nav.h owns the
    // values; these keep the familiar names for the screens that already use them.
    static constexpr uint16_t NAV_H = T5_NAV_H;
    static constexpr uint16_t CONSOLE_SLOT_W = T5_CONSOLE_SLOT_W;
    static constexpr uint16_t SIGNAL_W = 41; // Four 8 px bars, 3 px apart
    static constexpr uint16_t SIGNAL_H = 22;
    static constexpr int16_t MARGIN = T5_MARGIN;
    static constexpr int16_t CONSOLE_BAND = T5_CONSOLE_BAND;
    static constexpr const char *DASH = "\x97";     // Win-1253 em dash: the value we cannot supply
    static constexpr const char *SEP = "\xB7";      // Win-1253 middle dot: the frozen separator
    static constexpr const char *BACK = "\x8B";     // Win-1253 single left angle quote: parse() has no U+2039
    static constexpr const char *FWD = "\x9B";      // ...and no U+203A
    static constexpr const char *ELLIPSIS = "\x85"; // Win-1253 horizontal ellipsis (V14: present in all three fonts)

    using Convo = T5MessageModel::Convo;

    static int8_t indexOf(const char *name); // Index in InkHUD::userApplets by
                                             // registered name, or -1

    // "SHORT \x97 Long name", or whichever one we have. Shared with the T5 system
    // layer.
    static std::string join(const std::string &a, const std::string &b);

    static std::string hopsString(uint8_t hops); // "direct", "1 hop", "3 hops". Shared with the composer

    static std::string powerLabel(bool console, bool withVoltage = true); // Four states, no FULL

    void onForeground() override; // Hides the shared battery icon: every T5
                                  // screen draws its own battery status

  protected:
    // The shared nav enum under the name every T5 screen already uses
    using Destination = T5Dest;
    static constexpr Destination HOME = T5_HOME, MSGS = T5_MSGS, NODES = T5_NODES, MAP = T5_MAP, MENU = T5_MENU, APPS = T5_APPS;

    // The nav cell this screen belongs to, drawn inverted. A deeper screen reports the destination it
    // was opened from, so the bar doesn't move on the way down: Node detail sits under NODES.
    virtual Destination currentDestination() = 0;

    bool isConsole() { return t5IsConsole(*this); }
    uint16_t navWidth() { return t5NavWidth(*this); }
    int16_t navTop() { return t5NavTop(*this); }

    // Thin forwarders, so a T5 screen and a T5 system applet draw the same bar from one
    // implementation. printBold and boxedLabel are inherited from the mixin.
    void drawNav() { drawNavBar(currentDestination()); }
    bool handleNavTap(uint16_t x, uint16_t y) { return t5HandleNavTap(*this, x, y); } // True if a cell was hit
    void drawSignal(int16_t left, int16_t bottom, SignalStrength signal);             // Four bars, filled to the rating
    void strikeThrough(int16_t left, int16_t centerY, uint16_t w) { t5StrikeThrough(*this, left, centerY, w); }

    // The one-part Carry sub-page title (Panel Fit D12): "‹ PAGE" medium bold at baseline 46, the status small
    // at baseline 39 against the right margin. Short of room "· CHG" drops from the status, then the title
    // elides. Returns the right edge of the Back target: the title's + 12, never under 200
    int16_t drawSubPageTitle(const std::string &page, std::string status);

    // The frozen paging triangles. No Win-1253 glyph exists for either, and a
    // large font for two shapes would be worse than drawing them (V14) - as the
    // mode control already draws its arrows.
    void drawTriangle(int16_t centerX, int16_t centerY, bool up) { t5DrawTriangle(*this, centerX, centerY, up); }

    static bool heardRecently(const meshtastic_NodeInfoLite *node);
    static std::string sinceString(uint32_t secs); // "now", "5 min", "3 h", "2 d"
    static std::string agoString(uint32_t secs);   // "now", "5 min ago", ...

    int8_t destinationApplet(Destination d) { return t5DestApplet(d); } // Applet index, or -1 if unavailable
    void openDestination(Destination d) { t5OpenDest(d); }              // Shows it, or nothing at all without a T5 screen
};

} // namespace NicheGraphics::InkHUD

#endif
