#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5Nav.h"
#include "./T5Applet.h"
#include "./T5MenuApplet.h"

#include "graphics/niche/InkHUD/InkHUD.h"

using namespace NicheGraphics;

bool InkHUD::t5IsConsole(const Applet &a)
{
    return a.width() > a.height();
}

uint16_t InkHUD::t5NavWidth(const Applet &a)
{
    return t5IsConsole(a) ? a.width() - T5_CONSOLE_SLOT_W : a.width();
}

int16_t InkHUD::t5NavTop(const Applet &a)
{
    return a.height() - T5_NAV_H;
}

// All six destinations are native user applets, MENU included: it holds slot 8 of sixteen, in place
// of the shared Favorites Map the native Map retired (Menu Settings 14)
int8_t InkHUD::t5DestApplet(T5Dest d)
{
    const char *name = d == T5_HOME    ? "Home"
                       : d == T5_NODES ? "Nodes"
                       : d == T5_MSGS  ? "Messages"
                       : d == T5_MAP   ? "Map"
                       : d == T5_MENU  ? T5MenuApplet::APPLET_NAME
                       : d == T5_APPS  ? "Apps"
                                       : nullptr;
    if (!name)
        return -1;
    const int8_t i = T5Applet::indexOf(name);
    return (i >= 0 && InkHUD::getInstance()->userApplets[i]->isActive()) ? i : -1;
}

bool InkHUD::t5DestAvailable(T5Dest d)
{
    return t5DestApplet(d) >= 0;
}

// An unavailable destination, and one already on screen, both ask for no update at all: nothing flickers
void InkHUD::t5OpenDest(T5Dest d)
{
    const int8_t target = t5DestApplet(d);
    if (target < 0)
        return;
    if (InkHUD::getInstance()->userApplets[target]->isForeground()) {
        // MENU while already in the menu is the one destination that still does something: it returns
        // to the root in Carry, and to the landing tile in Console (Menu Settings 14, touch regions)
        if (d == T5_MENU)
            T5MenuApplet::open();
        return; // Already on screen: nothing flickers
    }
    if (d == T5_MENU) {
        T5MenuApplet::open(); // Picks the landing page, then takes the same showApplet path
        return;
    }
    InkHUD::getInstance()->showApplet(target, Drivers::EInk::UpdateTypes::UNSPECIFIED, true);
}

bool InkHUD::t5HandleNavTap(Applet &a, uint16_t x, uint16_t y)
{
    const uint16_t navW = t5NavWidth(a);
    if (y < t5NavTop(a) || x >= navW)
        return false;
    t5OpenDest((T5Dest)(x * 6 / navW));
    return true;
}

// The one unavailable treatment for a nav cell: a 2 px rule through the label, inset 14 px (Redesign 8.4)
void InkHUD::t5StrikeThrough(Applet &a, int16_t left, int16_t centerY, uint16_t w)
{
    a.fillRect(left + 14, centerY - 1, w - 28, 2, BLACK);
}

// A filled triangle, 20 px wide and 10 tall, centred on the point given
void InkHUD::t5DrawTriangle(Applet &a, int16_t centerX, int16_t centerY, bool up)
{
    constexpr int16_t halfW = 10, h = 10;
    for (int16_t row = 0; row < h; row++) {
        const int16_t half = halfW - halfW * row / h;
        const int16_t y = up ? centerY + h / 2 - row : centerY - h / 2 + row;
        a.fillRect(centerX - half, y, 2 * half + 1, 1, BLACK);
    }
}

void InkHUD::t5DottedRect(Applet &a, int16_t left, int16_t top, uint16_t w, uint16_t h)
{
    for (int16_t x = left; x < left + (int16_t)w; x += 4) {
        a.fillRect(x, top, 2, 1, BLACK);
        a.fillRect(x, top + (int16_t)h - 1, 2, 1, BLACK);
    }
    for (int16_t y = top; y < top + (int16_t)h; y += 4) {
        a.fillRect(left, y, 1, 2, BLACK);
        a.fillRect(left + (int16_t)w - 1, y, 1, 2, BLACK);
    }
}

#endif
