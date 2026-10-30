#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5MessagesApplet.h"
#include "./T5ComposeTask.h"
#include "./T5ConversationApplet.h"
#include "./T5SystemLayer.h"

#include "mesh/NodeDB.h"

#include <algorithm>

using namespace NicheGraphics;

namespace
{
// Frozen Carry geometry (Messaging Final 02, 13)
constexpr int16_t CARRY_HEADER_H = 79;
constexpr int16_t CARRY_LABEL_H = 36;
constexpr int16_t CARRY_ROW_H = 84;
constexpr int16_t CARRY_CONTENT_TOP = 80;
constexpr int16_t CARRY_PAGER_H = 64;
constexpr int16_t CARRY_TAG_COLUMN = 112; // Where the title and preview start
constexpr int16_t CARRY_TITLE_W = 250;
constexpr int16_t CARRY_PREVIEW_W = 262;

// Frozen Console geometry
constexpr int16_t CONSOLE_HEADER_H = 63;
constexpr int16_t CONSOLE_LIST_W = 300;
constexpr int16_t CONSOLE_ROW_H = 71;
constexpr int16_t CONSOLE_CONTENT_TOP = 64;
constexpr int16_t CONSOLE_CONTENT_BOTTOM = 419;
constexpr int16_t CONSOLE_PANE_LEFT = 302;
constexpr int16_t CONSOLE_STRIP_H = 52;
constexpr int16_t CONSOLE_STRIP_TOUCH_BOTTOM = 127;
constexpr int16_t CONSOLE_THREAD_TOP = 116;
constexpr int16_t CONSOLE_THREAD_BOTTOM = 350;
constexpr int16_t CONSOLE_ACTION_TOP = 356;
constexpr uint8_t CONSOLE_ROWS_PER_PAGE = 5;

const char *const NO_DMS_TITLE = "No direct messages yet";
} // namespace

bool InkHUD::T5MessagesApplet::open()
{
    InkHUD *hud = InkHUD::getInstance();
    const int8_t i = indexOf("Messages");
    return i >= 0 && hud->showApplet(i, EInk::UpdateTypes::UNSPECIFIED, true);
}

// The one entry point every deep link uses: Home's cards, the MSGS cell, and
// the system layer's DM banner
bool InkHUD::T5MessagesApplet::open(const Convo &c)
{
    T5MessageModel *model = T5MessageModel::get();
    if (!model)
        return false;

    // Carry opens the conversation screen itself; Console has no separate screen,
    // only the selection
    const bool console = InkHUD::getInstance()->width() > InkHUD::getInstance()->height();
    if (!console && c.valid())
        return T5ConversationApplet::open(c);

    model->selected = c;
    model->clearTag();
    const int8_t i = indexOf("Messages");
    if (i < 0)
        return false;
    T5MessagesApplet *applet = static_cast<T5MessagesApplet *>(InkHUD::getInstance()->userApplets[i]);
    if (c.valid())
        applet->enter(applet->thread, c);
    applet->firstItem = 0;
    applet->firstRow = 0;
    return InkHUD::getInstance()->showApplet(i, EInk::UpdateTypes::UNSPECIFIED, true);
}

void InkHUD::T5MessagesApplet::onActivate()
{
    setInputsSubscribed(NAV_UP | NAV_DOWN, true); // Vertical swipes page the list
}

void InkHUD::T5MessagesApplet::onDeactivate()
{
    setInputsSubscribed(NAV_UP | NAV_DOWN, false);
}

void InkHUD::T5MessagesApplet::onForeground()
{
    T5Applet::onForeground();
    if (T5MessageModel *model = T5MessageModel::get())
        model->clearTag(); // The list itself is a Messaging screen: the frozen tag
                           // belongs to deeper ones
}

void InkHUD::T5MessagesApplet::onMessagingChanged()
{
    const T5SystemLayer *system = T5SystemLayer::getIfBegun();
    if (isForeground() && !(system && system->coversContent()) && !T5MessageModel::taskIsForeground())
        requestUpdate();
}

// ---------------------------------------------------------------------------------------------
// The list, shared by both modes
// ---------------------------------------------------------------------------------------------

std::vector<InkHUD::T5MessagesApplet::Item> InkHUD::T5MessagesApplet::buildItems()
{
    std::vector<Item> items;
    T5MessageModel *model = T5MessageModel::get();
    if (!model)
        return items;

    const std::vector<Convo> all = model->conversations();
    uint16_t channelCount = 0, dmCount = 0;
    for (const Convo &c : all)
        (c.isChannel() ? channelCount : dmCount)++;

    Item label;
    label.kind = Item::LABEL;
    label.label = "CHANNELS";
    label.count = channelCount;
    label.channelSection = true;
    items.push_back(label);
    for (const Convo &c : all) {
        if (!c.isChannel())
            continue;
        Item row;
        row.convo = c;
        items.push_back(row);
    }

    label.label = "DIRECT";
    label.count = dmCount;
    label.channelSection = false;
    items.push_back(label);
    if (dmCount == 0) {
        Item empty;
        empty.kind = Item::EMPTY_DM;
        items.push_back(empty);
    } else {
        for (const Convo &c : all) {
            if (c.isChannel())
                continue;
            Item row;
            row.convo = c;
            items.push_back(row);
        }
    }
    return items;
}

uint16_t InkHUD::T5MessagesApplet::itemHeight(const Item &item)
{
    switch (item.kind) {
    case Item::LABEL:
        return CARRY_LABEL_H;
    case Item::EMPTY_DM:
        return 160; // The frozen dashed box: a title line and a wrapped hint, which
                    // is text, not a target
    default:
        return CARRY_ROW_H;
    }
}

// Mirrors drawBadge's choice of badge, in the small font it draws in
uint16_t InkHUD::T5MessagesApplet::badgeWidth(const Convo &c, bool suppress)
{
    T5MessageModel *model = T5MessageModel::get();
    if (!model || suppress)
        return 0;
    setFont(fontSmall);
    if (const uint16_t unread = model->unread(c))
        return getTextWidth((unread > 99 ? std::string("99+") : to_string(unread)) + " NEW") + 16;
    const StoredMessage *m = model->newest(c);
    const T5MessageModel::Outgoing *o = m ? model->outgoing(m) : nullptr;
    if (o && (o->state == T5MessageModel::State::FAILED || o->state == T5MessageModel::State::NOT_HEARD))
        return getTextWidth(o->state == T5MessageModel::State::FAILED ? "FAILED" : "NOT HEARD") + 14;
    return model->draft(c) ? getTextWidth("DRAFT") + 14 : 0;
}

// n NEW inverted, then a FAILED / NOT HEARD outline, then a dashed DRAFT - one
// slot, in that order
void InkHUD::T5MessagesApplet::drawBadge(int16_t right, int16_t top, const Convo &c, bool suppress)
{
    T5MessageModel *model = T5MessageModel::get();
    if (!model || suppress)
        return;

    setFont(fontSmall);
    const uint16_t unread = model->unread(c);
    if (unread) {
        const std::string label = (unread > 99 ? std::string("99+") : to_string(unread)) + " NEW";
        const int16_t w = getTextWidth(label) + 16, h = fontSmall.lineHeight() + 6;
        fillRect(right - w, top, w, h, BLACK);
        setTextColor(WHITE);
        printBold(right - 8, top + 3, label, RIGHT);
        setTextColor(BLACK);
        return;
    }

    const StoredMessage *m = model->newest(c);
    const T5MessageModel::Outgoing *o = m ? model->outgoing(m) : nullptr;
    if (o && (o->state == T5MessageModel::State::FAILED || o->state == T5MessageModel::State::NOT_HEARD)) {
        const char *label = o->state == T5MessageModel::State::FAILED ? "FAILED" : "NOT HEARD";
        const int16_t w = getTextWidth(label) + 14, h = fontSmall.lineHeight() + 6;
        drawRect(right - w, top, w, h, BLACK);
        drawRect(right - w + 1, top + 1, w - 2, h - 2, BLACK);
        printBold(right - 7, top + 3, label, RIGHT);
        return;
    }

    if (model->draft(c)) {
        const int16_t w = getTextWidth("DRAFT") + 14, h = fontSmall.lineHeight() + 6;
        // 1 px dashed, so a draft reads as the lightest of the three badges
        for (int16_t x = right - w; x < right; x += 4) {
            drawPixel(x, top, BLACK);
            drawPixel(x, top + h - 1, BLACK);
        }
        for (int16_t y = top; y < top + h; y += 4) {
            drawPixel(right - w, y, BLACK);
            drawPixel(right - 1, y, BLACK);
        }
        printAt(right - 7, top + 3, "DRAFT", RIGHT);
    }
}

void InkHUD::T5MessagesApplet::drawRow(int16_t top, uint16_t w, const Convo &c, bool console, bool selected)
{
    T5MessageModel *model = T5MessageModel::get();
    const int16_t rowH = console ? CONSOLE_ROW_H : CARRY_ROW_H;
    const int16_t right = (int16_t)w;

    if (selected) {
        fillRect(0, top, w, rowH, BLACK);
        setTextColor(WHITE);
    }

    const StoredMessage *m = model ? model->newest(c) : nullptr;
    const std::string time = m ? t5ListTime(m->timestamp, m->isBootRelative) : "";

    if (console) {
        // Tag and title share the first line; the preview sits under them
        setFont(fontMedium);
        const std::string tag = t5ConvoTag(c);
        printBold(MARGIN, top + 4, tag);
        const int16_t titleLeft = MARGIN + getTextWidth(tag) + 10;
        setFont(fontSmall);
        const int16_t timeW = time.empty() ? 0 : getTextWidth(time) + 12;
        printAt(right - 14, top + 8, time, RIGHT);
        printAt(titleLeft, top + 12, t5Elide(*this, t5ConvoTitle(c), std::max<int16_t>(0, right - 14 - timeW - titleLeft)));
        const uint16_t badgeW = badgeWidth(c, selected);
        setFont(fontSmall);
        const int16_t previewW = std::min<int16_t>(240, right - 14 - (badgeW ? badgeW + 12 : 0) - MARGIN);
        printAt(MARGIN, top + 42, t5Elide(*this, t5ConvoPreview(c), std::max<int16_t>(0, previewW)));
        if (selected)
            setTextColor(BLACK); // The selected row's badge is suppressed while its
                                 // thread is shown
        drawBadge(right - 14, top + 40, c, selected);
        drawLine(0, top + rowH - 1, right - 1, top + rowH - 1, BLACK);
        setTextColor(BLACK);
        return;
    }

    // A tag wider than its column steps down to small bold rather than pushing the title (Panel Fit D7)
    printShortName(MARGIN, top + 8, t5ConvoTag(c), CARRY_TAG_COLUMN - 12 - MARGIN);
    const int16_t column = CARRY_TAG_COLUMN;
    const uint16_t badgeW = badgeWidth(c, false);
    setFont(fontSmall);
    const int16_t previewW = std::min<int16_t>(CARRY_PREVIEW_W, right - 40 - (badgeW ? badgeW + 12 : 0) - column);
    printAt(column, top + 20, t5Elide(*this, t5ConvoTitle(c), CARRY_TITLE_W));
    printAt(column, top + 52, t5Elide(*this, t5ConvoPreview(c), std::max<int16_t>(0, previewW)));
    printAt(right - 40, top + 14, time, RIGHT);
    drawBadge(right - 40, top + 44, c, false);
    setFont(fontMedium);
    printAt(right - 14, top + rowH / 2, FWD, RIGHT, MIDDLE);
    drawLine(0, top + rowH - 1, right - 1, top + rowH - 1, BLACK);
    setTextColor(BLACK);
}

// The frozen dashed box. Text, not a target: it explains how to start a first
// conversation.
void InkHUD::T5MessagesApplet::drawEmptyDMBox(int16_t left, int16_t top, uint16_t w)
{
    const int16_t h = EMPTY_DM_H - 20;
    for (int16_t x = left; x < left + (int16_t)w; x += 4) {
        drawPixel(x, top, BLACK);
        drawPixel(x, top + h - 1, BLACK);
    }
    for (int16_t y = top; y < top + h; y += 4) {
        drawPixel(left, y, BLACK);
        drawPixel(left + (int16_t)w - 1, y, BLACK);
    }
    setFont(fontMedium);
    printBold(left + 18, top + 16, NO_DMS_TITLE);
    setFont(fontSmall);
    int16_t y = top + 16 + fontMedium.lineHeight() + 8;
    for (const std::string &line : t5WrapLines(*this,
                                               "Open a node in NODES and choose COMPOSE MESSAGE, or tap "
                                               "COMPOSE above.",
                                               w - 36)) {
        printAt(left + 18, y, line);
        y += fontSmall.lineHeight();
    }
}

void InkHUD::T5MessagesApplet::drawHeader(uint16_t unread, uint16_t conversations, uint8_t page, uint8_t pages)
{
    const int16_t right = width() - MARGIN;
    const bool console = isConsole();
    const int16_t bandH = console ? CONSOLE_HEADER_H : CARRY_HEADER_H;

    // COMPOSE, boxed, at the right of the band. Struck through with no radio
    // configured: the picker cannot open, so the tap is consumed and nothing
    // happens (Messaging Final 05).
    setFont(fontSmall);
    const bool canCompose = T5ComposeTask::available();
    const int16_t boxW = getTextWidth("COMPOSE") + 2 * 14 + 4, boxH = fontSmall.lineHeight() + 2 * 8 + 4;
    const int16_t boxLeft = right - boxW, boxTop = (bandH - boxH) / 2;
    composeLeft = boxLeft - 12; // The frozen 64 px minimum, grown a little around the drawn box
    drawRect(boxLeft, boxTop, boxW, boxH, BLACK);
    drawRect(boxLeft + 1, boxTop + 1, boxW - 2, boxH - 2, BLACK);
    printBold(boxLeft + boxW / 2, boxTop + 2 + 8, "COMPOSE", CENTER);
    if (!canCompose)
        strikeThrough(boxLeft, boxTop + boxH / 2, boxW);

    const std::string unreadText = unread ? to_string(unread) + " UNREAD" : "NO UNREAD";
    if (console) {
        setFont(fontLarge);
        const int16_t titleTop = (bandH - (fontLarge.heightAboveCursor() + fontLarge.heightBelowCursor())) / 2;
        printBold(MARGIN, titleTop, "MESSAGES");
        int16_t x = MARGIN + getTextWidth("MESSAGES") + 28;
        const int16_t metaTop = titleTop + fontLarge.heightAboveCursor() - fontSmall.heightAboveCursor();
        setFont(fontSmall);
        printBold(x, metaTop, unreadText);
        x += getTextWidth(unreadText) + 28;
        const std::string count = to_string(conversations) + (conversations == 1 ? " CONVERSATION" : " CONVERSATIONS");
        setCrop(x, 0, std::max<int16_t>(0, boxLeft - 16 - x), bandH);
        printAt(x, metaTop, pages > 1 ? "PAGE " + to_string(page + 1) + " / " + to_string(pages) : count);
        resetCrop();
        drawLine(0, bandH, width() - 1, bandH, BLACK);
        return;
    }

    // Carry: the three frozen header items do not fit on one line at the sizes
    // this panel has, so the meta drops to a second line rather than being cut
    // into nonsense.
    setFont(fontMedium);
    printBold(MARGIN, 4, "MESSAGES");
    setFont(fontSmall);
    std::string meta = unreadText;
    if (pages > 1)
        meta = join(meta, "PAGE " + to_string(page + 1) + " / " + to_string(pages));
    setCrop(MARGIN, 44, std::max<int16_t>(0, boxLeft - 16 - MARGIN), 35);
    printBold(MARGIN, 48, meta);
    resetCrop();
    drawLine(0, bandH, width() - 1, bandH, BLACK);
}

void InkHUD::T5MessagesApplet::drawPager(uint8_t page, uint8_t pages)
{
    const int16_t top = navTop() - 1 - CARRY_PAGER_H;
    const int16_t half = width() / 2;
    const int16_t mid = top + CARRY_PAGER_H / 2;
    drawLine(0, top, width() - 1, top, BLACK);
    drawLine(half, top, half, top + CARRY_PAGER_H - 1, BLACK);

    setFont(fontSmall);
    const bool canPrev = page > 0, canNext = page + 1 < pages;
    const int16_t prevW = getTextWidth("PREV"), nextW = getTextWidth("NEXT");
    const int16_t prevX = (half - prevW) / 2, nextX = half + (half - nextW) / 2;
    if (canPrev)
        drawTriangle(prevX - 20, mid, true);
    printBold(prevX, mid - fontSmall.lineHeight() / 2, "PREV");
    if (!canPrev)
        strikeThrough(prevX - 14, mid, prevW + 28);
    printBold(nextX, mid - fontSmall.lineHeight() / 2, "NEXT");
    if (canNext)
        drawTriangle(nextX + nextW + 20, mid, false);
    else
        strikeThrough(nextX - 14, mid, nextW + 28);

    // The page number sits on the top rule, in a gap punched out of it
    const std::string label = to_string(page + 1) + " / " + to_string(pages);
    const int16_t labelW = getTextWidth(label) + 20;
    fillRect(half - labelW / 2, top - 1, labelW, 3, WHITE);
    printAt(half, top - fontSmall.lineHeight() / 2, label, CENTER);
}

// ---------------------------------------------------------------------------------------------
// Carry
// ---------------------------------------------------------------------------------------------

void InkHUD::T5MessagesApplet::renderCarry()
{
    T5MessageModel *model = T5MessageModel::get();
    const std::vector<Item> items = buildItems();

    uint16_t conversations = 0;
    for (const Item &item : items)
        conversations += item.kind == Item::ROW;

    // One page if everything fits; otherwise the pager strip takes the bottom
    // band and pages are measured
    const int16_t fullBottom = navTop() - 1;
    int16_t total = 0;
    for (const Item &item : items)
        total += itemHeight(item);
    pagerShown = total > fullBottom - CARRY_CONTENT_TOP;
    const int16_t bottom = pagerShown ? fullBottom - CARRY_PAGER_H - 1 : fullBottom;

    std::vector<uint16_t> heights;
    heights.reserve(items.size());
    for (const Item &item : items)
        heights.push_back(itemHeight(item));
    pageStarts = t5Paginate(heights, bottom - CARRY_CONTENT_TOP);

    uint8_t page = 0;
    for (uint8_t p = 0; p < pageStarts.size(); p++) {
        if (firstItem >= pageStarts[p])
            page = p;
    }
    firstItem = pageStarts[page];

    drawHeader(model ? model->unread() : 0, conversations, page, pageStarts.size());

    shown.clear();
    shownY.clear();
    int16_t y = CARRY_CONTENT_TOP;
    for (uint16_t i = firstItem; i < items.size(); i++) {
        const Item &item = items[i];
        if (y + (int16_t)itemHeight(item) > bottom && i > firstItem)
            break;

        shown.push_back(item);
        shownY.push_back(y);
        if (item.kind == Item::LABEL) {
            setFont(fontSmall);
            // A section continued from the previous page says so, so the first row on
            // a page is never ambiguous (Messaging Final 02)
            const bool continued = i == firstItem && firstItem > 0;
            printBold(MARGIN, y + 8, continued ? item.label + std::string(" ") + SEP + " CONTINUED" : item.label);
            printBold(width() - MARGIN, y + 8, to_string(item.count), RIGHT);
        } else if (item.kind == Item::EMPTY_DM) {
            drawEmptyDMBox(MARGIN, y + 10, width() - 2 * MARGIN);
        } else {
            drawRow(y, width(), item.convo, false, false);
        }
        y += itemHeight(item);
    }

    if (pagerShown)
        drawPager(page, pageStarts.size());
    drawNav();
}

// ---------------------------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------------------------

// The split always has something selected: the newest unread conversation, else
// the newest one, else the DM placeholder, which is an invalid Convo (Messaging
// Final 09)
void InkHUD::T5MessagesApplet::ensureSelection()
{
    T5MessageModel *model = T5MessageModel::get();
    if (!model)
        return;
    const std::vector<Convo> all = model->conversations();
    if (model->selected.valid() && std::find(all.begin(), all.end(), model->selected) != all.end())
        return;

    const Convo unread = model->newestUnread();
    if (unread.valid() && std::find(all.begin(), all.end(), unread) != all.end()) {
        selectConsoleRow(unread);
        return;
    }
    // The newest conversation by its own newest message, channels included
    const Convo *best = nullptr;
    uint32_t bestAt = 0;
    for (const Convo &c : all) {
        const StoredMessage *m = model->newest(c);
        if (m && (!best || m->timestamp > bestAt)) {
            best = &c;
            bestAt = m->timestamp;
        }
    }
    selectConsoleRow(best ? *best : Convo());
}

void InkHUD::T5MessagesApplet::selectConsoleRow(const Convo &c)
{
    T5MessageModel *model = T5MessageModel::get();
    if (!model)
        return;
    model->selected = c;
    if (c.valid())
        enter(thread, c);
    else
        thread = Thread(); // The DM placeholder: the pane explains the absence
                           // instead of drawing a thread
}

void InkHUD::T5MessagesApplet::renderConsole()
{
    T5MessageModel *model = T5MessageModel::get();
    ensureSelection();

    // The pane shows the selection's newest page, so it is read before the header
    // states a count: the two must not disagree inside one frame (Messaging Final
    // 01)
    if (model && model->selected.valid() && thread.page == 0)
        model->markSeen(model->selected);

    const std::vector<Convo> all = model ? model->conversations() : std::vector<Convo>();
    uint16_t channelCount = 0, dmCount = 0;
    for (const Convo &c : all)
        (c.isChannel() ? channelCount : dmCount)++;

    // The list: channels, then the DM group. With no DMs the group label is
    // itself the selectable row which explains the absence (Messaging Final 12,
    // X9).
    listRows.clear();
    for (const Convo &c : all) {
        if (c.isChannel())
            listRows.push_back(c);
    }
    const uint16_t groupAt = listRows.size();
    if (dmCount == 0)
        listRows.push_back(Convo()); // The placeholder
    for (const Convo &c : all) {
        if (!c.isChannel())
            listRows.push_back(c);
    }

    const uint8_t pages = (listRows.size() + CONSOLE_ROWS_PER_PAGE - 1) / CONSOLE_ROWS_PER_PAGE;
    firstRow -= firstRow % CONSOLE_ROWS_PER_PAGE;
    if (firstRow >= listRows.size())
        firstRow = listRows.empty() ? 0 : (listRows.size() - 1) / CONSOLE_ROWS_PER_PAGE * CONSOLE_ROWS_PER_PAGE;

    drawHeader(model ? model->unread() : 0, all.size(), firstRow / CONSOLE_ROWS_PER_PAGE, pages ? pages : 1);

    for (uint8_t i = 0; i < CONSOLE_ROWS_PER_PAGE && firstRow + i < listRows.size(); i++) {
        const uint16_t index = firstRow + i;
        const int16_t top = CONSOLE_CONTENT_TOP + i * CONSOLE_ROW_H;
        const Convo &c = listRows[index];
        const bool selected = model && model->selected == c;

        // The 3 px rule which separates the two groups, drawn on the boundary row's
        // top edge
        if (index == groupAt && dmCount > 0)
            fillRect(0, top, CONSOLE_LIST_W, 3, BLACK);

        if (!c.valid()) {
            if (selected)
                fillRect(0, top, CONSOLE_LIST_W, CONSOLE_ROW_H, BLACK);
            fillRect(0, top, CONSOLE_LIST_W, 3, BLACK);
            setTextColor(selected ? WHITE : BLACK);
            setFont(fontMedium);
            printBold(MARGIN, top + 2, "DIRECT");
            setFont(fontSmall);
            printAt(MARGIN, top + 42, "No conversations yet");
            setTextColor(BLACK);
            drawLine(0, top + CONSOLE_ROW_H - 1, CONSOLE_LIST_W - 1, top + CONSOLE_ROW_H - 1, BLACK);
            continue;
        }
        drawRow(top, CONSOLE_LIST_W, c, true, selected);
    }

    // The rule between list and pane
    fillRect(CONSOLE_LIST_W, CONSOLE_CONTENT_TOP, 2, CONSOLE_CONTENT_BOTTOM - CONSOLE_CONTENT_TOP, BLACK);

    const uint16_t paneW = width() - CONSOLE_PANE_LEFT;
    if (model && model->selected.valid()) {
        drawThread(thread, CONSOLE_PANE_LEFT + MARGIN, CONSOLE_THREAD_TOP, paneW - 2 * MARGIN,
                   CONSOLE_THREAD_BOTTOM - CONSOLE_THREAD_TOP);
        drawPagingStrip(thread, CONSOLE_PANE_LEFT, CONSOLE_CONTENT_TOP, paneW, CONSOLE_STRIP_H);

        drawLine(CONSOLE_PANE_LEFT, CONSOLE_ACTION_TOP, width() - 1, CONSOLE_ACTION_TOP, BLACK);
        setFont(fontMedium);
        std::string label = model->draft(model->selected) ? std::string("REPLY ") + SEP + " DRAFT" : "REPLY";
        printBold(CONSOLE_PANE_LEFT + paneW / 2,
                  CONSOLE_ACTION_TOP + (CONSOLE_CONTENT_BOTTOM - CONSOLE_ACTION_TOP - fontMedium.lineHeight()) / 2, label,
                  CENTER);
    } else {
        setFont(fontMedium);
        printBold(CONSOLE_PANE_LEFT + 24, 150, NO_DMS_TITLE);
        setFont(fontSmall);
        int16_t y = 150 + fontMedium.lineHeight() + 10;
        for (const std::string &line : t5WrapLines(*this,
                                                   "Start one below, or open a node in NODES and choose "
                                                   "COMPOSE MESSAGE.",
                                                   paneW - 48)) {
            printAt(CONSOLE_PANE_LEFT + 24, y, line);
            y += fontSmall.lineHeight();
        }
        drawLine(CONSOLE_PANE_LEFT, CONSOLE_ACTION_TOP, width() - 1, CONSOLE_ACTION_TOP, BLACK);
        setFont(fontMedium);
        printBold(CONSOLE_PANE_LEFT + paneW / 2,
                  CONSOLE_ACTION_TOP + (CONSOLE_CONTENT_BOTTOM - CONSOLE_ACTION_TOP - fontMedium.lineHeight()) / 2,
                  "NEW DIRECT MESSAGE", CENTER);
    }

    drawNav();
}

void InkHUD::T5MessagesApplet::onRender(bool full)
{
    (void)full;
    if (isConsole())
        renderConsole();
    else
        renderCarry();
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

bool InkHUD::T5MessagesApplet::onTouchPoint(uint16_t x, uint16_t y, bool longPress)
{
    if (longPress)
        return true; // No T5 screen assigns a long press, and the stock fallback
                     // would open the shared menu

    const int16_t tx = x - getTile()->getLeft();
    const int16_t ty = y - getTile()->getTop();

    if (handleNavTap(tx, ty))
        return true;

    const bool console = isConsole();
    const int16_t bandH = console ? CONSOLE_HEADER_H : CARRY_HEADER_H;

    // COMPOSE occupies the right of the header band in both modes
    if (ty < bandH) {
        if (tx >= composeLeft)
            T5ComposeTask::openPicker();
        return true;
    }

    if (console) {
        T5MessageModel *model = T5MessageModel::get();
        if (tx < CONSOLE_LIST_W) {
            const uint16_t slot = (ty - CONSOLE_CONTENT_TOP) / CONSOLE_ROW_H;
            if (ty >= CONSOLE_CONTENT_TOP && firstRow + slot < listRows.size()) {
                selectConsoleRow(listRows[firstRow + slot]);
                if (model)
                    model->clearTag();
                requestUpdate();
            }
            return true;
        }

        if (model && !model->selected.valid()) {
            if (ty >= CONSOLE_ACTION_TOP && ty < CONSOLE_CONTENT_BOTTOM)
                T5ComposeTask::openPicker();
            return true;
        }
        if (retryTapped(thread, tx, ty)) {
            requestUpdate();
            return true;
        }
        if (stripTapped(thread, CONSOLE_PANE_LEFT, CONSOLE_CONTENT_TOP, width() - CONSOLE_PANE_LEFT,
                        CONSOLE_STRIP_TOUCH_BOTTOM - CONSOLE_CONTENT_TOP, tx, ty))
            return true;
        if (ty >= CONSOLE_ACTION_TOP && ty < CONSOLE_CONTENT_BOTTOM && model)
            T5ComposeTask::openReply(model->selected);
        return true;
    }

    // Carry: a row opens its conversation; the pager strip pages the list
    if (pagerShown && ty >= navTop() - 1 - CARRY_PAGER_H && ty < navTop()) {
        if (tx < width() / 2)
            onNavUp();
        else
            onNavDown();
        return true;
    }

    for (uint8_t i = 0; i < shown.size(); i++) {
        const int16_t top = shownY[i];
        if (ty < top || ty >= top + (int16_t)itemHeight(shown[i]))
            continue;
        if (shown[i].kind == Item::ROW)
            T5ConversationApplet::open(shown[i].convo);
        break; // A label and the dashed box are text, not targets: the tap stops
               // here
    }

    // Consume every tap: the button fallback would cycle applets
    return true;
}

// Swipes page the list, with the direction Nodes already uses: down moves
// forward
void InkHUD::T5MessagesApplet::onNavDown()
{
    if (isConsole()) {
        const uint16_t next = firstRow - firstRow % CONSOLE_ROWS_PER_PAGE + CONSOLE_ROWS_PER_PAGE;
        if (next < listRows.size()) {
            firstRow = next;
            requestUpdate();
        }
        return;
    }
    for (uint8_t p = 0; p + 1 < pageStarts.size(); p++) {
        if (pageStarts[p] == firstItem) {
            firstItem = pageStarts[p + 1];
            requestUpdate();
            return;
        }
    }
}

void InkHUD::T5MessagesApplet::onNavUp()
{
    if (isConsole()) {
        if (firstRow < CONSOLE_ROWS_PER_PAGE)
            return;
        firstRow -= firstRow % CONSOLE_ROWS_PER_PAGE + CONSOLE_ROWS_PER_PAGE;
        requestUpdate();
        return;
    }
    for (uint8_t p = 1; p < pageStarts.size(); p++) {
        if (pageStarts[p] == firstItem) {
            firstItem = pageStarts[p - 1];
            requestUpdate();
            return;
        }
    }
}

#endif
