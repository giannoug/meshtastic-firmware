#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5NodeDetailApplet.h"
#include "./T5ComposeTask.h"
#include "./T5MapApplet.h"
#include "./T5NodesApplet.h"

#include "mesh/NodeDB.h"

#include <cmath>

using namespace NicheGraphics;

namespace
{
constexpr const char *BACK = "\x8B "; // Win-1253 single left angle quote: parse() has no remap for U+2039
constexpr int16_t PLOT_RADIUS = 64;   // One node gives direction, not scale: the marker sits at a fixed distance
} // namespace

bool InkHUD::T5NodeDetailApplet::open(NodeNum num, Destination from)
{
    InkHUD *hud = InkHUD::getInstance();
    const int8_t i = indexOf("Node Detail");
    if (i < 0 || !nodeDB->getMeshNode(num))
        return false;
    T5NodeDetailApplet *detail = static_cast<T5NodeDetailApplet *>(hud->userApplets[i]);
    detail->node = num;
    detail->origin = from;
    return hud->showApplet(i, EInk::UpdateTypes::UNSPECIFIED, true);
}

// Live while shown: the node's own packets change its last heard, signal and SNR / RSSI facts
bool InkHUD::T5NodeDetailApplet::wantPacket(const meshtastic_MeshPacket *p)
{
    return isForeground() && p->from == node;
}

ProcessMessage InkHUD::T5NodeDetailApplet::handleReceived(const meshtastic_MeshPacket &mp)
{
    (void)mp;
    requestUpdate();
    return ProcessMessage::CONTINUE;
}

void InkHUD::T5NodeDetailApplet::onRender(bool full)
{
    (void)full;
    const bool console = isConsole();
    const int16_t titleTop = console ? 4 : 14;

    meshtastic_NodeInfoLite *info = nodeDB->getMeshNode(node);
    if (!info) {
        // Gone from nodeDB, or reached by cycling applets before any node was opened: only the ways out
        setFont(fontLarge);
        printBold(MARGIN, titleTop, std::string(BACK) + "NODES");
        drawNav();
        return;
    }

    // Same reading of the node as its T5 Nodes row
    const int8_t nodesIndex = indexOf("Nodes");
    assert(nodesIndex >= 0);
    const T5NodesApplet::Row row = static_cast<T5NodesApplet *>(inkhud->userApplets[nodesIndex])->readRow(info);
    const std::string back = std::string(BACK) + row.shortName, id = hexifyNodeNum(node);

    // Header: the whole band is the Back target. Long name and id share the title's baseline
    setFont(fontLarge);
    printBold(MARGIN, titleTop, back);
    const int16_t nameTop = titleTop + fontLarge.heightAboveCursor() - fontMedium.heightAboveCursor();
    if (console) {
        // The id sits before the favourite chip's slot, which stays reserved when the chip isn't drawn
        setFont(fontSmall);
        const int16_t chipLeft = boxedLabel("FAVOURITE", CONSOLE_BAND, nodeInfoLiteIsFavorite(info));
        const int16_t idLeft = chipLeft - MARGIN - getTextWidth(id);
        printAt(idLeft, titleTop + fontLarge.heightAboveCursor() - fontSmall.heightAboveCursor(), id);
        setFont(fontMedium);
        setCrop(276, 0, idLeft - 12 - 276, 63);
        printAt(276, nameTop, row.longName);
        resetCrop();
        drawLine(0, CONSOLE_BAND, width() - 1, CONSOLE_BAND, BLACK);
    } else {
        // Long name clear of the widest short name, and cut short of the id rather than pushing it off its line
        const int16_t column = std::max<int16_t>(150, MARGIN + getTextWidth(back) + 2 + 12);
        setFont(fontSmall);
        const int16_t idLeft = width() - MARGIN - getTextWidth(id);
        printAt(width() - MARGIN, 36, id, RIGHT);
        setFont(fontMedium);
        setCrop(column, 0, std::max<int16_t>(0, idLeft - 12 - column), 99);
        printAt(column, nameTop, row.longName);
        resetCrop();
        drawLine(0, 99, width() - 1, 99, BLACK);
    }

    // Six facts: 2-up in Carry, 3-up in Console. Console's 192 px cells can't hold "59 min ago" or "-20.0 / -130"
    // at real font widths, so it uses the shorter forms
    static constexpr const char *signalNames[] = {"None", "Bad", "Fair", "Good"};
    auto cell = [](const std::string &text) { return text.empty() ? std::string(DASH) : text; };
    std::string battery;
    meshtastic_DeviceMetrics metrics;
    if (nodeDB->copyNodeTelemetry(node, metrics) && metrics.has_battery_level)
        battery = metrics.battery_level > 100 ? "Plugged In" : to_string(metrics.battery_level) + "%";
    const std::pair<const char *, std::string> facts[] = {
        {"LAST HEARD", cell(row.heardSecs < 0 ? ""
                            : console         ? sinceString(row.heardSecs)
                                              : agoString(row.heardSecs))},
        {"HOPS AWAY", cell(row.hops < 0    ? ""
                           : row.hops == 0 ? "Direct"
                                           : hopsString(row.hops))},
        {"SIGNAL", cell(row.signal == SIGNAL_UNKNOWN ? "" : signalNames[row.signal])},
        {"SNR / RSSI", row.snr.empty() && row.rssi.empty() ? DASH : cell(row.snr) + (console ? "/" : " / ") + cell(row.rssi)},
        {"DISTANCE", cell(row.distance)},
        {"BATTERY", cell(battery)},
    };
    const uint8_t cols = console ? 3 : 2;
    const int16_t gridTop = console ? 76 : 112, cellW = console ? 192 : 238, cellH = console ? 82 : 88;
    const int16_t keyInset = console ? 10 : 12, valueTop = keyInset + fontSmall.lineHeight() + (console ? 4 : 6);
    for (uint8_t i = 0; i < 6; i++) {
        const int16_t left = MARGIN + i % cols * cellW, top = gridTop + i / cols * cellH;
        setFont(fontSmall);
        printAt(left, top + keyInset, facts[i].first);
        setFont(fontMedium);
        printBold(left, top + valueTop, facts[i].second);
        drawLine(left, top + cellH - 1, left + cellW - 1, top + cellH - 1, BLACK);
    }

    // Relative position: under the facts in Carry, beside them in Console. North is up, we are the circle
    const int16_t plotLeft = console ? 624 : MARGIN, plotTop = console ? 76 : 396;
    const int16_t plotW = console ? 288 : 476, plotH = console ? 246 : 240, inset = console ? 10 : 12;
    const int16_t cx = plotLeft + plotW / 2, cy = plotTop + plotH / 2 + 12;
    drawRect(plotLeft, plotTop, plotW, plotH, BLACK);
    drawRect(plotLeft + 1, plotTop + 1, plotW - 2, plotH - 2, BLACK);
    setFont(fontSmall);
    printAt(plotLeft + inset, plotTop + 8, "RELATIVE POSITION");
    for (int16_t y = plotTop + 34; y < plotTop + plotH - 10; y += 4)
        drawPixel(cx, y, BLACK);
    for (int16_t x = plotLeft + inset; x < plotLeft + plotW - inset; x += 4)
        drawPixel(x, cy, BLACK);
    printAt(plotLeft + plotW - inset, plotTop + plotH - 8, cell(join(row.distance, row.bearing)), RIGHT, BOTTOM);
    if (row.degrees >= 0) {
        drawCircle(cx, cy, 8, BLACK);
        drawCircle(cx, cy, 7, BLACK);
        const int16_t mx = cx + lround(PLOT_RADIUS * sin(row.degrees * M_PI / 180));
        const int16_t my = cy - lround(PLOT_RADIUS * cos(row.degrees * M_PI / 180));
        fillRect(mx - 13, my - 1, 26, 2, BLACK);
        fillRect(mx - 1, my - 13, 2, 26, BLACK);
        // Name on the side away from the middle: clear of the title above and the distance label below
        printBold(mx, my > cy ? my - 16 - fontSmall.lineHeight() : my + 16, row.shortName, CENTER);
    }

    // Actions. COMPOSE MESSAGE opens the native composer with this node already
    // addressed; SHOW ON MAP opens the native map with this node selected and
    // centred, whatever the age of its position, and with no position at all it
    // still opens and says so rather than pretending (Map 07, E4, E10).
    auto action = [this](int16_t left, int16_t top, uint16_t w, uint16_t h, const char *text, bool available) {
        printThick(left + w / 2, top + h / 2, text, 2, 1);
        if (!available)
            strikeThrough(left + (w - getTextWidth(text)) / 2 - 22, top + h / 2, getTextWidth(text) + 44);
    };
    const bool canCompose = T5ComposeTask::available();
    const bool canMap = indexOf("Map") >= 0;
    if (console) {
        drawLine(MARGIN, 334, MARGIN + 575, 334, BLACK);
        drawLine(MARGIN + 288, 334, MARGIN + 288, 403, BLACK);
        setFont(fontSmall);
        action(MARGIN, 334, 288, 70, "COMPOSE MESSAGE", canCompose);
        action(MARGIN + 288, 334, 288, 70, "SHOW ON MAP", canMap);
    } else {
        drawLine(0, 660, width() - 1, 660, BLACK);
        drawLine(0, 748, width() - 1, 748, BLACK);
        drawLine(0, 836, width() - 1, 836, BLACK);
        setFont(fontMedium);
        action(0, 660, width(), 88, "COMPOSE MESSAGE", canCompose);
        action(0, 748, width(), 88, "SHOW ON MAP", canMap);
    }

    drawNav();
}

bool InkHUD::T5NodeDetailApplet::onTouchPoint(uint16_t x, uint16_t y, bool longPress)
{
    if (longPress)
        return true; // No T5 screen assigns a long press, and the stock fallback would open the shared menu

    // Touch points are in display space; drawing is relative to our tile
    const int16_t tx = x - getTile()->getLeft();
    const int16_t ty = y - getTile()->getTop();

    if (handleNavTap(tx, ty))
        return true;

    // Back: the whole header band in Carry, only the frozen 260 x 63 region in Console, which leaves the
    // long name, id and favourite chip inert. Back returns where this screen was opened from, never Home.
    const bool console = isConsole();
    if (console ? (tx < 260 && ty < 63) : ty < 100) {
        openDestination(origin);
        return true;
    }

    // COMPOSE MESSAGE: the composer opens with this node already addressed, and
    // CANCEL comes back here, so the Node detail context is not lost (Messaging
    // Final 04, entry C)
    const bool onCompose = console ? (ty >= 334 && ty < 404 && tx < MARGIN + 288) : (ty >= 660 && ty < 748);
    if (onCompose) {
        T5ComposeTask::openForNode(node);
        return true;
    }

    // SHOW ON MAP: the map opens with this node selected and centred, and its back chip returns here
    const bool onMap = console ? (ty >= 334 && ty < 404 && tx >= MARGIN + 288) : (ty >= 748 && ty < 836);
    if (onMap)
        T5MapApplet::focusNode(node);

    // Consume every tap: the button fallback would cycle applets
    return true;
}

#endif
