#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5AppsApplet.h"
#include "./T5Text.h"
#include "./T5TracePickTask.h"

#include "UptimeClock.h"
#include "airtime.h"
#include "gps/RTC.h"
#include "mesh/NodeDB.h"
#include "mesh/Throttle.h"
#include "modules/Telemetry/Sensor/TelemetrySensor.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR
#include "modules/Telemetry/EnvironmentTelemetry.h"
#endif
#if !MESHTASTIC_EXCLUDE_AIR_QUALITY_SENSOR
#include "modules/Telemetry/AirQualityTelemetry.h"
#endif
#if !MESHTASTIC_EXCLUDE_POWER_TELEMETRY
#include "modules/Telemetry/PowerTelemetry.h"
#endif

using namespace NicheGraphics;

namespace
{
// Frozen launcher geometry (Apps 06). Carry cards are full width; Console tiles are half, two up
constexpr int16_t CARRY_CARD_H = 148; // A pager drops the page size to 4 rather than shrinking the card
constexpr int16_t CONSOLE_TILE_H = 177;
constexpr int16_t CONSOLE_TILE_H_PAGED = 145;
constexpr int16_t CODE_BOX = 68; // The two-letter badge
constexpr int16_t PAGER_H = 64;

// Frozen row heights, from the Menu vocabulary Apps borrows plus the two rows Apps defines
constexpr int16_t SECTION_H_CARRY = 36;
constexpr int16_t SECTION_H_CONSOLE = 32;
constexpr int16_t INFO_H_CARRY = 48;
constexpr int16_t INFO_H_CONSOLE = 44;
constexpr int16_t FACTS_H = 66;
constexpr int16_t FACTS_H_LARGE = 96;
constexpr int16_t ACTION_BOX_H = 64;
constexpr int16_t ACTION_SLOT_CARRY = 80;
constexpr int16_t ACTION_SLOT_CONSOLE = 72;
constexpr int16_t HOP_H = 64;
constexpr int16_t BAR_H = 34;

InkHUD::T5AppsApplet *instance = nullptr;

// A dotted rule, for the frozen "unavailable" chip and the dashed badge of a blocked card
void dottedRect(InkHUD::Applet &a, int16_t left, int16_t top, uint16_t w, uint16_t h)
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
} // namespace

// The frozen catalogue: fixed order, fixed copy, no user control over either (Apps 03)
const InkHUD::T5AppsApplet::Entry InkHUD::T5AppsApplet::CATALOGUE[TOOL_COUNT] = {
    {Tool::TRACE_ROUTE, "TR", "TRACE ROUTE", "Every relay between you and a node, both ways."},
    {Tool::RF_STATUS, "RF", "RF STATUS", "How busy the channel is, your airtime, packet counts."},
    {Tool::SENSORS, "SN", "SENSORS", "Latest readings from sensors on this device."},
};

InkHUD::T5AppsApplet::T5AppsApplet() : MeshModule("T5AppsApplet"), concurrency::OSThread("T5Apps")
{
    hideFromAppSwitcher = true; // Reached from the APPS nav cell
    instance = this;
    OSThread::disable(); // Enabled on activation, as the other T5 applet threads are
}

InkHUD::T5AppsApplet *InkHUD::T5AppsApplet::get()
{
    return instance;
}

// ---------------------------------------------------------------- lifecycle

void InkHUD::T5AppsApplet::onActivate()
{
    setInputsSubscribed(NAV_UP | NAV_DOWN, true); // Vertical swipes page the launcher
    OSThread::enabled = true;
    OSThread::setIntervalFromNow(60 * 1000UL);
}

void InkHUD::T5AppsApplet::onDeactivate()
{
    setInputsSubscribed(NAV_UP | NAV_DOWN, false);
    OSThread::disable();
}

// Coming back to APPS from another destination shows the launcher, not the tool that was open - the
// tool's state is kept, the card's status line is what carries it back (Apps 05)
void InkHUD::T5AppsApplet::onForeground()
{
    T5Applet::onForeground();
    adoptResult(); // A reply may have landed while we were away
}

void InkHUD::T5AppsApplet::onBackground()
{
    mode = Mode::LAUNCHER;
}

void InkHUD::T5AppsApplet::openTool(Tool t)
{
    // Availability is re-evaluated at the tap, not trusted from the render that drew the card: a
    // sensor can be unplugged in between, and the frozen answer to that is X1 (Apps 04)
    if (availability(t) == Avail::HIDDEN) {
        failedTool = t;
        mode = Mode::FAILED;
        requestUpdate();
        return;
    }
    switch (t) {
    case Tool::TRACE_ROUTE:
        mode = Mode::TRACE;
        adoptResult();
        break;
    case Tool::RF_STATUS:
        mode = Mode::RF;
        break;
    case Tool::SENSORS:
        mode = Mode::SENSORS;
        break;
    }
    lastValueText.clear(); // The next render is a fresh screen, not a value refresh
    requestUpdate();
}

void InkHUD::T5AppsApplet::backToLauncher()
{
    mode = Mode::LAUNCHER;
    requestUpdate();
}

// ---------------------------------------------------------------- catalogue

InkHUD::T5AppsApplet::Avail InkHUD::T5AppsApplet::availability(Tool t) const
{
    switch (t) {
    case Tool::TRACE_ROUTE:
        // Not compiled in: hidden. Compiled in but a setting the user can change here stops it:
        // shown, with the reason (Apps 04)
        if (!traceRouteModule)
            return Avail::HIDDEN;
        return traceBlocked() ? Avail::BLOCKED : Avail::READY;

    case Tool::RF_STATUS:
        // airTime and the local counters exist on every build, so this tool is always available.
        // With the region unset there is simply no traffic to report, which its own status says
        return airTime ? Avail::READY : Avail::HIDDEN;

    case Tool::SENSORS: {
        // A telemetry module that reads local sensors must exist, and at least one sensor must have
        // been detected. Neither is something the user can fix from this device, so: hidden (Apps 04)
        bool detected = false;
        for (uint8_t i = 1; i <= (uint8_t)_meshtastic_TelemetrySensorType_MAX; i++) {
            if (nodeTelemetrySensorsMap[i].first > 0) {
                detected = true;
                break;
            }
        }
        if (!detected)
            return Avail::HIDDEN;
        bool haveModule = false;
#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR
        haveModule = haveModule || environmentTelemetryModule != nullptr;
#endif
#if !MESHTASTIC_EXCLUDE_AIR_QUALITY_SENSOR
        haveModule = haveModule || airQualityTelemetryModule != nullptr;
#endif
#if !MESHTASTIC_EXCLUDE_POWER_TELEMETRY
        haveModule = haveModule || powerTelemetryModule != nullptr;
#endif
        return haveModule ? Avail::READY : Avail::HIDDEN;
    }
    }
    return Avail::HIDDEN;
}

// Trace Route sends a packet, so the two settings that stop it sending block the tool (Apps T8)
bool InkHUD::T5AppsApplet::traceBlocked() const
{
    return config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET || !config.lora.tx_enabled;
}

const char *InkHUD::T5AppsApplet::blockedReason(Tool t) const
{
    if (t != Tool::TRACE_ROUTE || !traceBlocked())
        return nullptr;
    // Region first: with no region there is no radio at all, so it is the more basic of the two
    return config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET ? "SET REGION FIRST" : "TRANSMIT OFF";
}

std::vector<InkHUD::T5AppsApplet::Tool> InkHUD::T5AppsApplet::visibleTools() const
{
    std::vector<Tool> out;
    for (const Entry &e : CATALOGUE)
        if (availability(e.id) != Avail::HIDDEN)
            out.push_back(e.id);
    return out;
}

// The card's live line: short, caps, and the reason instead whenever the tool is blocked (Apps 14)
std::string InkHUD::T5AppsApplet::statusLine(Tool t) const
{
    if (const char *reason = blockedReason(t))
        return reason;

    switch (t) {
    case Tool::TRACE_ROUTE:
        if (traceState == Trace::TRACING)
            return std::string("TRACING") + ELLIPSIS;
        if (haveResult && traceTarget) {
            const std::string when = t5ListTime(traceResultAtTime, false);
            std::string line = "LAST " + std::string(SEP) + " " + t5NodeTitle(traceTarget);
            const uint8_t hops = (uint8_t)(traceResult.route.route_count + 1);
            line += std::string(" ") + SEP + " " + (hops == 1 ? "DIRECT" : to_string(hops) + " HOPS");
            if (!when.empty())
                line += std::string(" ") + SEP + " " + when;
            return line;
        }
        if (traceState == Trace::NO_REPLY)
            return "NO REPLY";
        return "NO TRACE YET";

    case Tool::RF_STATUS: {
        if (config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET)
            return std::string("RADIO SILENT ") + SEP + " NO TRAFFIC";
        if (!airTime)
            return "";
        char line[48];
        snprintf(line, sizeof(line), "CHANNEL %.0f %% %s MY AIRTIME %.1f %%", airTime->channelUtilizationPercent(), SEP,
                 airTime->utilizationTXPercent());
        return line;
    }

    case Tool::SENSORS: {
        const std::vector<Section> sections = sensorSections();
        if (sections.empty())
            return "NO SENSOR DATA";
        const Section &first = sections.front();
        if (first.readings.empty())
            return first.label; // Found, nothing measured yet
        // The sensor and its first couple of readings, which is what the frozen card shows
        std::string line = first.label;
        for (size_t i = 0; i < first.readings.size() && i < 2; i++)
            line += std::string(" ") + SEP + " " + first.readings[i].value;
        return line;
    }
    }
    return "";
}

// ---------------------------------------------------------------- render

void InkHUD::T5AppsApplet::onRender(bool full)
{
    (void)full;
    hits.clear();
    pagerShown = false;
    clearPane(); // A pane never outlives the render that set it

    switch (mode) {
    case Mode::LAUNCHER:
        renderLauncher();
        break;
    case Mode::TRACE:
        renderTrace();
        break;
    case Mode::RF:
        renderRf();
        break;
    case Mode::SENSORS:
        renderSensors();
        break;
    case Mode::FAILED: {
        // X1: the generic "could not open" template, with the tool's own name kept in the title
        const Entry &e = CATALOGUE[(uint8_t)failedTool];
        drawHeader(e.name, "NOT AVAILABLE", "APPS");
        int16_t y = headerHeight();
        const std::string what = failedTool == Tool::SENSORS ? "Sensors" : e.name;
        rowBig(y, what + " could not open",
               "The sensor stopped responding. Check the connection. This tool disappears from APPS after the next restart "
               "if the sensor is gone.",
               100);
        rowAction(y, Hit::BACK_TO_APPS, "BACK TO APPS", false);
        drawNav();
        break;
    }
    }
}

// The frozen header: Carry gives it a 100 px band with the context on its own line; Console packs
// title and context into the 64 px band. The launcher has no Back (Apps 06)
void InkHUD::T5AppsApplet::drawHeader(const std::string &title, const std::string &context, const char *back)
{
    const bool console = isConsole();
    const std::string status = console ? join(powerLabel(false), getTimeString()) : powerLabel(false);
    const std::string head = back ? std::string(BACK) + " " + back : title;

    if (console) {
        setFont(fontSmall);
        const int16_t statusLeft = width() - MARGIN - getTextWidth(status);
        printAt(width() - MARGIN, (CONSOLE_BAND - fontSmall.lineHeight()) / 2, status, RIGHT);

        setFont(fontLarge);
        const int16_t titleTop = (CONSOLE_BAND - fontLarge.lineHeight()) / 2;
        printBold(MARGIN, titleTop, head);
        int16_t x = MARGIN + getTextWidth(head) + 12;
        if (back) {
            // The tool's own name follows the crumb, then the context
            setFont(fontMedium);
            printBold(x, titleTop + fontLarge.heightAboveCursor() - fontMedium.heightAboveCursor(), title);
            x += getTextWidth(title) + 12;
        }
        setFont(fontSmall);
        const int16_t contextW = std::max<int16_t>(0, statusLeft - 12 - x);
        setCrop(x, 0, contextW, CONSOLE_BAND);
        printAt(x, (CONSOLE_BAND - fontSmall.lineHeight()) / 2, t5Elide(*this, context, contextW));
        resetCrop();
        drawLine(0, CONSOLE_BAND, width() - 1, CONSOLE_BAND, BLACK);
        return;
    }

    setFont(fontSmall);
    const int16_t statusLeft = width() - MARGIN - getTextWidth(status);
    printAt(width() - MARGIN, 20, status, RIGHT);

    setFont(fontLarge);
    setCrop(MARGIN, 0, std::max<int16_t>(0, statusLeft - 12 - MARGIN), 99);
    printBold(MARGIN, 14, head);
    resetCrop();
    if (back) {
        // Carry has room for the crumb and the tool name on one line. The crumb is measured in its own
        // large font, which is still set
        const int16_t nameLeft = MARGIN + getTextWidth(head) + 12;
        const int16_t nameTop = 14 + fontLarge.heightAboveCursor() - fontMedium.heightAboveCursor();
        setFont(fontMedium);
        const int16_t nameW = std::max<int16_t>(0, statusLeft - 12 - nameLeft);
        setCrop(nameLeft, 0, nameW, 99);
        printBold(nameLeft, nameTop, t5Elide(*this, title, nameW > 2 ? nameW - 2 : 0)); // 2 px of faux bold
        resetCrop();
    }
    setFont(fontSmall);
    setCrop(MARGIN, 0, width() - 2 * MARGIN, 99);
    printAt(MARGIN, 66, t5Elide(*this, context, width() - 2 * MARGIN));
    resetCrop();
    drawLine(0, 99, width() - 1, 99, BLACK);
}

int16_t InkHUD::T5AppsApplet::bodyBottom()
{
    return navTop() - 1 - (pagerShown ? PAGER_H : 0);
}

// Three cells: PREV, the count, NEXT. The end pages drop the arrow that would do nothing (Apps A4)
void InkHUD::T5AppsApplet::drawPager(uint8_t page, uint8_t pages)
{
    pagerShown = true;
    pagerTop = navTop() - PAGER_H;
    drawLine(0, pagerTop, width() - 1, pagerTop, BLACK);
    const int16_t cell = width() / 3;
    setFont(fontSmall);
    if (page > 0)
        printAt(cell / 2, pagerTop + PAGER_H / 2, std::string(BACK) + " PREV", CENTER, MIDDLE);
    printAt(width() / 2, pagerTop + PAGER_H / 2, to_string(page + 1) + " / " + to_string(pages), CENTER, MIDDLE);
    if (page + 1 < pages)
        printAt(width() - cell / 2, pagerTop + PAGER_H / 2, std::string("NEXT ") + FWD, CENTER, MIDDLE);
}

// ---------------------------------------------------------------- launcher

uint8_t InkHUD::T5AppsApplet::cardsPerPage()
{
    if (isConsole())
        return 4;                             // Two by two
    return visibleTools().size() > 5 ? 4 : 5; // The pager costs one row (Apps 06)
}

uint8_t InkHUD::T5AppsApplet::launcherPages()
{
    const size_t n = visibleTools().size();
    const uint8_t per = cardsPerPage();
    return n == 0 ? 1 : (uint8_t)((n + per - 1) / per);
}

void InkHUD::T5AppsApplet::renderLauncher()
{
    const bool console = isConsole();
    const std::vector<Tool> tools = visibleTools();
    cardsShown.clear();
    cardTops.clear();

    // The context line counts the shelf, except when there is no radio to use it with (Apps A3, A6)
    std::string context;
    if (tools.empty())
        context = "NO TOOLS IN THIS BUILD";
    else if (config.lora.region == meshtastic_Config_LoRaConfig_RegionCode_UNSET)
        context = std::string("RADIO SILENT ") + SEP + " REGION NOT SET";
    else
        context = to_string(tools.size()) + (tools.size() == 1 ? " TOOL " : " TOOLS ") + SEP + " SETTINGS ARE IN MENU";

    drawHeader("APPS", context, nullptr); // No Back: this is a top-level destination

    if (tools.empty()) {
        // A6: only reachable in a build that excludes both unconditional tools. The nav keeps its
        // six cells regardless, so the bar never changes shape
        int16_t y = headerHeight();
        rowBig(y, "No tools in this build",
               "This firmware was built without the optional tools. Mesh status is on HOME; settings are in MENU.", 140);
        drawNav();
        return;
    }

    const uint8_t per = cardsPerPage();
    const uint8_t pages = launcherPages();
    if (launcherFirst >= tools.size())
        launcherFirst = 0;
    // Frozen mode-switch rule: the page is whichever one holds the entry that was at the top
    const uint8_t launcherPage = (uint8_t)std::min<uint16_t>(launcherFirst / per, (uint16_t)(pages - 1));
    launcherFirst = (uint16_t)launcherPage * per;
    const bool paged = tools.size() > (size_t)(console ? 4 : 5);
    if (paged)
        drawPager(launcherPage, pages);

    cardColumns = console ? 2 : 1;
    cardHeight = console ? (paged ? CONSOLE_TILE_H_PAGED : CONSOLE_TILE_H) : CARRY_CARD_H;
    cardWidth = console ? (width() / 2) : width();

    const int16_t top = headerHeight();
    for (uint8_t i = 0; i < per; i++) {
        const size_t index = (size_t)launcherPage * per + i;
        if (index >= tools.size())
            break; // The last page is not padded: unused cells stay blank (Apps K1)
        const int16_t left = console ? (int16_t)(i % 2) * cardWidth : 0;
        const int16_t cardTop = top + (console ? (i / 2) : i) * cardHeight;
        if (cardTop + cardHeight > bodyBottom())
            break;
        drawCard(left, cardTop, cardWidth, cardHeight, tools[index]);
        cardsShown.push_back(tools[index]);
        cardTops.push_back(cardTop);
    }

    // A1's single hint, and only while the tool it describes is still hidden
    if (!console && availability(Tool::SENSORS) == Avail::HIDDEN) {
        int16_t y = top + (int16_t)cardsShown.size() * cardHeight + 8;
        if (y + 2 * fontSmall.lineHeight() < bodyBottom())
            rowNote(y, "SENSORS appears here when a sensor is connected and turned on in the Meshtastic app.");
    }

    drawNav();
}

// One card: the badge, the name, the one-line description, and the live status line. The whole card
// is the target, so there is no chevron and no separate hit region (Apps 06)
void InkHUD::T5AppsApplet::drawCard(int16_t left, int16_t top, uint16_t w, uint16_t h, Tool t)
{
    const Entry &e = CATALOGUE[(uint8_t)t];
    const bool blocked = availability(t) == Avail::BLOCKED;
    const int16_t right = left + (int16_t)w;

    drawLine(left, top + (int16_t)h - 1, right - 1, top + (int16_t)h - 1, BLACK);
    if (left > 0)
        drawLine(left, top, left, top + (int16_t)h - 1, BLACK); // Console's column rule

    // The badge: a 2 px box, or the frozen dashed one when the tool is blocked
    const int16_t boxTop = top + 16;
    if (blocked)
        dottedRect(*this, left + MARGIN, boxTop, CODE_BOX, CODE_BOX);
    else {
        drawRect(left + MARGIN, boxTop, CODE_BOX, CODE_BOX, BLACK);
        drawRect(left + MARGIN + 1, boxTop + 1, CODE_BOX - 2, CODE_BOX - 2, BLACK);
    }
    setFont(fontMedium);
    printAt(left + MARGIN + CODE_BOX / 2, boxTop + CODE_BOX / 2, e.code, CENTER, MIDDLE);

    const int16_t textLeft = left + MARGIN + CODE_BOX + 16;
    const int16_t textRight = right - MARGIN;
    setFont(fontMedium);
    setCrop(textLeft, top, std::max<int16_t>(0, textRight - textLeft), h);
    // A blocked tool keeps its name at regular weight: the card is still usable, just not yet
    if (blocked)
        printAt(textLeft, top + 14, e.name);
    else
        printBold(textLeft, top + 14, e.name);
    resetCrop();

    setFont(fontSmall);
    const int16_t descTop = top + 14 + fontMedium.lineHeight() + 6;
    printAt(textLeft, descTop, t5Elide(*this, e.desc, std::max<int16_t>(0, textRight - textLeft)));

    const std::string status = statusLine(t);
    const int16_t statusTop = descTop + fontSmall.lineHeight() + 8;
    if (blocked) {
        // The reason sits in a dotted chip, the Menu vocabulary for "you can fix this"
        const int16_t cw = getTextWidth(status) + 2 * 12, ch = fontSmall.lineHeight() + 2 * 6;
        dottedRect(*this, textLeft, statusTop, cw, ch);
        printAt(textLeft + 12, statusTop + 6, status);
    } else {
        setCrop(textLeft, top, std::max<int16_t>(0, textRight - textLeft), h);
        printAt(textLeft, statusTop, t5Elide(*this, status, std::max<int16_t>(0, textRight - textLeft)));
        resetCrop();
    }
}

// ---------------------------------------------------------------- panes

void InkHUD::T5AppsApplet::setPane(int16_t left, uint16_t w)
{
    paneLeft = left;
    paneSpan = w;
    setCrop(left, 0, w, height());
}

void InkHUD::T5AppsApplet::clearPane()
{
    paneLeft = 0;
    paneSpan = 0;
    resetCrop();
}

// ---------------------------------------------------------------- row vocabulary

void InkHUD::T5AppsApplet::rowSection(int16_t &y, const std::string &label)
{
    const int16_t h = isConsole() ? SECTION_H_CONSOLE : SECTION_H_CARRY;
    setFont(fontSmall);
    printAt(paneL() + MARGIN, y + (h - fontSmall.lineHeight()) / 2 - 2, label);
    drawLine(paneL(), y + h - 1, paneR() - 1, y + h - 1, BLACK);
    y += h;
}

void InkHUD::T5AppsApplet::rowInfo(int16_t &y, const std::string &label, const std::string &value)
{
    const int16_t h = isConsole() ? INFO_H_CONSOLE : INFO_H_CARRY;
    setFont(fontSmall);
    printAt(paneL() + MARGIN, y + (h - fontSmall.lineHeight()) / 2, label);
    printBold(paneR() - MARGIN, y + (h - fontSmall.lineHeight()) / 2, value, RIGHT);
    y += h;
}

// A grid of key/value cells: the frozen facts row. `large` is the Sensors treatment, where the
// reading itself is the point and gets the bigger font (Apps S1)
void InkHUD::T5AppsApplet::rowFacts(int16_t &y, const std::vector<Reading> &cells, uint8_t cols, bool large)
{
    if (cells.empty() || cols == 0)
        return;
    const int16_t h = large ? FACTS_H_LARGE : FACTS_H;
    const int16_t cellW = (paneR() - paneL() - 2 * MARGIN) / cols;
    const uint8_t rows = (uint8_t)((cells.size() + cols - 1) / cols);
    for (size_t i = 0; i < cells.size(); i++) {
        const int16_t left = paneL() + MARGIN + (int16_t)(i % cols) * cellW;
        const int16_t top = y + (int16_t)(i / cols) * h;
        setFont(fontSmall);
        printAt(left, top + 6, cells[i].key);
        setFont(large ? fontLarge : fontMedium);
        setCrop(left, top, cellW - 8, h);
        printBold(left, top + 6 + fontSmall.lineHeight() + 4, cells[i].value);
        resetCrop();
    }
    y += (int16_t)rows * h;
}

void InkHUD::T5AppsApplet::rowNote(int16_t &y, const std::string &text)
{
    setFont(fontSmall);
    for (const std::string &line : t5WrapLines(*this, text, paneR() - paneL() - 2 * MARGIN)) {
        printAt(paneL() + MARGIN, y, line);
        y += fontSmall.lineHeight() + 2;
    }
    y += 6;
}

void InkHUD::T5AppsApplet::rowBullet(int16_t &y, const std::string &text)
{
    setFont(fontSmall);
    const int16_t indent = paneL() + MARGIN + 18;
    // A filled square rather than a bullet glyph: one shape, no font dependency
    fillRect(paneL() + MARGIN + 2, y + fontSmall.lineHeight() / 2 - 2, 5, 5, BLACK);
    for (const std::string &line : t5WrapLines(*this, text, paneR() - MARGIN - indent)) {
        printAt(indent, y, line);
        y += fontSmall.lineHeight() + 2;
    }
    y += 4;
}

// The frozen "big" block: a statement in the medium font, then an explanation, with the pad above
void InkHUD::T5AppsApplet::rowBig(int16_t &y, const std::string &title, const std::string &body, int16_t pad)
{
    y += pad;
    setFont(fontMedium);
    printBold(paneL() + MARGIN, y, title);
    y += fontMedium.lineHeight() + 10;
    setFont(fontSmall);
    for (const std::string &line : t5WrapLines(*this, body, paneR() - paneL() - 2 * MARGIN)) {
        printAt(paneL() + MARGIN, y, line);
        y += fontSmall.lineHeight() + 2;
    }
    y += 12;
}

// An inverted banner: information, never a shortcut. Jumping into Menu from here would break Back,
// so it names the place instead (Apps T8)
void InkHUD::T5AppsApplet::rowBanner(int16_t &y, const std::string &title, const std::string &body)
{
    setFont(fontSmall);
    const std::vector<std::string> lines = t5WrapLines(*this, body, paneR() - paneL() - 2 * MARGIN - 8);
    setFont(fontMedium);
    const int16_t h = 16 + fontMedium.lineHeight() + 6 + (int16_t)lines.size() * (fontSmall.lineHeight() + 2) + 16;
    fillRect(paneL(), y, paneR() - paneL(), h, BLACK);
    setTextColor(WHITE);
    printBold(paneL() + MARGIN, y + 16, title);
    setFont(fontSmall);
    int16_t ty = y + 16 + fontMedium.lineHeight() + 6;
    for (const std::string &line : lines) {
        printAt(paneL() + MARGIN, ty, line);
        ty += fontSmall.lineHeight() + 2;
    }
    setTextColor(BLACK);
    y += h;
}

// One hop of a route. The box says what the node was: an endpoint filled, a relay outlined, a relay
// that could not decode the packet dashed (Apps T5)
void InkHUD::T5AppsApplet::rowHop(int16_t &y, uint8_t n, const std::string &name, const std::string &sub, const std::string &snr,
                                  HopKind kind)
{
    const int16_t box = 40, boxLeft = paneL() + MARGIN, boxTop = y + (HOP_H - box) / 2;
    if (kind == HOP_END) {
        fillRect(boxLeft, boxTop, box, box, BLACK);
        setTextColor(WHITE);
    } else if (kind == HOP_UNK)
        dottedRect(*this, boxLeft, boxTop, box, box);
    else {
        drawRect(boxLeft, boxTop, box, box, BLACK);
        drawRect(boxLeft + 1, boxTop + 1, box - 2, box - 2, BLACK);
    }
    setFont(fontSmall);
    printAt(boxLeft + box / 2, boxTop + box / 2, to_string(n), CENTER, MIDDLE);
    setTextColor(BLACK);

    // The SNR this node heard from the one before it, right-aligned with its own label above
    int16_t right = paneR() - MARGIN;
    if (!snr.empty()) {
        setFont(fontSmall);
        printAt(right, y + 10, "SNR", RIGHT);
        setFont(fontMedium);
        printAt(right, y + 10 + fontSmall.lineHeight() + 2, snr, RIGHT);
        right -= std::max<int16_t>(getTextWidth(snr), 40) + 16;
    }

    const int16_t textLeft = boxLeft + box + 16;
    setFont(fontMedium);
    setCrop(textLeft, y, std::max<int16_t>(0, right - textLeft), HOP_H);
    printBold(textLeft, y + 8, name);
    resetCrop();
    setFont(fontSmall);
    setCrop(textLeft, y, std::max<int16_t>(0, right - textLeft), HOP_H);
    printAt(textLeft, y + 8 + fontMedium.lineHeight() + 2, sub);
    resetCrop();
    y += HOP_H;
}

void InkHUD::T5AppsApplet::rowBarHeader(int16_t &y)
{
    setFont(fontSmall);
    printAt(paneL() + MARGIN, y, "HOUR");
    printAt(paneR() - MARGIN, y, std::string("RX ") + SEP + " TX", RIGHT);
    y += fontSmall.lineHeight() + 6;
}

// Solid rectangles and the numbers beside them. No gradients, no line chart: the numbers carry the
// meaning and the bars only rank the hours (Apps 10)
void InkHUD::T5AppsApplet::rowBar(int16_t &y, const std::string &label, uint32_t rx, uint32_t tx, uint32_t max)
{
    setFont(fontSmall);
    const int16_t labelW = 72, numW = 108;
    printAt(paneL() + MARGIN, y + (BAR_H - fontSmall.lineHeight()) / 2, label);
    const int16_t barLeft = paneL() + MARGIN + labelW;
    const int16_t barRoom = paneR() - MARGIN - numW - barLeft;
    if (barRoom > 0 && max > 0) {
        const int16_t rxW = (int16_t)((uint64_t)rx * barRoom / max);
        const int16_t txW = (int16_t)((uint64_t)tx * barRoom / max);
        fillRect(barLeft, y + 6, std::max<int16_t>(rxW, rx ? 1 : 0), 10, BLACK);
        fillRect(barLeft, y + 20, std::max<int16_t>(txW, tx ? 1 : 0), 6, BLACK);
    }
    printAt(paneR() - MARGIN, y + (BAR_H - fontSmall.lineHeight()) / 2, to_string(rx) + " " + SEP + " " + to_string(tx), RIGHT);
    y += BAR_H;
}

// The target card: one target, and tapping it chooses another. Locked while a trace is in flight,
// which is why `allowChange` exists (Apps T4)
void InkHUD::T5AppsApplet::rowTarget(int16_t &y, bool allowChange, bool small)
{
    const int16_t h = small ? 72 : 96;
    const std::string name = traceTarget ? t5NodeTitle(traceTarget) : "No target";
    std::string sub = traceTarget ? t5NodeTag(traceTarget) : "";
    if (traceTarget) {
        const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(traceTarget);
        if (node && node->has_hops_away)
            sub = join(sub, hopsString(node->hops_away) + " away");
        if (node && node->last_heard)
            sub = join(sub, "heard " + t5ListTime(node->last_heard, false));
    }

    int16_t right = paneR() - MARGIN;
    if (allowChange) {
        setFont(fontSmall);
        printAt(right, y + h / 2, "CHANGE", RIGHT, MIDDLE);
        right -= getTextWidth("CHANGE") + 16;
        addHit(Hit::TARGET_CARD, paneL(), y, paneR() - paneL(), h);
    }
    setFont(small ? fontMedium : fontLarge);
    setCrop(paneL() + MARGIN, y, std::max<int16_t>(0, right - MARGIN), h);
    printBold(paneL() + MARGIN, y + 10, name);
    resetCrop();
    setFont(fontSmall);
    setCrop(paneL() + MARGIN, y, std::max<int16_t>(0, right - MARGIN), h);
    printAt(paneL() + MARGIN, y + 10 + (small ? fontMedium.lineHeight() : fontLarge.lineHeight()) + 4, sub);
    resetCrop();
    drawLine(paneL(), y + h - 1, paneR() - 1, y + h - 1, BLACK);
    y += h;
}

void InkHUD::T5AppsApplet::rowAction(int16_t &y, Hit what, const std::string &label, bool inverted, bool enabled,
                                     const std::string &reason)
{
    const int16_t slot = isConsole() ? ACTION_SLOT_CONSOLE : ACTION_SLOT_CARRY;
    const int16_t boxTop = y + (slot - ACTION_BOX_H) / 2;
    const int16_t left = paneL() + MARGIN, w = paneR() - paneL() - 2 * MARGIN;

    if (!enabled) {
        // The frozen unavailable action: a dashed box, and the reason where the label's emphasis was
        dottedRect(*this, left, boxTop, w, ACTION_BOX_H);
        setFont(fontMedium);
        printAt(left + 16, boxTop + (ACTION_BOX_H - fontMedium.lineHeight()) / 2, label);
        if (!reason.empty()) {
            setFont(fontSmall);
            printAt(left + w - 16, boxTop + (ACTION_BOX_H - fontSmall.lineHeight()) / 2, reason, RIGHT);
        }
        y += slot;
        return;
    }

    if (inverted) {
        fillRect(left, boxTop, w, ACTION_BOX_H, BLACK);
        setTextColor(WHITE);
    } else {
        drawRect(left, boxTop, w, ACTION_BOX_H, BLACK);
        drawRect(left + 1, boxTop + 1, w - 2, ACTION_BOX_H - 2, BLACK);
    }
    setFont(fontMedium);
    printThick(left + w / 2, boxTop + ACTION_BOX_H / 2, label, 2, 1);
    setTextColor(BLACK);
    addHit(what, left, boxTop, w, ACTION_BOX_H);
    y += slot;
}

void InkHUD::T5AppsApplet::addHit(Hit what, int16_t left, int16_t top, uint16_t w, uint16_t h)
{
    hits.push_back({what, top, (int16_t)(top + (int16_t)h), left, (int16_t)(left + (int16_t)w)});
}

InkHUD::T5AppsApplet::Hit InkHUD::T5AppsApplet::hitAt(int16_t x, int16_t y) const
{
    for (const Target &t : hits)
        if (y >= t.top && y < t.bottom && x >= t.left && x < t.right)
            return t.what;
    return Hit::NONE;
}

// ---------------------------------------------------------------- input

void InkHUD::T5AppsApplet::onNavUp()
{
    const uint8_t per = cardsPerPage();
    if (mode == Mode::LAUNCHER && launcherFirst >= per) {
        launcherFirst -= per;
        requestUpdate();
    }
}

void InkHUD::T5AppsApplet::onNavDown()
{
    const uint8_t per = cardsPerPage();
    if (mode == Mode::LAUNCHER && launcherFirst + per < visibleTools().size()) {
        launcherFirst += per;
        requestUpdate();
    }
}

// Carry <-> Console. The launcher page follows its first visible entry, and every tool re-lays out
// from state that survives a re-layout untouched. Only the picker's own tile has to be re-placed
void InkHUD::T5AppsApplet::onModeChanged()
{
    if (T5TracePickTask *pick = T5TracePickTask::get())
        pick->onModeChanged();
}

bool InkHUD::T5AppsApplet::onTouchPoint(uint16_t x, uint16_t y, bool longPress)
{
    if (longPress)
        return true; // No T5 screen assigns a long press, and the stock fallback would open the menu

    const int16_t tx = (int16_t)x - getTile()->getLeft();
    const int16_t ty = (int16_t)y - getTile()->getTop();

    if (handleNavTap(tx, ty))
        return true;

    // The header band is Back from a tool; the launcher has no Back, so its band does nothing
    const int16_t backBottom = isConsole() ? CONSOLE_BAND : 99;
    const int16_t backRight = isConsole() ? 260 : 200;
    if (mode != Mode::LAUNCHER && ty < backBottom && tx < backRight) {
        backToLauncher();
        return true;
    }

    if (pagerShown && ty >= pagerTop && ty < navTop()) {
        const int16_t cell = width() / 3;
        if (tx < cell)
            onNavUp();
        else if (tx >= 2 * cell)
            onNavDown();
        return true;
    }

    if (mode == Mode::LAUNCHER) {
        for (size_t i = 0; i < cardsShown.size(); i++) {
            const int16_t left = cardColumns == 2 ? (int16_t)(i % 2) * cardWidth : 0;
            if (ty >= cardTops[i] && ty < cardTops[i] + cardHeight && tx >= left && tx < left + cardWidth) {
                openTool(cardsShown[i]);
                return true;
            }
        }
        return true; // Consume every tap: the button fallback would cycle applets
    }

    switch (hitAt(tx, ty)) {
    case Hit::CHOOSE_NODE:
    case Hit::NEW_TARGET:
    case Hit::TARGET_CARD:
        T5TracePickTask::open();
        break;
    case Hit::START_TRACE:
    case Hit::RUN_AGAIN:
        startTrace();
        break;
    case Hit::BACK_TO_APPS:
        backToLauncher();
        break;
    case Hit::NONE:
        break;
    }
    return true;
}

// ---------------------------------------------------------------- packets and ticks

bool InkHUD::T5AppsApplet::wantPacket(const meshtastic_MeshPacket *p)
{
    // Only the reply to our own trace matters here, and only while the applet is live. Note this
    // outlives TRACING: after the 60 s deadline we have stopped waiting but still owe the user the
    // late reply T6 promises, so the request id - not the state - is what decides (Apps 09)
    return isActive() && traceRequestId != 0 && p->decoded.portnum == meshtastic_PortNum_TRACEROUTE_APP;
}

ProcessMessage InkHUD::T5AppsApplet::handleReceived(const meshtastic_MeshPacket &mp)
{
    (void)mp;
    // The module decodes and retains the route; we only look for one matching our request. Reading
    // it here rather than from the packet keeps the unknown-hop placeholders it inserts
    adoptResult();
    return ProcessMessage::CONTINUE;
}

int32_t InkHUD::T5AppsApplet::runOnce()
{
    bool changed = false;

    if (traceState == Trace::TRACING) {
        adoptResult();
        if (traceState == Trace::TRACING && !Throttle::isWithinTimespanMs(traceSentAtMs, traceDeadlineMs())) {
            traceState = Trace::NO_REPLY; // The frozen deadline passed with nothing back
            changed = true;
        }
    }

    // One FAST exactly when RUN AGAIN becomes available, and only if that screen is on show
    if (traceRouteModule) {
        const uint32_t end = traceRouteModule->getCooldownEndMs();
        const bool active = end && (int32_t)(end - Time::stampMillis()) > 0;
        if (cooldownWasActive && !active && mode == Mode::TRACE)
            changed = true;
        cooldownWasActive = active;
    }

    // At most one value redraw a minute, and none when the formatted text is identical
    if (!changed && isForeground() && !Throttle::isWithinTimespanMs(lastValueRedrawMs, 60000)) {
        const std::string now = valueSignature();
        if (now != lastValueText) {
            lastValueText = now;
            lastValueRedrawMs = millis();
            changed = true;
        }
    }

    if (changed && isForeground())
        requestUpdate();

    // A tool that is not on screen has nothing to redraw, so the tick can be lazy. On screen, the
    // frozen cadence is at most one value refresh a minute (Apps 12, 16)
    if (!isForeground())
        return 60000;
    if (mode == Mode::RF || mode == Mode::SENSORS || mode == Mode::LAUNCHER)
        return 60000;
    return traceState == Trace::TRACING ? 1000 : 60000;
}

// What the open tool is currently showing, formatted. Compared on the tick so an unchanged poll
// asks for no frame at all, which is the frozen rule for a value redraw (Apps 12)
std::string InkHUD::T5AppsApplet::valueSignature()
{
    switch (mode) {
    case Mode::LAUNCHER: {
        std::string out;
        for (Tool t : visibleTools())
            out += statusLine(t);
        return out;
    }
    case Mode::RF:
        return rfSignature();
    case Mode::SENSORS:
        return sensorSignature();
    default:
        return ""; // Trace redraws on its own events, not on a poll
    }
}

#endif
