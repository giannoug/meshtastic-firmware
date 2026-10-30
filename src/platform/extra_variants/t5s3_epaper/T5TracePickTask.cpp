#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5TracePickTask.h"
#include "./T5AppsApplet.h"
#include "./T5Text.h"

#include "gps/RTC.h"
#include "graphics/niche/InkHUD/InkHUD.h"
#include "graphics/niche/InkHUD/Tile.h"
#include "mesh/NodeDB.h"
#include <algorithm>

using namespace NicheGraphics;

namespace
{
// Frozen picker geometry (Apps T2, K7). Carry keeps the recipient picker's row height; Console packs
// two columns of shorter rows, filled column by column
constexpr int16_t CARRY_HEADER_H = 100;
constexpr int16_t CONSOLE_HEADER_H = 63;
constexpr int16_t CARRY_ROW_H = 84;
constexpr int16_t CONSOLE_ROW_H = 72;
constexpr int16_t BUTTON_BAND = 88; // Where the nav would be: the task puts CANCEL there instead
constexpr int16_t PAGER_H = 64;
constexpr uint8_t CONSOLE_COLUMNS = 2;

InkHUD::T5TracePickTask *instance = nullptr;
} // namespace

InkHUD::T5TracePickTask::T5TracePickTask()
{
    // A temporary layer that replaces the whole screen, so it owns its whole region: it clears that
    // region itself rather than letting the Renderer do it, which is the only way the screen
    // underneath cannot show through a gap on a frame that re-renders everything - a wake from the
    // display timeout, a Carry / Console switch, or the applet below redrawing itself. Paired with
    // alwaysRender while open, so such a frame always redraws us last. The contract T5SystemLayer
    // already uses for its own full-screen states.
    clearsOwnRegion = true;
    // A task owns its own full-screen tile, as the composer does: nothing else places one for a
    // system applet, and begin() sets its region straight after
    (new Tile)->assignApplet(this);
}

void InkHUD::T5TracePickTask::begin()
{
    assert(!instance);
    instance = new T5TracePickTask;
    instance->name = "T5TracePick";
    instance->activate();

    // Before the T5 system layer, for the same reason the composer is: our input claim is checked
    // first, but the layer still draws its card over us and still takes the press when it does
    InkHUD::getInstance()->systemApplets.push_back(instance);
    instance->getTile()->setRegion(0, 0, InkHUD::getInstance()->width(), InkHUD::getInstance()->height());
}

InkHUD::T5TracePickTask *InkHUD::T5TracePickTask::get()
{
    return instance;
}

bool InkHUD::T5TracePickTask::isOpen()
{
    return instance && instance->open_;
}

void InkHUD::T5TracePickTask::open()
{
    if (!instance)
        return;
    instance->open_ = true;
    instance->firstRow = 0;
    instance->reconcile();
    instance->requestUpdate();
}

void InkHUD::T5TracePickTask::close()
{
    if (!instance || !instance->open_)
        return;
    instance->open_ = false;
    instance->reconcile();
    // The tool underneath is exactly as it was; it only has to be drawn again
    T5AppsApplet::pickCancelled();
}

void InkHUD::T5TracePickTask::reconcile()
{
    handleInput = open_; // While up, one Home press peels us rather than going Home (Apps 05)
    // Drawn in every frame while we are up, so a redraw of the tool underneath cannot erase us, and
    // cleared again on close so a closed task costs nothing
    alwaysRender = open_;
    if (open_)
        bringToForeground();
    else
        sendToBackground();
}

// Home x1 cancels to the tool underneath. The next press is an ordinary Home
void InkHUD::T5TracePickTask::onExitShort()
{
    close();
}

void InkHUD::T5TracePickTask::onModeChanged()
{
    getTile()->setRegion(0, 0, InkHUD::getInstance()->width(), InkHUD::getInstance()->height());
    // The picker stays open across an orientation change, on the page holding the same first node
    // (Apps 11). Column-major Console pages and single-column Carry pages both start from firstRow
}

// NodeDB, minus ourselves and minus nodes the user has chosen to ignore: a trace to either is not
// something the frozen flow offers. Favourites first, then last heard, newest first (Apps T2)
std::vector<NodeNum> InkHUD::T5TracePickTask::candidates() const
{
    std::vector<meshtastic_NodeInfoLite *> nodes;
    const NodeNum me = nodeDB->getNodeNum();
    for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
        meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!n || n->num == me || n->num == 0 || nodeInfoLiteIsIgnored(n))
            continue;
        nodes.push_back(n);
    }
    std::stable_sort(nodes.begin(), nodes.end(), [](const meshtastic_NodeInfoLite *a, const meshtastic_NodeInfoLite *b) {
        const bool fa = nodeInfoLiteIsFavorite(a), fb = nodeInfoLiteIsFavorite(b);
        if (fa != fb)
            return fa;
        return a->last_heard > b->last_heard;
    });
    std::vector<NodeNum> out;
    out.reserve(nodes.size());
    for (const meshtastic_NodeInfoLite *n : nodes)
        out.push_back(n->num);
    return out;
}

uint8_t InkHUD::T5TracePickTask::rowsPerPage() const
{
    const bool console = width() > height();
    const int16_t top = console ? CONSOLE_HEADER_H + 1 : CARRY_HEADER_H;
    const int16_t room = height() - BUTTON_BAND - PAGER_H - top;
    const int16_t rowH = console ? CONSOLE_ROW_H : CARRY_ROW_H;
    const int16_t perColumn = room / rowH;
    return (uint8_t)std::max<int16_t>(1, perColumn * (console ? CONSOLE_COLUMNS : 1));
}

void InkHUD::T5TracePickTask::page(int8_t delta)
{
    const std::vector<NodeNum> all = candidates();
    const uint8_t per = rowsPerPage();
    const int32_t next = (int32_t)firstRow + delta * (int32_t)per;
    if (next < 0 || next >= (int32_t)all.size())
        return;
    firstRow = (uint16_t)next;
    requestUpdate();
}

void InkHUD::T5TracePickTask::onNavUp()
{
    if (open_)
        page(-1);
}

void InkHUD::T5TracePickTask::onNavDown()
{
    if (open_)
        page(1);
}

// The recipient picker's row look, with a hops badge where that one has a chevron: this row picks a
// target rather than opening anything (Apps T2)
void InkHUD::T5TracePickTask::drawRow(int16_t left, int16_t top, uint16_t w, NodeNum num, bool console)
{
    const int16_t rowH = console ? CONSOLE_ROW_H : CARRY_ROW_H;
    const int16_t right = left + (int16_t)w;
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(num);

    // The hops badge: what the NodeDB believes, so the user knows roughly what to expect
    std::string hops = "? HOPS";
    if (node && node->has_hops_away)
        hops = node->hops_away == 0 ? "DIRECT" : to_string(node->hops_away) + " HOPS";
    setFont(Applet::fontSmall);
    const int16_t hopsW = getTextWidth(hops);
    printAt(right - T5Applet::MARGIN, top + rowH / 2, hops, RIGHT, MIDDLE);
    const int16_t textRight = right - T5Applet::MARGIN - hopsW - 16;

    // The star the frozen mockup puts before a favourite's name has no glyph in the T5 fonts, so the
    // favourite is named in the detail line instead - which the frozen row already does
    std::string sub = t5NodeTag(num);
    if (nodeInfoLiteIsFavorite(node))
        sub = T5Applet::join(sub, "favourite");
    else if (node && node->last_heard) {
        const std::string when = t5ListTime(node->last_heard, false);
        if (!when.empty())
            sub = T5Applet::join(sub, when);
    }

    setFont(Applet::fontMedium);
    const int16_t nameLeft = left + T5Applet::MARGIN;
    const int16_t textW = std::max<int16_t>(0, textRight - nameLeft);
    setCrop(nameLeft, top, textW, rowH);
    printBold(nameLeft, top + (console ? 6 : 12), t5Elide(*this, t5NodeTitle(num), textW > 2 ? textW - 2 : 0)); // 2 px bold
    resetCrop();

    setFont(Applet::fontSmall);
    setCrop(nameLeft, top, textW, rowH);
    printAt(nameLeft, top + (console ? 6 : 12) + Applet::fontMedium.lineHeight() + 4, t5Elide(*this, sub, textW));
    resetCrop();

    drawLine(left, top + rowH - 1, right - 1, top + rowH - 1, BLACK);
}

// CANCEL stands where the nav would be: a task replaces navigation rather than sitting inside it

void InkHUD::T5TracePickTask::drawCancel()
{
    const int16_t top = height() - BUTTON_BAND;
    drawLine(0, top, width() - 1, top, BLACK);
    setFont(Applet::fontSmall);
    const int16_t w = getTextWidth("CANCEL") + 2 * 16 + 4, h = Applet::fontSmall.lineHeight() + 2 * 12 + 4;
    const int16_t left = (width() - w) / 2, boxTop = top + (BUTTON_BAND - h) / 2;
    drawRect(left, boxTop, w, h, BLACK);
    drawRect(left + 1, boxTop + 1, w - 2, h - 2, BLACK);
    printBold(left + w / 2, boxTop + (h - Applet::fontSmall.lineHeight()) / 2, "CANCEL", CENTER);
}

void InkHUD::T5TracePickTask::onRender(bool full)
{
    (void)full;
    // alwaysRender puts us in every frame, including the one that adds the lock band when the display
    // timeout starts. While that hold is in force nothing underneath us is being redrawn either, so
    // there is nothing to cover: leave the frozen frame exactly as it is and let T5SystemLayer draw
    // the band over it, which is the one surface allowed to change a held image (Lock and Power 01).
    if (inkhud->isRenderHeld())
        return;

    // Opaque: this layer replaces the screen, and clearsOwnRegion means nothing else blanks it
    fillRect(0, 0, width(), height(), WHITE);
    const bool console = width() > height();
    const int16_t bandH = console ? CONSOLE_HEADER_H : CARRY_HEADER_H;
    rowsShown.clear();
    rowTops.clear();
    rowLefts.clear();
    rowWidths.clear();

    // The header is the question, and there is no Back: a task is left by CANCEL or by Home
    setFont(Applet::fontLarge);
    printBold(T5Applet::MARGIN, console ? (bandH - Applet::fontLarge.lineHeight()) / 2 : 14, "TRACE WHICH NODE?");
    setFont(Applet::fontSmall);
    if (console)
        printAt(width() - T5Applet::MARGIN, (bandH - Applet::fontSmall.lineHeight()) / 2, "SELECT TARGET", RIGHT);
    else
        printAt(T5Applet::MARGIN, 66, "SELECT TARGET");
    drawLine(0, bandH, width() - 1, bandH, BLACK);

    const std::vector<NodeNum> all = candidates();
    if (all.empty()) {
        // T7: nothing has been heard yet, so there is nothing to retry either - CANCEL only
        int16_t y = bandH + 140;
        setFont(Applet::fontMedium);
        printBold(T5Applet::MARGIN, y, "No nodes yet");
        y += Applet::fontMedium.lineHeight() + 10;
        setFont(Applet::fontSmall);
        const std::string body = "Nodes appear once they are heard on the mesh. Check NODES later, or confirm your region "
                                 "and preset in MENU " +
                                 std::string(T5Applet::FWD) + " RADIO.";
        for (const std::string &line : t5WrapLines(*this, body, width() - 2 * T5Applet::MARGIN)) {
            printAt(T5Applet::MARGIN, y, line);
            y += Applet::fontSmall.lineHeight() + 2;
        }
        drawCancel();
        return;
    }

    const uint8_t per = rowsPerPage();
    if (firstRow >= all.size())
        firstRow = 0;
    const uint8_t pages = (uint8_t)((all.size() + per - 1) / per);
    const uint8_t page = (uint8_t)(firstRow / per);
    const bool paged = pages > 1;

    const int16_t top = bandH + 1;
    const int16_t rowH = console ? CONSOLE_ROW_H : CARRY_ROW_H;
    const int16_t perColumn = console ? per / CONSOLE_COLUMNS : per;
    const int16_t colW = console ? width() / CONSOLE_COLUMNS : width();

    for (uint8_t i = 0; i < per; i++) {
        const size_t index = firstRow + i;
        if (index >= all.size())
            break;
        // Column-major: the left column fills before the right one starts (Apps K7)
        const int16_t left = console ? (int16_t)(i / perColumn) * colW : 0;
        const int16_t rowTop = top + (console ? (int16_t)(i % perColumn) : (int16_t)i) * rowH;
        if (rowTop + rowH > height() - BUTTON_BAND - (paged ? PAGER_H : 0))
            continue;
        drawRow(left, rowTop, (uint16_t)colW, all[index], console);
        rowsShown.push_back(all[index]);
        rowTops.push_back(rowTop);
        rowLefts.push_back(left);
        rowWidths.push_back((uint16_t)colW);
    }
    if (console)
        drawLine(colW, top, colW, height() - BUTTON_BAND - 1, BLACK);

    if (paged) {
        const int16_t pagerTop = height() - BUTTON_BAND - PAGER_H;
        drawLine(0, pagerTop, width() - 1, pagerTop, BLACK);
        const int16_t cell = width() / 3;
        setFont(Applet::fontSmall);
        if (page > 0)
            printAt(cell / 2, pagerTop + PAGER_H / 2, std::string(T5Applet::BACK) + " PREV", CENTER, MIDDLE);
        printAt(width() / 2, pagerTop + PAGER_H / 2, to_string(page + 1) + " / " + to_string(pages), CENTER, MIDDLE);
        if (page + 1 < pages)
            printAt(width() - cell / 2, pagerTop + PAGER_H / 2, std::string("NEXT ") + T5Applet::FWD, CENTER, MIDDLE);
    }

    drawCancel();
}

bool InkHUD::T5TracePickTask::onTouchPoint(uint16_t x, uint16_t y, bool longPress)
{
    if (!open_)
        return false;
    if (longPress)
        return true;

    const int16_t tx = (int16_t)x - getTile()->getLeft();
    const int16_t ty = (int16_t)y - getTile()->getTop();

    // CANCEL returns to the tool with nothing changed
    if (ty >= height() - BUTTON_BAND) {
        close();
        return true;
    }

    for (size_t i = 0; i < rowsShown.size(); i++) {
        const int16_t rowH = (width() > height()) ? CONSOLE_ROW_H : CARRY_ROW_H;
        if (ty >= rowTops[i] && ty < rowTops[i] + rowH && tx >= rowLefts[i] && tx < rowLefts[i] + (int16_t)rowWidths[i]) {
            const NodeNum picked = rowsShown[i];
            open_ = false;
            reconcile();
            // Sets the target and returns to the tool: it does not start the trace (Apps T2)
            T5AppsApplet::targetPicked(picked);
            return true;
        }
    }

    const std::vector<NodeNum> all = candidates();
    const uint8_t per = rowsPerPage();
    if (all.size() > per) {
        const int16_t pagerTop = height() - BUTTON_BAND - PAGER_H;
        if (ty >= pagerTop) {
            const int16_t cell = width() / 3;
            if (tx < cell)
                page(-1);
            else if (tx >= 2 * cell)
                page(1);
            return true;
        }
    }

    return true; // A task consumes every tap inside it
}

#endif
