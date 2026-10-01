#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

The six-destination nav bar and the boxed controls every T5 screen draws, shared by the T5 screens and
the T5 system applets.

Two kinds of helper here, split by what each one needs:

  - Free functions, for everything expressible through the public drawing API, as T5Text already does:
    nav geometry, hit-testing, which destination is live, and the three shapes made of plain rects.
  - T5NavDraw<Base>, a mixin, for the three that need Applet's protected text API. A free function
    cannot reach setFont / printThick / getTextWidth, and re-deriving them from Adafruit_GFX would be
    a second nav implementation. Deriving from the caller's own base reaches them with no change to
    shared InkHUD code and no second inheritance of Applet: T5Applet is T5NavDraw<Applet>, and a
    system applet is T5NavDraw<SystemApplet>.

*/

#pragma once

#include "configuration.h"

#include "graphics/niche/InkHUD/Applet.h"

namespace NicheGraphics::InkHUD
{

// The six nav cells, left to right. T5Applet exposes these under the names its screens already use.
enum T5Dest : uint8_t { T5_HOME, T5_MSGS, T5_NODES, T5_MAP, T5_MENU, T5_APPS };

constexpr uint16_t T5_NAV_H = 88;
constexpr uint16_t T5_CONSOLE_SLOT_W = 150; // Console: trailing mode slot, outside the six nav cells
constexpr int16_t T5_MARGIN = 16;           // The frozen side gutter, clear of the panel's rejected margin
constexpr int16_t T5_CONSOLE_BAND = 63;     // Console header band, above the first rule

bool t5IsConsole(const Applet &a);
uint16_t t5NavWidth(const Applet &a);
int16_t t5NavTop(const Applet &a);

// Index in InkHUD::userApplets a destination opens, or -1 when it has no T5 screen. MENU is always
// -1: the native menu is a system applet and costs no user-applet slot (Menu Settings 14)
int8_t t5DestApplet(T5Dest d);
bool t5DestAvailable(T5Dest d); // Whether the cell is live, which for MENU the menu itself answers
void t5OpenDest(T5Dest d);      // Shows it, or does nothing at all when it has no T5 screen
// Hit-tests the six cells only: the Console mode slot never resolves to a cell. An unavailable cell
// still consumes its tap, so the button fallback never sees it (Redesign 8.4)
bool t5HandleNavTap(Applet &a, uint16_t x, uint16_t y);

void t5StrikeThrough(Applet &a, int16_t left, int16_t centerY, uint16_t w); // The 2 px rule, inset 14 px
// The frozen paging triangles. No Win-1253 glyph exists for either, and a large font for two shapes
// would be worse than drawing them (V14)
void t5DrawTriangle(Applet &a, int16_t centerX, int16_t centerY, bool up);
// The frozen "unavailable" outline: 2 px dashes on a 4 px pitch, holding the reason
void t5DottedRect(Applet &a, int16_t left, int16_t top, uint16_t w, uint16_t h);

// The three that need Applet's protected text API. Defined here because they are a template; the
// bodies are the ones T5Applet had, unchanged.
template <class Base> class T5NavDraw : public Base
{
  protected:
    // Six cells across t5NavWidth(): the current one inverted, unavailable ones struck through
    void drawNavBar(T5Dest current)
    {
        static constexpr const char *labels[] = {"HOME", "MSGS", "NODES", "MAP", "MENU", "APPS"};
        const int16_t top = t5NavTop(*this);
        const uint16_t navW = t5NavWidth(*this);

        this->drawLine(0, top - 1, this->width() - 1, top - 1, BLACK);
        this->setFont(Applet::fontSmall);
        for (uint8_t i = 0; i < 6; i++) {
            const int16_t left = i * navW / 6;
            const int16_t cellW = (i + 1) * navW / 6 - left;
            const int16_t centerX = left + cellW / 2;
            const int16_t centerY = top + T5_NAV_H / 2;

            if (i == current) {
                this->fillRect(left, top, cellW, T5_NAV_H, BLACK);
                this->setTextColor(WHITE);
                this->printThick(centerX, centerY, labels[i], 2, 1);
                this->setTextColor(BLACK);
                continue;
            }

            if (i > 0)
                this->drawLine(left, top, left, this->height() - 1, BLACK);
            this->printAt(centerX, centerY, labels[i], Applet::CENTER, Applet::MIDDLE);
            if (!t5DestAvailable((T5Dest)i))
                t5StrikeThrough(*this, left, centerY, cellW);
        }

        if (t5IsConsole(*this))
            this->fillRect(navW, top, 2, T5_NAV_H, BLACK); // Rule between the six cells and the mode slot
    }

    // Top-aligned, with the two-pass thickening every T5 screen uses for bold
    void printBold(int16_t x, int16_t y, const std::string &text, Applet::HorizontalAlignment ha = Applet::LEFT)
    {
        const int16_t w = this->getTextWidth(text);
        const int16_t centerX = ha == Applet::LEFT ? x + w / 2 : ha == Applet::RIGHT ? x - w / 2 : x;
        this->printThick(centerX, y + this->getFont().lineHeight() / 2, text, 2, 1);
    }

    // A 2 px box with the frozen 14 / 8 px padding, at the right edge of a header band. Returns its
    // left edge, so a slot that is only sometimes filled still reserves its width
    int16_t boxedLabel(const char *text, int16_t bandH, bool draw = true)
    {
        this->setFont(Applet::fontSmall);
        const int16_t w = this->getTextWidth(text) + 2 * 14 + 4, h = Applet::fontSmall.lineHeight() + 2 * 8 + 4;
        const int16_t left = this->width() - T5_MARGIN - w, top = (bandH - h) / 2;
        if (draw) {
            this->drawRect(left, top, w, h, BLACK);
            this->drawRect(left + 1, top + 1, w - 2, h - 2, BLACK);
            printBold(left + w / 2, top + 2 + 8, text, Applet::CENTER);
        }
        return left;
    }
};

} // namespace NicheGraphics::InkHUD

#endif
