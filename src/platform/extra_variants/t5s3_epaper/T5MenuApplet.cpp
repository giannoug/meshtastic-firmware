#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5MenuApplet.h"
#include "./T5Applet.h"
#include "./T5ComposeTask.h"
#include "./T5MenuTask.h"
#include "./T5SystemLayer.h"
#include "./T5Text.h"
#include "./T5TracePickTask.h"

#include "UptimeClock.h"
#include "gps/RTC.h"
#include "graphics/niche/InkHUD/InkHUD.h"
#include "graphics/niche/InkHUD/Tile.h"
#include "mesh/Throttle.h"

using namespace NicheGraphics;

namespace
{
InkHUD::T5MenuApplet *instance = nullptr;

// The frozen LIVE rows refresh at most once a minute, only while visible, and only if they changed
constexpr uint32_t LIVE_REFRESH_MS = 60 * 1000UL;

// Frozen Console tile grid: eight categories, 2 x 4, all visible so any one is a single tap away
constexpr int16_t TILE_W = 180;
constexpr int16_t TILE_H = 89;
constexpr uint8_t TILE_COLUMNS = 2;
constexpr int16_t TILES_WIDTH = TILE_COLUMNS * TILE_W; // 360; the pane starts two pixels later
} // namespace

InkHUD::T5MenuApplet::T5MenuApplet() : concurrency::OSThread("T5Menu")
{
    // A destination that is a system applet owns its own full-screen tile, as the task layers do:
    // nothing else places a tile for a system applet
    (new Tile)->assignApplet(this);
    OSThread::setIntervalFromNow(LIVE_REFRESH_MS);
    OSThread::disable(); // Nothing to tick until the menu is open
}

void InkHUD::T5MenuApplet::begin()
{
    assert(!instance);
    instance = new T5MenuApplet;
    instance->name = "T5Menu";
    instance->activate();

    // First among the T5 system applets: earlier means its input claim is checked first, and later
    // means it draws on top. The menu wants the opposite of both - the task layers and the system
    // layer draw over it and take its taps - so it goes first and declines a tap whenever one of
    // them is up, which is how the composer already defers to the system layer
    InkHUD::getInstance()->systemApplets.push_back(instance);
    instance->getTile()->setRegion(0, 0, InkHUD::getInstance()->width(), InkHUD::getInstance()->height());
}

InkHUD::T5MenuApplet *InkHUD::T5MenuApplet::get()
{
    return instance;
}

bool InkHUD::T5MenuApplet::available()
{
    return instance != nullptr;
}

bool InkHUD::T5MenuApplet::isOpen()
{
    return instance && instance->open_;
}

void InkHUD::T5MenuApplet::open()
{
    if (!instance)
        return;

    if (!instance->open_) {
        instance->open_ = true;
        instance->page = T5Page::ROOT;
        instance->pageIndex = 0;
        instance->errorRow = T5Act::NONE;
        // Console lands on DISPLAY the first time after boot, and on the last category of this
        // session afterwards. RAM only: never persisted (decision 5)
        if (t5IsConsole(*instance))
            instance->page = t5CategoryPage(instance->consoleCategory);
    } else {
        // MENU while already in the menu returns to the root in Carry, and to the landing tile in
        // Console (Menu Settings 14, touch regions)
        instance->page = t5IsConsole(*instance) ? t5CategoryPage(instance->consoleCategory) : T5Page::ROOT;
        instance->pageIndex = 0;
    }

    instance->reconcile();
    instance->requestUpdate();
}

// Another nav cell was tapped. The user applet it opens requests the frame, so nothing is asked for
// here beyond letting go of the screen
void InkHUD::T5MenuApplet::leaveForDestination()
{
    if (!instance || !instance->open_)
        return;
    instance->open_ = false;
    instance->reconcile();
}

void InkHUD::T5MenuApplet::onModeChanged()
{
    if (!instance)
        return;
    instance->getTile()->setRegion(0, 0, InkHUD::getInstance()->width(), InkHUD::getInstance()->height());

    // The operation survives the rotate. Console has no root page of its own - the tiles are the
    // root - so a Carry root becomes the landing category, and a Carry category becomes its tile
    if (instance->open_) {
        if (t5IsConsole(*instance)) {
            if (instance->page == T5Page::ROOT) {
                instance->page = t5CategoryPage(instance->consoleCategory);
            } else {
                for (uint8_t i = 0; i < T5_CATEGORY_COUNT; i++) {
                    if (t5CategoryPage(i) == instance->page)
                        instance->consoleCategory = i;
                }
            }
        }
        instance->pageIndex = 0; // Every row height changed, so the old page number means nothing
    }
}

// A task layer, or a system-layer surface, is in front of us
bool InkHUD::T5MenuApplet::coveredByTask() const
{
    if (T5MenuTask::isOpen() || T5ComposeTask::taskOpen() || T5TracePickTask::isOpen())
        return true;
    const T5SystemLayer *system = T5SystemLayer::getIfBegun();
    return system && system->coversContent();
}

void InkHUD::T5MenuApplet::reconcile()
{
    // While a task layer is up it draws the whole screen, so the menu does not also render - but it
    // is still the foreground logical screen, which is what open_ says
    const bool draw = open_ && !T5MenuTask::isOpen();

    // The menu claims input while it is the drawn screen, so a swipe pages the menu rather than the
    // destination still sitting underneath it, and Home is answered here rather than by that screen.
    // It stands down while a task layer is up: the task is later in the list, so dropping the claim is
    // what lets its own Home rule - cancel first, Home second - reach it (System UI 04).
    handleInput = draw;

    if (draw)
        bringToForeground();
    else
        sendToBackground();

    if (open_) {
        OSThread::enabled = true;
        OSThread::setIntervalFromNow(LIVE_REFRESH_MS);
    } else {
        OSThread::disable();
    }
}

bool InkHUD::T5MenuApplet::hasBack() const
{
    if (!t5PageBack(page))
        return false; // The Carry root
    // In Console the eight tiles are the root, so a category page is already at the top level
    return !t5IsConsole(*this) || t5PageParent(page) != T5Page::ROOT;
}

void InkHUD::T5MenuApplet::goTo(T5Page p, uint8_t channel)
{
    if (!t5PageExists(p))
        return;
    page = p;
    channelIndex = channel;
    pageIndex = 0;
    errorRow = T5Act::NONE; // A different page's rows cannot carry this one's failure
    if (t5IsConsole(*this)) {
        for (uint8_t i = 0; i < T5_CATEGORY_COUNT; i++) {
            if (t5CategoryPage(i) == p)
                consoleCategory = i;
        }
    }
    requestUpdate();
}

// Back goes one level up, and only exists where hasBack() says so: from a category to the root in
// Carry, and from a sub-page to its category. The root has no Back - the nav and Home are the ways
// out of the menu (Menu Settings 14, touch regions)
void InkHUD::T5MenuApplet::back()
{
    if (hasBack())
        goTo(t5PageParent(page), channelIndex);
}

// ---------------------------------------------------------------------------- geometry

InkHUD::T5MenuApplet::Metrics InkHUD::T5MenuApplet::metrics() const
{
    Metrics m{};
    if (t5IsConsole(*this)) {
        m.headerH = 64;
        m.listTop = 64;
        m.listBottom = 420; // The pane is 356 px tall; the nav takes 420..508
        m.sectionH = 32;
        m.rowH = 64;
        m.rowHintH = 64; // Console drops hints: they move into the confirmation
        m.categoryH = TILE_H;
        m.infoH = 44;
        m.actionSlot = 72;
        m.pagerH = 64;
        m.paneLeft = TILES_WIDTH + 2;
        m.paneRight = width();
    } else {
        m.headerH = 100;
        m.listTop = 100;
        m.listBottom = t5NavTop(*this); // 840
        m.sectionH = 36;
        m.rowH = 72;
        m.rowHintH = 92;
        m.categoryH = 80;
        m.infoH = 48;
        m.actionSlot = 80;
        m.pagerH = 64;
        m.paneLeft = 0;
        m.paneRight = width();
    }
    return m;
}

int16_t InkHUD::T5MenuApplet::rowHeight(const T5Row &row, const Metrics &m)
{
    switch (row.kind) {
    case T5RowKind::SECTION:
        return m.sectionH;
    case T5RowKind::CATEGORY:
        return m.categoryH;
    case T5RowKind::INFO:
        return m.infoH;
    case T5RowKind::ACTION:
        return m.actionSlot;
    case T5RowKind::BANNER:
        return t5IsConsole(*this) ? 72 : 96;
    case T5RowKind::FACTS: {
        // Two columns of 66 px cells: the frozen Console grid for a read-only page (K3)
        constexpr int16_t CELL_H = 66;
        const uint8_t columns = 2;
        return (int16_t)(((row.facts.size() + columns - 1) / columns) * CELL_H);
    }
    case T5RowKind::NOTE: {
        // As many lines as the sentence needs, at the small font
        setFont(fontSmall);
        const uint16_t w = (uint16_t)(m.paneRight - m.paneLeft - 2 * T5_MARGIN);
        const size_t lines = t5WrapLines(*this, row.label, w).size();
        return (int16_t)(lines * (fontSmall.lineHeight() + 2) + 16);
    }
    default: {
        // The retry line a failed write adds needs the same room a hint does
        const bool notSaved = row.available && row.action != T5Act::NONE && row.action == errorRow;
        const bool twoLines = (notSaved || !row.hint.empty()) && !t5IsConsole(*this);
        return twoLines ? m.rowHintH : m.rowH;
    }
    }
}

// ---------------------------------------------------------------------------- drawing

void InkHUD::T5MenuApplet::drawHeader(const Metrics &m)
{
    const bool console = t5IsConsole(*this);
    const char *backLabel = t5PageBack(page);
    const std::string title = t5PageTitle(page, channelIndex);
    const std::string context = t5PageContext(page, channelIndex);
    const std::string status =
        console ? T5Applet::join(T5Applet::powerLabel(false), getTimeString()) : T5Applet::powerLabel(false);

    setFont(fontSmall);
    const int16_t statusLeft = width() - T5_MARGIN - getTextWidth(status);

    if (console) {
        printAt(width() - T5_MARGIN, (m.headerH - fontSmall.lineHeight()) / 2, status, RIGHT);

        // MENU, then the crumb for the category in the pane. A sub-page replaces the title with Back
        setFont(fontLarge);
        const int16_t titleTop = (m.headerH - fontLarge.lineHeight()) / 2;
        const std::string head = hasBack() ? std::string(T5Applet::BACK) + " " + backLabel : "MENU";
        printBold(T5_MARGIN, titleTop, head);
        int16_t x = T5_MARGIN + getTextWidth(head) + 12;

        setFont(fontMedium);
        const std::string crumb = std::string(T5Applet::FWD) + " " + title;
        const int16_t crumbTop = titleTop + fontLarge.heightAboveCursor() - fontMedium.heightAboveCursor();
        setCrop(x, 0, std::max<int16_t>(0, statusLeft - 12 - x), m.headerH);
        printBold(x, crumbTop, crumb);
        resetCrop();
        drawLine(0, m.headerH - 1, width() - 1, m.headerH - 1, BLACK);
        return;
    }

    printAt(width() - T5_MARGIN, 20, status, RIGHT);

    setFont(fontLarge);
    const std::string head = backLabel ? std::string(T5Applet::BACK) + " " + backLabel : title;
    setCrop(T5_MARGIN, 0, std::max<int16_t>(0, statusLeft - 12 - T5_MARGIN), 99);
    printBold(T5_MARGIN, 14, head);
    int16_t x = T5_MARGIN + getTextWidth(head) + 12;
    resetCrop();

    if (backLabel) {
        // Carry has room for the crumb and the page name on one line
        setFont(fontMedium);
        const int16_t nameTop = 14 + fontLarge.heightAboveCursor() - fontMedium.heightAboveCursor();
        setCrop(x, 0, std::max<int16_t>(0, statusLeft - 12 - x), 99);
        printBold(x, nameTop, title);
        resetCrop();
    }

    setFont(fontSmall);
    setCrop(T5_MARGIN, 0, std::max<int16_t>(0, width() - 2 * T5_MARGIN), 99);
    printAt(T5_MARGIN, 66, context);
    resetCrop();
    drawLine(0, 99, width() - 1, 99, BLACK);
}

// Chosen layout C: all eight categories always visible, with live summaries, so any one is one tap
// away. A tap swaps the pane and two tiles (Menu Settings 13)
void InkHUD::T5MenuApplet::drawConsoleTiles(const Metrics &m)
{
    for (uint8_t i = 0; i < T5_CATEGORY_COUNT; i++) {
        const int16_t left = (int16_t)(i % TILE_COLUMNS) * TILE_W;
        const int16_t top = m.headerH + (int16_t)(i / TILE_COLUMNS) * TILE_H;
        const bool current = t5CategoryPage(i) == page || (t5PageParent(page) != T5Page::ROOT && i == consoleCategory);

        if (current) {
            fillRect(left, top, TILE_W, TILE_H, BLACK);
            setTextColor(WHITE);
        }
        setFont(fontMedium);
        printBold(left + 12, top + 14, t5CategoryName(i));
        setFont(fontSmall);
        setCrop(left + 12, top, TILE_W - 24, TILE_H);
        printAt(left + 12, top + 14 + fontMedium.lineHeight() + 4, t5CategorySummary(i, true));
        resetCrop();
        if (current)
            setTextColor(BLACK);

        if (!current) {
            drawLine(left, top + TILE_H - 1, left + TILE_W - 1, top + TILE_H - 1, BLACK);
            if (i % TILE_COLUMNS)
                drawLine(left, top, left, top + TILE_H - 1, BLACK);
        }
    }
    // The rule between the tiles and the pane
    fillRect(TILES_WIDTH, m.headerH, 2, m.listBottom - m.headerH, BLACK);
}

void InkHUD::T5MenuApplet::drawPager(const Metrics &m, uint8_t pages)
{
    const int16_t top = m.listBottom - m.pagerH;
    drawLine(m.paneLeft, top, m.paneRight - 1, top, BLACK);
    const int16_t cellW = (m.paneRight - m.paneLeft) / 3;
    setFont(fontSmall);
    if (pageIndex > 0)
        printAt(m.paneLeft + cellW / 2, top + m.pagerH / 2, std::string(T5Applet::BACK) + " PREV", CENTER, MIDDLE);
    printAt(m.paneLeft + (m.paneRight - m.paneLeft) / 2, top + m.pagerH / 2, to_string(pageIndex + 1) + " / " + to_string(pages),
            CENTER, MIDDLE);
    if (pageIndex + 1 < pages)
        printAt(m.paneRight - cellW / 2, top + m.pagerH / 2, std::string("NEXT ") + T5Applet::FWD, CENTER, MIDDLE);
}

// ON is a filled chip, OFF an outlined one. No sliding switches: a switch on a 1-bit panel is two
// rectangles whose meaning depends on knowing which side is "on" (Menu Settings 03)
int16_t InkHUD::T5MenuApplet::drawChip(int16_t right, int16_t centerY, const char *label, bool filled)
{
    const bool console = t5IsConsole(*this);
    const int16_t w = console ? 72 : 76, h = console ? 36 : 40;
    const int16_t left = right - w, top = centerY - h / 2;
    setFont(fontSmall);
    if (filled) {
        fillRect(left, top, w, h, BLACK);
        setTextColor(WHITE);
        printThick(left + w / 2, top + h / 2, label, 2, 1);
        setTextColor(BLACK);
    } else {
        drawRect(left, top, w, h, BLACK);
        drawRect(left + 1, top + 1, w - 2, h - 2, BLACK);
        printAt(left + w / 2, top + h / 2, label, CENTER, MIDDLE);
    }
    return left;
}

// The dotted box holds the reason, in place of the control (Menu Settings 03, C0)
int16_t InkHUD::T5MenuApplet::drawReason(int16_t right, int16_t centerY, const std::string &reason)
{
    setFont(fontSmall);
    const int16_t textW = getTextWidth(reason);
    const int16_t w = textW + 24, h = t5IsConsole(*this) ? 36 : 40;
    const int16_t left = right - w, top = centerY - h / 2;
    t5DottedRect(*this, left, top, w, h);
    printAt(left + w / 2, top + h / 2, reason, CENTER, MIDDLE);
    return left;
}

void InkHUD::T5MenuApplet::drawRow(const T5Row &row, int16_t top, int16_t h, const Metrics &m)
{
    const bool console = t5IsConsole(*this);
    // The frozen save-failure state: the value is replaced by an inverted NOT SAVED and the row says
    // how to retry. Any row that writes can carry it, not just a picker row (Menu Settings 11)
    const bool notSaved = row.available && row.action != T5Act::NONE && row.action == errorRow;
    static constexpr const char *RETRY = "Could not save. Tap to try again.";
    const int16_t left = m.paneLeft + T5_MARGIN;
    const int16_t right = m.paneRight - T5_MARGIN;
    const int16_t centerY = top + h / 2;

    switch (row.kind) {

    case T5RowKind::SECTION:
        setFont(fontSmall);
        printAt(left, top + (h - fontSmall.lineHeight()) / 2 - 2, row.label);
        drawLine(m.paneLeft, top + h - 1, m.paneRight - 1, top + h - 1, BLACK);
        return;

    case T5RowKind::CATEGORY: {
        setFont(console ? fontMedium : fontLarge);
        printBold(left, top + (console ? 8 : 12), row.label);
        setFont(fontSmall);
        setCrop(left, top, std::max<int16_t>(0, right - left), h);
        printAt(left, top + (console ? 8 : 12) + (console ? fontMedium : fontLarge).lineHeight() + 4, row.summary);
        resetCrop();
        setFont(fontMedium);
        printAt(right, centerY, T5Applet::FWD, RIGHT, MIDDLE);
        drawLine(m.paneLeft, top + h - 1, m.paneRight - 1, top + h - 1, BLACK);
        return;
    }

    case T5RowKind::BANNER: {
        // Inverted block, title and one line, with a chevron: the one thing wrong right now
        fillRect(m.paneLeft, top, m.paneRight - m.paneLeft, h - 8, BLACK);
        setTextColor(WHITE);
        setFont(fontMedium);
        printBold(left, top + 10, row.label);
        setFont(fontSmall);
        setCrop(left, top, std::max<int16_t>(0, right - left - 24), h - 8);
        printAt(left, top + 10 + fontMedium.lineHeight() + 2, row.hint);
        resetCrop();
        printAt(right, top + (h - 8) / 2, T5Applet::FWD, RIGHT, MIDDLE);
        setTextColor(BLACK);
        return;
    }

    case T5RowKind::NOTE: {
        setFont(fontSmall);
        int16_t y = top + 8;
        for (const std::string &line : t5WrapLines(*this, row.label, (uint16_t)(right - left))) {
            printAt(left, y, line);
            y += fontSmall.lineHeight() + 2;
        }
        return;
    }

    case T5RowKind::INFO:
        setFont(fontSmall);
        printAt(left, top + (h - fontSmall.lineHeight()) / 2, row.label);
        printBold(right, top + (h - fontSmall.lineHeight()) / 2, row.value, RIGHT);
        return;

    case T5RowKind::FACTS: {
        // Label above value in each cell; neither is a touch target, so there is no chevron and no rule
        constexpr int16_t CELL_H = 66;
        const uint8_t columns = 2;
        const int16_t cellW = (right - left) / columns;
        for (size_t i = 0; i < row.facts.size(); i++) {
            const int16_t cl = left + (int16_t)(i % columns) * cellW;
            const int16_t ct = top + (int16_t)(i / columns) * CELL_H;
            setFont(fontSmall);
            printAt(cl, ct + 6, row.facts[i].first);
            setFont(fontMedium);
            setCrop(cl, ct, std::max<int16_t>(0, cellW - 12), CELL_H);
            printBold(cl, ct + 6 + fontSmall.lineHeight() + 2, row.facts[i].second);
            resetCrop();
        }
        return;
    }

    case T5RowKind::ACTION: {
        // A bordered box, 64 px tall inside its slot. "..." already in the label means a confirmation
        const int16_t boxH = 64, boxTop = top + (h - boxH) / 2;
        setFont(fontSmall);
        if (!row.available) {
            t5DottedRect(*this, left, boxTop, (uint16_t)(right - left), boxH);
            printAt(left + 14, boxTop + boxH / 2, row.label, LEFT, MIDDLE);
            drawReason(right - 14, boxTop + boxH / 2, row.reason);
            return;
        }
        drawRect(left, boxTop, right - left, boxH, BLACK);
        drawRect(left + 1, boxTop + 1, right - left - 2, boxH - 2, BLACK);
        printThick((left + right) / 2, boxTop + boxH / 2, row.label, 2, 1);
        return;
    }

    case T5RowKind::STATE: {
        setFont(fontMedium);
        const int16_t labelTop = row.hint.empty() || console ? centerY - fontMedium.lineHeight() / 2 : top + 12;
        int16_t labelRight = right - (console ? 72 : 76) - 12;
        if (row.tag) {
            setFont(fontSmall);
            const int16_t tagW = getTextWidth(row.tag) + 16;
            printAt(labelRight, labelTop + 4, row.tag, RIGHT);
            labelRight -= tagW;
        }
        setFont(fontMedium);
        setCrop(left, top, std::max<int16_t>(0, labelRight - left), h);
        printBold(left, labelTop, row.label);
        resetCrop();

        const int16_t controlY = row.hint.empty() || console ? centerY : top + 12 + fontMedium.lineHeight() / 2;
        if (notSaved)
            drawChip(right, controlY, "NOT SAVED", true);
        else if (row.available)
            drawChip(right, controlY, row.on ? "ON" : "OFF", row.on);
        else
            drawReason(right, controlY, row.reason);

        if (!console && (notSaved || !row.hint.empty())) {
            setFont(fontSmall);
            setCrop(left, top, std::max<int16_t>(0, right - left), h);
            printAt(left, top + 12 + fontMedium.lineHeight() + 2, notSaved ? RETRY : row.hint);
            resetCrop();
        }
        drawLine(m.paneLeft, top + h - 1, m.paneRight - 1, top + h - 1, BLACK);
        return;
    }

    case T5RowKind::CHOICE: {
        // Joined cells, the selected one inverted
        const int16_t cellW = console ? 112 : 108, cellH = console ? 44 : 48;
        const int16_t cells = (int16_t)row.options.size();
        const int16_t groupLeft = right - cells * cellW;
        const int16_t labelTop = row.hint.empty() || console ? centerY - fontMedium.lineHeight() / 2 : top + 12;
        const int16_t cellTop = (row.hint.empty() || console ? centerY : top + 12 + fontMedium.lineHeight() / 2) - cellH / 2;

        setFont(fontMedium);
        setCrop(left, top, std::max<int16_t>(0, groupLeft - 12 - left), h);
        printBold(left, labelTop, row.label);
        resetCrop();

        setFont(fontSmall);
        for (int16_t i = 0; i < cells; i++) {
            const int16_t cl = groupLeft + i * cellW;
            if (i == (int16_t)row.selected) {
                fillRect(cl, cellTop, cellW, cellH, BLACK);
                setTextColor(WHITE);
                printThick(cl + cellW / 2, cellTop + cellH / 2, row.options[i], 2, 1);
                setTextColor(BLACK);
            } else {
                drawRect(cl, cellTop, cellW, cellH, BLACK);
                printAt(cl + cellW / 2, cellTop + cellH / 2, row.options[i], CENTER, MIDDLE);
            }
        }

        if (!console && (notSaved || !row.hint.empty())) {
            setFont(fontSmall);
            setCrop(left, top, std::max<int16_t>(0, right - left), h);
            printAt(left, top + 12 + fontMedium.lineHeight() + 2, notSaved ? RETRY : row.hint);
            resetCrop();
        }
        drawLine(m.paneLeft, top + h - 1, m.paneRight - 1, top + h - 1, BLACK);
        return;
    }

    case T5RowKind::NAV:
    default: {
        const int16_t labelTop = row.hint.empty() || console ? centerY - fontMedium.lineHeight() / 2 : top + 12;
        const int16_t chevronW = row.available ? 24 : 0;
        int16_t valueRight = right - chevronW;

        if (row.available) {
            setFont(fontMedium);
            printAt(right, centerY, T5Applet::FWD, RIGHT, MIDDLE);
        }

        // The value, or the inverted NOT SAVED the frozen error row carries
        if (!row.available) {
            valueRight =
                drawReason(right, row.hint.empty() || console ? centerY : top + 12 + fontMedium.lineHeight() / 2, row.reason) -
                12;
        } else if (notSaved) {
            setFont(fontSmall);
            const int16_t w = getTextWidth("NOT SAVED") + 20, hh = console ? 36 : 40;
            const int16_t bl = valueRight - w, bt = labelTop + fontMedium.lineHeight() / 2 - hh / 2;
            fillRect(bl, bt, w, hh, BLACK);
            setTextColor(WHITE);
            printThick(bl + w / 2, bt + hh / 2, "NOT SAVED", 2, 1);
            setTextColor(BLACK);
            valueRight = bl - 12;
        } else if (!row.value.empty()) {
            setFont(fontMedium);
            printBold(valueRight, labelTop, row.value, RIGHT);
            valueRight -= getTextWidth(row.value) + 12;
        }

        if (row.tag) {
            setFont(fontSmall);
            printAt(valueRight, labelTop + 4, row.tag, RIGHT);
            valueRight -= getTextWidth(row.tag) + 16;
        }

        setFont(fontMedium);
        setCrop(left, top, std::max<int16_t>(0, valueRight - left), h);
        printBold(left, labelTop, row.label);
        resetCrop();

        setFont(fontSmall);
        if (!console && (notSaved || !row.hint.empty())) {
            setCrop(left, top, std::max<int16_t>(0, right - left), h);
            printAt(left, top + 12 + fontMedium.lineHeight() + 2, notSaved ? RETRY : row.hint);
            resetCrop();
        }
        drawLine(m.paneLeft, top + h - 1, m.paneRight - 1, top + h - 1, BLACK);
        return;
    }
    }
}

void InkHUD::T5MenuApplet::onRender(bool full)
{
    (void)full;
    const Metrics m = metrics();
    const bool console = t5IsConsole(*this);

    rowsShown.clear();
    rowTops.clear();
    rowHeights.clear();
    pagerShown = false;

    drawHeader(m);
    if (console)
        drawConsoleTiles(m);

    const std::vector<T5Row> rows = t5BuildPage(page, channelIndex, console);

    // Fixed pages filled greedily from the top, never a scroll: a page boundary that cannot move is
    // a page the user can learn (Menu Settings 12)
    std::vector<uint16_t> heights;
    heights.reserve(rows.size());
    for (const T5Row &r : rows)
        heights.push_back((uint16_t)rowHeight(r, m));

    const int16_t roomNoPager = m.listBottom - m.listTop;
    std::vector<uint16_t> starts = t5Paginate(heights, roomNoPager);
    if (starts.size() > 1) {
        // A pager costs the list its own height, so the split is recomputed against what is left
        starts = t5Paginate(heights, roomNoPager - m.pagerH);
    }
    pageCount = (uint8_t)std::max<size_t>(1, starts.size());
    if (pageIndex >= pageCount)
        pageIndex = pageCount - 1;
    pagerShown = pageCount > 1;

    const size_t first = starts.empty() ? 0 : starts[pageIndex];
    const size_t last = (pageIndex + 1 < pageCount) ? starts[pageIndex + 1] : rows.size();
    const int16_t bottom = m.listBottom - (pagerShown ? m.pagerH : 0);

    int16_t y = m.listTop;
    for (size_t i = first; i < last; i++) {
        const int16_t h = (int16_t)heights[i];
        if (y + h > bottom)
            break;
        drawRow(rows[i], y, h, m);
        rowsShown.push_back(rows[i]);
        rowTops.push_back(y);
        rowHeights.push_back(h);
        y += h;
    }

    if (pagerShown)
        drawPager(m, pageCount);

    // Nav on the root and on every category page: they are shallow, and MENU stays lit
    drawNavBar(T5_MENU);

    liveShownAtMs = Time::getMillis();
}

// ---------------------------------------------------------------------------- input

void InkHUD::T5MenuApplet::turnPage(int8_t delta)
{
    const int16_t next = (int16_t)pageIndex + delta;
    if (next < 0 || next >= (int16_t)pageCount)
        return;
    pageIndex = (uint8_t)next;
    requestUpdate();
}

void InkHUD::T5MenuApplet::onNavUp()
{
    if (open_ && !coveredByTask())
        turnPage(-1);
}

void InkHUD::T5MenuApplet::onNavDown()
{
    if (open_ && !coveredByTask())
        turnPage(1);
}

// Home from a menu page is absolute: the menu is a destination, not a task, so there is no layer to
// peel first. Back is the only one-level-up control, and it lives in the header (Menu Settings 27)
void InkHUD::T5MenuApplet::onExitShort()
{
    if (!open_)
        return;
    open_ = false;
    reconcile();
    const int8_t home = t5DestApplet(T5_HOME);
    if (home >= 0)
        InkHUD::getInstance()->showApplet((uint8_t)home, Drivers::EInk::UpdateTypes::UNSPECIFIED, true);
    else
        requestUpdate(); // No Home screen to go to: at least stop covering whatever is there
}

bool InkHUD::T5MenuApplet::onTouchPoint(uint16_t x, uint16_t y, bool longPress)
{
    if (!open_)
        return false;

    // A task layer or a system-layer surface is in front: its targets win, and its owner has them
    if (coveredByTask())
        return false;

    if (longPress)
        return true; // Long press is unused in the menu, and never falls through to the button path

    const int16_t tx = (int16_t)x - getTile()->getLeft();
    const int16_t ty = (int16_t)y - getTile()->getTop();
    const Metrics m = metrics();

    // The nav keeps its six cells on every menu page
    if (t5HandleNavTap(*this, (uint16_t)tx, (uint16_t)ty))
        return true;
    if (ty >= t5NavTop(*this))
        return true; // The Console mode slot: consumed, never a cell

    // Header Back: one level up, and from the root out of the menu
    if (ty < m.headerH) {
        const int16_t backWidth = t5IsConsole(*this) ? 260 : 200;
        if (tx < backWidth && hasBack())
            back();
        return true;
    }

    // Console tiles: a tap swaps the pane
    if (t5IsConsole(*this) && tx < TILES_WIDTH && ty < m.listBottom) {
        const uint8_t column = (uint8_t)(tx / TILE_W);
        const uint8_t rowIdx = (uint8_t)((ty - m.headerH) / TILE_H);
        const uint8_t index = (uint8_t)(rowIdx * TILE_COLUMNS + column);
        if (index < T5_CATEGORY_COUNT) {
            consoleCategory = index;
            goTo(t5CategoryPage(index));
        }
        return true;
    }

    // The pager
    if (pagerShown && ty >= m.listBottom - m.pagerH && ty < m.listBottom && tx >= m.paneLeft) {
        const int16_t cellW = (m.paneRight - m.paneLeft) / 3;
        if (tx < m.paneLeft + cellW)
            turnPage(-1);
        else if (tx >= m.paneRight - cellW)
            turnPage(1);
        return true;
    }

    // A row. The whole width is the target, except that a choice row's cells are separate targets
    for (size_t i = 0; i < rowsShown.size(); i++) {
        if (ty < rowTops[i] || ty >= rowTops[i] + rowHeights[i])
            continue;
        if (tx < m.paneLeft)
            return true;

        const T5Row &row = rowsShown[i];
        int16_t cell = -1;
        if (row.kind == T5RowKind::CHOICE && row.available) {
            const int16_t cellW = t5IsConsole(*this) ? 112 : 108;
            const int16_t groupLeft = m.paneRight - T5_MARGIN - (int16_t)row.options.size() * cellW;
            if (tx >= groupLeft)
                cell = (tx - groupLeft) / cellW;
        }
        runRow(row, cell);
        return true;
    }

    return true; // The menu consumes every tap inside it
}

void InkHUD::T5MenuApplet::applyNow(T5Act which, int32_t value)
{
    const T5ApplyResult result = t5Apply(which, value, channelIndex);
    if (result == T5ApplyResult::FAILED) {
        errorRow = which;
    } else if (errorRow == which) {
        errorRow = T5Act::NONE;
    }
    // A restart-class apply has already scheduled the reboot and raised the applying band; the
    // terminal frame belongs to T5SystemLayer, as it has since Phase 3. A new value, or a failure to
    // save one, is the model changing: while the snapshot gate is set that stays logical, and the
    // wake reconcile is what puts it on the panel (Menu Settings 30)
    if (result != T5ApplyResult::RESTARTING && !T5UiGate::snapshotHeld())
        requestUpdate();
}

void InkHUD::T5MenuApplet::runRow(const T5Row &row, int16_t choiceCell)
{
    if (!row.available || row.action == T5Act::NONE)
        return; // An INFO row, a note, and an unavailable row: ignored, and no redraw

    switch (row.action) {

    case T5Act::OPEN_RADIO:
        goTo(T5Page::RADIO);
        return;
    case T5Act::OPEN_CHANNELS:
        goTo(T5Page::CHANNELS);
        return;
    case T5Act::OPEN_POSITION:
        goTo(T5Page::POSITION);
        return;
    case T5Act::OPEN_CONNECTIVITY:
        goTo(T5Page::CONNECTIVITY);
        return;
    case T5Act::OPEN_DISPLAY:
        goTo(T5Page::SCREEN);
        return;
    case T5Act::OPEN_POWER:
        goTo(T5Page::POWER);
        return;
    case T5Act::OPEN_DEVICE:
        goTo(T5Page::DEVICE);
        return;
    case T5Act::OPEN_ABOUT:
        goTo(T5Page::ABOUT);
        return;
    case T5Act::OPEN_DEVTOOLS:
        goTo(T5Page::DEVTOOLS);
        return;
    case T5Act::OPEN_CHANNEL:
        goTo(T5Page::CHANNEL, (uint8_t)row.arg);
        return;

    // Pickers and fields are task layers
    case T5Act::PICK_REGION:
    case T5Act::PICK_PRESET:
    case T5Act::PICK_HOP:
    case T5Act::PICK_ROLE:
    case T5Act::PICK_SLEEP:
    case T5Act::PICK_BROADCAST:
    case T5Act::PICK_PRECISION:
    case T5Act::PICK_TZ:
        T5MenuTask::openPicker(row.action, channelIndex);
        reconcile();
        return;

    case T5Act::EDIT_LONG_NAME:
        T5MenuTask::openNameField(false);
        reconcile();
        return;
    case T5Act::EDIT_SHORT_NAME:
        T5MenuTask::openNameField(true);
        reconcile();
        return;

    default:
        break;
    }

    // A choice row writes the cell that was tapped, and tapping the selected cell changes nothing
    int32_t value = row.arg;
    if (row.kind == T5RowKind::CHOICE) {
        if (choiceCell < 0 || choiceCell >= (int16_t)row.options.size())
            return;
        if ((uint8_t)choiceCell == row.selected)
            return;
        value = choiceCell;
    }

    // Confirm if it restarts, cuts you off, or destroys data
    T5Confirm spec;
    if (t5NeedsConfirm(row.action, value, channelIndex, spec)) {
        T5MenuTask::openConfirm(spec, row.action, value);
        reconcile();
        return;
    }

    applyNow(row.action, value);
}

// ---------------------------------------------------------------------------- task results

void InkHUD::T5MenuApplet::pickChosen(T5Act which, int32_t value)
{
    if (!instance)
        return;
    instance->reconcile();

    T5Confirm spec;
    if (t5NeedsConfirm(which, value, instance->channelIndex, spec)) {
        // The picker itself never writes config: a risky one ends in a confirmation (C3)
        T5MenuTask::openConfirm(spec, which, value);
        instance->reconcile();
        return;
    }
    instance->applyNow(which, value);
}

void InkHUD::T5MenuApplet::pickCancelled()
{
    if (!instance)
        return;
    instance->reconcile();
    instance->requestUpdate(); // The page underneath is exactly as it was; it only has to be drawn
}

void InkHUD::T5MenuApplet::confirmed(T5Act which, int32_t value)
{
    if (!instance)
        return;
    instance->reconcile();
    instance->applyNow(which, value);
}

void InkHUD::T5MenuApplet::confirmCancelled()
{
    if (!instance)
        return;
    instance->reconcile();
    instance->requestUpdate(); // CANCEL returns to the originating page, not to the picker
}

void InkHUD::T5MenuApplet::nameEdited(bool shortName, const std::string &utf8)
{
    if (!instance)
        return;
    instance->reconcile();
    const T5Act which = shortName ? T5Act::EDIT_SHORT_NAME : T5Act::EDIT_LONG_NAME;
    if (t5SetOwnerName(shortName, utf8) == T5ApplyResult::FAILED)
        instance->errorRow = which;
    else if (instance->errorRow == which)
        instance->errorRow = T5Act::NONE;
    instance->requestUpdate();
}

// The LIVE rows refresh at most once a minute, only while the menu is actually visible, and never
// while a surface is covering it: a redraw only this screen wants would erase what is on top of it
int32_t InkHUD::T5MenuApplet::runOnce()
{
    // While the display timeout has the UI gated the base frame is a snapshot. System applets are
    // rendered even then - that is how the lock band appears - so the menu must not ask: no menu
    // timer wakes the panel, and the wake reconcile draws the one current frame (Menu Settings 30)
    if (T5UiGate::snapshotHeld())
        return LIVE_REFRESH_MS;

    if (!open_ || !isForeground() || coveredByTask())
        return LIVE_REFRESH_MS;

    // Only the RADIO page and the developer counters have values that move on their own
    if (page == T5Page::RADIO || page == T5Page::DEVTOOLS || page == T5Page::ROOT) {
        if (!Throttle::isWithinTimespanMs(liveShownAtMs, LIVE_REFRESH_MS))
            requestUpdate();
    }
    return LIVE_REFRESH_MS;
}

#endif
