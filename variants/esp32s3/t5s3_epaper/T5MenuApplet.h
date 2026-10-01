#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

MENU: the native T5 settings surface, and the fifth of the six destinations (Menu Settings 01, 12, 13).

A user applet, and the sixth T5 screen, exactly as Home, Nodes, Messages, Map and Apps are. It owns
registration slot 8, which used to register the shared Favorites Map - retired on T5 by the native
Map - so the T5 build still registers sixteen user applets and no later index moved. The menu is a
destination, not an overlay, and this is the ownership class that says so: the screen it replaces is
sent to the background by the ordinary showApplet path, so it can neither draw underneath the menu
nor ask the panel to refresh while the menu is being read. Nothing about the frozen UI changes: the
menu keeps the nav bar on the root and on every category page, and the pickers and confirmations are
task layers that replace it.

Being the foreground screen is the whole of "the menu is open": isForeground() is that fact, and
there is no second copy of it. A task layer or a system-layer card draws over the menu without
dismissing it, the display timeout freezes the image without dismissing it, and on wake the same
section and selection are still here (Menu Settings 07 of the brief, 30).

Carry gives each page the list between the header and the nav. Console puts the eight categories in a
2 x 4 tile grid on the left and the current page in the pane on the right; the landing category is
remembered for the session only, never persisted (decision 5).

*/

#pragma once

#include "configuration.h"

#include "./T5Applet.h"
#include "./T5Settings.h"

#include "concurrency/OSThread.h"

namespace NicheGraphics::InkHUD
{

class T5MenuApplet : public T5Applet, public concurrency::OSThread
{
  public:
    T5MenuApplet();

    // The registered name, and the one place it is spelled. Slot 8 of sixteen.
    static constexpr const char *APPLET_NAME = "T5Menu";

    static T5MenuApplet *get();  // Null before registration
    static bool isOpen();        // The menu is the foreground screen
    static void open();          // From the MENU nav cell, or from a frozen deep link
    static void onModeChanged(); // The page survives the rotate; only its number is recomputed

    // The task layer hands its result back here
    static void pickChosen(T5Act which, int32_t value);
    static void pickCancelled();
    static void confirmed(T5Act which, int32_t value);
    static void confirmCancelled();
    static void nameEdited(bool shortName, const std::string &utf8);

    void onForeground() override;
    void onBackground() override;
    void onRender(bool full) override;
    bool onTouchPoint(uint16_t x, uint16_t y, bool longPress) override;
    void onNavUp() override;
    void onNavDown() override;

  protected:
    int32_t runOnce() override; // The frozen LIVE rows refresh at most once a minute, only when visible

  private:
    Destination currentDestination() override { return MENU; }

    // ---- state (RAM only: nothing the menu remembers is written to flash) ----
    T5Page page = T5Page::ROOT;
    uint8_t channelIndex = 0;     // Which channel the CHANNEL page is showing
    uint8_t pageIndex = 0;        // Which page of a list that does not fit
    uint8_t consoleCategory = 4;  // First open after boot lands on DISPLAY (decision 5)
    T5Act errorRow = T5Act::NONE; // The row whose last write did not reach flash: NOT SAVED
    uint32_t liveShownAtMs = 0;   // When the LIVE values were last drawn
    // A fresh entry from the MENU cell. The page it lands on depends on the mode, which is the shape
    // of the tile, so it is resolved on the next render rather than at the tap: a layout rebuild
    // backgrounds and re-foregrounds every screen, and must not move the page being read.
    bool landingPending = false;

    // Whether this page shows a header Back, which is also the only thing that makes the Back target
    // live. Console has no root page - the tile grid is the root - so a category page has neither.
    bool hasBack() const;
    void goTo(T5Page p, uint8_t channel = 0);
    void back();
    void runRow(const T5Row &row, int16_t choiceCell); // What a tap on a row does
    void applyNow(T5Act which, int32_t value);
    bool coveredByTask() const; // A menu task, or one of the Phase 4 / 6 tasks, owns the screen

    // ---- layout ----
    // Frozen geometry, per mode, never derived from the other one (Menu Settings 14)
    struct Metrics {
        int16_t headerH, listTop, listBottom, sectionH, rowH, rowHintH, categoryH, infoH, actionSlot, pagerH;
        int16_t paneLeft, paneRight;
    };
    Metrics metrics() const;

    int16_t rowHeight(const T5Row &row, const Metrics &m);
    void drawHeader(const Metrics &m);
    void drawConsoleTiles(const Metrics &m);
    void drawPager(const Metrics &m, uint8_t pages);
    void drawRow(const T5Row &row, int16_t top, int16_t height, const Metrics &m);
    // Both return their left edge, so the label beside them knows where it has to stop
    int16_t drawChip(int16_t right, int16_t centerY, const char *label, bool filled);
    int16_t drawReason(int16_t right, int16_t centerY, const std::string &reason);

    // Rows on screen and where each one sits, rebuilt on every render so a tap can only ever hit
    // what the user is actually looking at
    std::vector<T5Row> rowsShown;
    std::vector<int16_t> rowTops;
    std::vector<int16_t> rowHeights;
    bool pagerShown = false;
    uint8_t pageCount = 1;

    void turnPage(int8_t delta);
};

} // namespace NicheGraphics::InkHUD

#endif
