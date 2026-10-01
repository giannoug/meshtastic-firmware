#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

TRACE WHICH NODE?: the Trace Route target picker (Apps T2, T7, K7).

A task, not a screen, for the same reasons the composer is one: the frozen picker replaces the nav
bar with CANCEL, has no header Back, and one Home press cancels it to the tool underneath rather
than going Home (Apps 05). Only a system applet owns input that way - and a system applet costs none
of InkHUD's 16 user-applet slots, so the whole of Apps still fits in the one slot T5AppsApplet holds.

It is deliberately not the Nodes list in pick mode. That mode exists for the recipient picker and is
bound to the composer: it keeps the Nodes header, the Nodes sort and the Nodes rows. The frozen trace
picker asks a question in its header, sorts favourites first, and ends each row with a hops badge, so
it is its own list - as the frozen class handoff has it. What it does reuse is the row *look* of the
recipient picker, and the shared naming helpers, rather than re-deriving either.

*/

#pragma once

#include "configuration.h"

#include "./T5Applet.h"

#include "graphics/niche/InkHUD/SystemApplet.h"

namespace NicheGraphics::InkHUD
{

class T5TracePickTask : public T5NavDraw<SystemApplet>
{
  public:
    T5TracePickTask();

    static void begin();
    static T5TracePickTask *get(); // Null before begin()

    static bool isOpen();
    static void open();  // From CHOOSE NODE, or from tapping the target card
    static void close(); // Without a pick: the tool is left exactly as it was

    void onRender(bool full) override;
    bool onTouchPoint(uint16_t x, uint16_t y, bool longPress) override;
    void onExitShort() override; // Home x1 cancels to the tool; the next press goes Home
    void onNavUp() override;
    void onNavDown() override;
    void onModeChanged(); // Our tile spans the screen, so its region follows the orientation

  private:
    void reconcile();
    void page(int8_t delta);

    // NodeDB, minus ourselves and minus ignored nodes: favourites first, then last heard, newest
    // first. "? HOPS" stands in wherever hops_away is unknown (Apps T2)
    std::vector<NodeNum> candidates() const;
    void drawRow(int16_t left, int16_t top, uint16_t w, NodeNum num, bool console);
    void drawCancel();

    bool open_ = false;
    uint16_t firstRow = 0;          // List index of the first row on screen
    std::vector<NodeNum> rowsShown; // Rows on screen, in draw order
    std::vector<int16_t> rowTops;
    std::vector<int16_t> rowLefts; // Console draws two columns, column-major
    std::vector<uint16_t> rowWidths;
    uint8_t rowsPerPage() const;
};

} // namespace NicheGraphics::InkHUD

#endif
