#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

T5 Messages: the root of Messaging. Channels in index order, then the people you
have written to.

One applet, two compositions picked from the live dimensions (Messaging Final
02): Carry    a single paged list; a row opens the Carry conversation screen
  Console  a split - the same list on the left, the selected conversation's
thread on the right

*/

#pragma once

#include "configuration.h"

#include "./T5ThreadApplet.h"

namespace NicheGraphics::InkHUD
{

class T5MessagesApplet : public T5ThreadApplet, public T5Redrawable
{
  public:
    T5MessagesApplet() { hideFromAppSwitcher = true; } // Reached from the MSGS nav cell

    static bool open();               // Show the Messages root. False if it isn't available
    static bool open(const Convo &c); // ...with this conversation selected
                                      // (Console) or opened (Carry)

    void onForeground() override;
    void onRender(bool full) override;
    bool onTouchPoint(uint16_t x, uint16_t y, bool longPress) override;
    void onNavUp() override;
    void onNavDown() override;
    void onActivate() override;
    void onDeactivate() override;

    void onMessagingChanged() override;

  private:
    Destination currentDestination() override { return MSGS; }

    // One line of the Carry list: a section label, a conversation, or the frozen
    // "no DMs yet" box
    struct Item {
        enum Kind : uint8_t { LABEL, ROW, EMPTY_DM } kind = ROW;
        Convo convo;
        const char *label = nullptr; // LABEL only
        uint16_t count = 0;          // LABEL only: the number to its right
        bool channelSection = false; // LABEL only: which section it opens, for the CONTINUED suffix
    };

    // The frozen dashed box: a title line and a wrapped hint. Text, not a target.
    static constexpr uint16_t EMPTY_DM_H = 160;

    std::vector<Item> buildItems();
    uint16_t itemHeight(const Item &item);

    void renderCarry();
    void renderConsole();
    void drawHeader(uint16_t unread, uint16_t conversations, uint8_t page, uint8_t pages);
    void drawRow(int16_t top, uint16_t w, const Convo &c, bool console, bool selected);
    void drawBadge(int16_t right, int16_t top, const Convo &c, bool suppress);
    void drawEmptyDMBox(int16_t left, int16_t top, uint16_t w);
    void drawPager(uint8_t page, uint8_t pages);

    // Which item the Carry list starts on, and the first item of each page,
    // recomputed on every render because the rows are measured, not counted
    uint16_t firstItem = 0;
    std::vector<uint16_t> pageStarts;
    std::vector<Item> shown;     // Items on screen, top to bottom: what a row tap opens
    std::vector<int16_t> shownY; // Their tops, so a tap maps back without re-measuring

    bool pagerShown = false;
    int16_t composeLeft = 0; // Left edge of the COMPOSE target, set when the header is drawn

    // Console: the selection lives in the model, so a mode change carries it.
    // These are the rows on screen, where the list starts, and the thread the
    // pane is showing.
    Thread thread;
    uint16_t firstRow = 0;
    std::vector<Convo> listRows;

    void selectConsoleRow(const Convo &c);
    void ensureSelection();
};

} // namespace NicheGraphics::InkHUD

#endif
