#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5ComposeTask.h"
#include "./T5Applet.h"
#include "./T5ConversationApplet.h"
#include "./T5MessagesApplet.h"
#include "./T5NodeDetailApplet.h"
#include "./T5NodesApplet.h"
#include "./T5SystemLayer.h"

#include "Throttle.h"
#include "UptimeClock.h"
#include "mesh/Channels.h"
#include "mesh/NodeDB.h"

#include <algorithm>

using namespace NicheGraphics;

namespace
{
InkHUD::T5ComposeTask *instance = nullptr;

// Frozen Carry composer geometry (Messaging Final 04, 13)
constexpr int16_t CARRY_HEADER_H = 99;
constexpr int16_t CARRY_CONTEXT_TOP = 112, CARRY_CONTEXT_RULE = 184;
constexpr int16_t CARRY_FIELD_TOP = 194, CARRY_FIELD_LINES = 8;
constexpr int16_t CARRY_CONTROL_RULE = 491, CARRY_CONTROL_BOTTOM = 571;
constexpr int16_t CARRY_SEND_LEFT = 342, CARRY_SEND_TOP = 503, CARRY_SEND_W = 150, CARRY_SEND_H = 56;
constexpr int16_t CARRY_SEND_HIT_LEFT = 330; // The frozen 64 px minimum, grown around the visible button
constexpr int16_t CARRY_CANCEL_HIT_LEFT = 340;

// Frozen Console composer geometry
constexpr int16_t CONSOLE_HEADER_H = 63;
constexpr int16_t CONSOLE_FIELD_TOP = 74, CONSOLE_FIELD_LINES = 3, CONSOLE_FIELD_W = 746;
constexpr int16_t CONSOLE_CONTEXT_LEFT = 352, CONSOLE_CONTEXT_W = 420;
constexpr int16_t CONSOLE_CANCEL_HIT_LEFT = 760;

// Frozen picker geometry
constexpr int16_t PICK_HEADER_H = 79;
constexpr int16_t PICK_LABEL_H = 36;
constexpr int16_t PICK_ROW_H = 84;
constexpr int16_t PICK_CONSOLE_ROW_H = 71;
constexpr int16_t PICK_CONSOLE_SPLIT = 464; // Channels left, nodes right
constexpr int16_t PICK_PAGER_H = 64;
} // namespace

InkHUD::T5ComposeTask::T5ComposeTask()
{
    (new Tile)->assignApplet(this);
}

void InkHUD::T5ComposeTask::begin()
{
    assert(!instance);
    instance = new T5ComposeTask;
    instance->name = "T5Compose";
    instance->activate();

    // Before the T5 system layer, so the layer still draws its card over us and
    // still takes input ahead of nothing: our claim is checked first, and we hand
    // the press on while a surface covers content.
    InkHUD::getInstance()->systemApplets.push_back(instance);
    instance->getTile()->setRegion(0, 0, InkHUD::getInstance()->width(), InkHUD::getInstance()->height());

    if (T5MessageModel *model = T5MessageModel::get())
        model->attach(instance);
}

InkHUD::T5ComposeTask *InkHUD::T5ComposeTask::get()
{
    return instance;
}

// A channel always exists on a configured radio, so this is the frozen "radio
// not configured" test: with nothing to write to, COMPOSE is struck through and
// the picker cannot open (Messaging Final 05).
bool InkHUD::T5ComposeTask::available()
{
    T5MessageModel *model = T5MessageModel::get();
    return model && !model->enabledChannels().empty();
}

bool InkHUD::T5ComposeTask::taskOpen()
{
    return instance && instance->mode != Mode::CLOSED;
}

bool InkHUD::T5ComposeTask::composerOpen()
{
    return instance && instance->mode == Mode::COMPOSER;
}

bool InkHUD::T5ComposeTask::pickingNode()
{
    return instance && instance->mode == Mode::NODE_LIST;
}

// ---------------------------------------------------------------------------------------------
// Opening and closing
// ---------------------------------------------------------------------------------------------

void InkHUD::T5ComposeTask::reconcile()
{
    // NODE_LIST hands the screen to T5NodesApplet but keeps the input claim, so
    // one Home press still closes the whole task rather than only the list
    // (Messaging Final 10).
    handleInput = mode != Mode::CLOSED;
    if (mode == Mode::PICKER || mode == Mode::COMPOSER)
        bringToForeground();
    else
        sendToBackground();
}

void InkHUD::T5ComposeTask::openPicker()
{
    if (!instance || !available())
        return;
    instance->mode = Mode::PICKER;
    instance->origin = Origin::MESSAGES;
    instance->firstItem = 0;
    if (T5MessageModel *model = T5MessageModel::get())
        model->clearTag();
    instance->reconcile();
    instance->requestUpdate();
}

void InkHUD::T5ComposeTask::openReply(const Convo &c)
{
    if (!instance || !c.valid())
        return;
    instance->openComposer(c, Origin::CONVERSATION);
}

void InkHUD::T5ComposeTask::openForNode(NodeNum num)
{
    T5MessageModel *model = T5MessageModel::get();
    if (!instance || !model || !model->canMessage(num))
        return;
    instance->nodeOrigin = num;
    instance->openComposer(Convo::forPeer(num), Origin::NODE_DETAIL);
}

void InkHUD::T5ComposeTask::openComposer(const Convo &c, Origin from)
{
    T5MessageModel *model = T5MessageModel::get();
    mode = Mode::COMPOSER;
    origin = from;
    destination = c;

    // This destination's draft, if it has one: a draft survives being left and
    // come back to
    typed.clear();
    if (model) {
        if (const std::string *draft = model->draft(c))
            typed = *draft;
        model->clearTag();
    }
    layer = T5Keyboard::Layer::LETTERS;
    armShift();
    reconcile();
    requestUpdate();
}

// The frozen auto-arm: the start of a message, and after a sentence ends
void InkHUD::T5ComposeTask::armShift()
{
    const bool atStart = typed.empty();
    const bool afterSentence =
        typed.size() >= 2 && typed.back() == ' ' &&
        (typed[typed.size() - 2] == '.' || typed[typed.size() - 2] == '?' || typed[typed.size() - 2] == '!');
    if (shift != T5Keyboard::Shift::CAPS)
        shift = (atStart || afterSentence) ? T5Keyboard::Shift::ONCE : T5Keyboard::Shift::OFF;
}

void InkHUD::T5ComposeTask::close(bool keepDraft)
{
    T5MessageModel *model = T5MessageModel::get();
    if (model && mode == Mode::COMPOSER) {
        // CANCEL keeps what was typed, without a confirmation; an empty field
        // simply drops any old draft
        if (keepDraft && !typed.empty())
            model->setDraft(destination, typed);
        else
            model->clearDraft(destination);
    }
    typed.clear();
    mode = Mode::CLOSED;
    if (model)
        model->clearTag();
    reconcile();
}

// Where CANCEL and the first Home press land (Messaging Final 04, 10)
void InkHUD::T5ComposeTask::leaveTo(Origin target)
{
    const Convo was = destination;
    const NodeNum node = nodeOrigin;
    close(true);

    switch (target) {
    case Origin::NODE_DETAIL:
        if (node && T5NodeDetailApplet::open(node))
            return;
        break;
    case Origin::CONVERSATION:
        if (was.valid() && T5MessagesApplet::open(was))
            return;
        break;
    case Origin::MESSAGES:
        break;
    }
    T5MessagesApplet::open();
}

void InkHUD::T5ComposeTask::onExitShort()
{
    // A card or a terminal state sits on top of us, so it peels first: the
    // arbiter owns that decision, and we are only earlier in the input list
    // (System UI 04).
    T5SystemLayer *system = T5SystemLayer::getIfBegun();
    if (system && system->coversContent()) {
        system->onExitShort();
        return;
    }

    switch (mode) {
    case Mode::COMPOSER:
        leaveTo(origin);
        break;
    case Mode::NODE_LIST:
    case Mode::PICKER:
        leaveTo(Origin::MESSAGES); // The whole task closes: the frozen rule peels
                                   // the task, not its layers
        break;
    case Mode::CLOSED:
        break;
    }
}

void InkHUD::T5ComposeTask::backToPicker()
{
    if (!instance || instance->mode != Mode::NODE_LIST)
        return;
    instance->mode = Mode::PICKER;
    instance->reconcile();
    instance->requestUpdate();
}

void InkHUD::T5ComposeTask::nodePicked(NodeNum num)
{
    if (!instance || instance->mode != Mode::NODE_LIST)
        return;
    instance->openComposer(Convo::forPeer(num), Origin::MESSAGES);
}

void InkHUD::T5ComposeTask::onModeChanged()
{
    getTile()->setRegion(0, 0, inkhud->width(), inkhud->height());
    firstItem = 0; // The frozen simplification: the picker's page resets, because
                   // the geometry differs
}

// Only the "n NEW" tag can change under a task, and only the header band
// carries it (Messaging Final 08)
void InkHUD::T5ComposeTask::onMessagingChanged()
{
    if (mode == Mode::PICKER || mode == Mode::COMPOSER)
        requestUpdate();
}

// ---------------------------------------------------------------------------------------------
// Editing (Messaging Final 07)
// ---------------------------------------------------------------------------------------------

uint16_t InkHUD::T5ComposeTask::bytesLeft() const
{
    const size_t used = t5Utf8Size(typed);
    return used >= T5MessageModel::MAX_TEXT_BYTES ? 0 : T5MessageModel::MAX_TEXT_BYTES - used;
}

void InkHUD::T5ComposeTask::type(char c)
{
    // A character whose bytes would cross the limit is refused: the field does
    // not change, and the counter is already inverted. No sound, no flash.
    const std::string candidate = typed + c;
    if (t5Utf8Size(candidate) > T5MessageModel::MAX_TEXT_BYTES)
        return;

    typed = candidate;
    if (shift == T5Keyboard::Shift::ONCE)
        shift = T5Keyboard::Shift::OFF;
    armShift();
}

void InkHUD::T5ComposeTask::backspace(bool word)
{
    if (typed.empty())
        return;
    if (!word) {
        typed.pop_back();
    } else {
        // The previous whole word, once. A long press does not repeat, so a held
        // finger cannot wipe several words (Messaging Final 07).
        while (!typed.empty() && typed.back() == ' ')
            typed.pop_back();
        while (!typed.empty() && typed.back() != ' ')
            typed.pop_back();
    }
    armShift();
}

void InkHUD::T5ComposeTask::toggleShift()
{
    const uint32_t now = Time::getMillis();
    if (shift == T5Keyboard::Shift::CAPS) {
        shift = T5Keyboard::Shift::OFF;
    } else if (lastShiftTapMs && now - lastShiftTapMs <= SHIFT_DOUBLE_TAP_MS) {
        shift = T5Keyboard::Shift::CAPS; // A double tap inside the frozen window
                                         // locks it
    } else {
        shift = T5Keyboard::Shift::ONCE;
    }
    lastShiftTapMs = now;
}

// SEND closes the composer at once and the message appears in the thread as
// SENDING...; every later outcome is shown there. The composer never waits on
// the radio (Messaging Final 04, 21).
void InkHUD::T5ComposeTask::send()
{
    T5MessageModel *model = T5MessageModel::get();
    if (!model || typed.empty())
        return;

    const Convo target = destination;
    if (!model->send(target, t5ToUtf8(typed)))
        return; // The radio would not take it: the composer stays, with the draft
                // intact

    model->clearDraft(target);
    typed.clear();
    mode = Mode::CLOSED;
    reconcile();
    T5MessagesApplet::open(target); // Land on the conversation, wherever it lives in this mode
}

// ---------------------------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------------------------

void InkHUD::T5ComposeTask::boxed(int16_t left, int16_t top, uint16_t w, uint16_t h, const std::string &label, bool filled,
                                  bool struck)
{
    if (filled) {
        fillRect(left, top, w, h, BLACK);
        setTextColor(WHITE);
    } else {
        drawRect(left, top, w, h, BLACK);
        drawRect(left + 1, top + 1, w - 2, h - 2, BLACK);
    }
    printBold(left + w / 2, top + (h - getFont().lineHeight()) / 2, label, CENTER);
    setTextColor(BLACK);
    if (struck)
        fillRect(left + 14, top + h / 2 - 1, w - 28, 2, BLACK);
}

void InkHUD::T5ComposeTask::drawCancel()
{
    const bool console = width() > height();
    setFont(Applet::fontSmall);
    const int16_t w = getTextWidth("CANCEL") + 2 * 16 + 4, h = fontSmall.lineHeight() + 2 * 12 + 4;
    const int16_t left = width() - T5Applet::MARGIN - w;
    const int16_t top = console ? (CONSOLE_HEADER_H - h) / 2 : 24;
    boxed(left, top, w, h, "CANCEL", false, false);
}

// The frozen indicator for a message which arrived while the task is up:
// inverted, and not tappable, because tapping it would throw away the context
// the user is in (Messaging Final 08)
void InkHUD::T5ComposeTask::drawNewTag(int16_t left, int16_t top)
{
    T5MessageModel *model = T5MessageModel::get();
    if (!model || !model->tag())
        return;
    setFont(Applet::fontSmall);
    const std::string label = to_string(model->tag()) + " NEW";
    const int16_t w = getTextWidth(label) + 16, h = fontSmall.lineHeight() + 6;
    fillRect(left, top, w, h, BLACK);
    setTextColor(WHITE);
    printBold(left + w / 2, top + 3, label, CENTER);
    setTextColor(BLACK);
}

void InkHUD::T5ComposeTask::drawSend(bool enabled)
{
    setFont(Applet::fontMedium);
    // Filled while there is something to send, a struck-through outline while the
    // field is empty. The empty tap is consumed and costs no refresh.
    boxed(CARRY_SEND_LEFT, CARRY_SEND_TOP, CARRY_SEND_W, CARRY_SEND_H, "SEND", enabled, !enabled);
}

// Hidden above 40 bytes left, then "n BYTES LEFT", inverted at zero (Messaging
// Final 04)
void InkHUD::T5ComposeTask::drawCounter()
{
    const uint16_t left = bytesLeft();
    if (left > COUNTER_FROM)
        return;

    setFont(Applet::fontSmall);
    const std::string label = to_string(left) + " BYTES LEFT";
    const bool console = width() > height();
    const int16_t x = console ? T5Keyboard::CONSOLE_RAIL_LEFT + 12 : T5Applet::MARGIN;
    const int16_t y = console ? 110 : 514;

    if (left == 0) {
        const int16_t w = getTextWidth(label) + 20, h = fontSmall.lineHeight() + 12;
        fillRect(x, y, w, h, BLACK);
        setTextColor(WHITE);
        printBold(x + 10, y + 6, label);
        setTextColor(BLACK);
        return;
    }
    printBold(x, y, label);
}

void InkHUD::T5ComposeTask::drawKeyboard()
{
    const bool console = width() > height();
    const int16_t rulesTop = console ? T5Keyboard::CONSOLE_TOP : T5Keyboard::CARRY_TOP;
    const int16_t keysRight = console ? T5Keyboard::CONSOLE_KEYS_RIGHT : width();

    fillRect(0, rulesTop, width(), 2, BLACK); // The 2 px rule above the rows

    for (const T5Keyboard::Cell &cell : T5Keyboard::cells(console, layer, shift)) {
        if (cell.boundaryLeft)
            fillRect(cell.left, cell.top, 1, cell.bottom - cell.top - 1, BLACK);
        setFont(cell.small ? Applet::fontSmall : Applet::fontMedium);
        if (cell.sub.empty()) {
            printAt((cell.left + cell.right) / 2, (cell.top + cell.bottom) / 2, cell.label, CENTER, MIDDLE);
        } else {
            // SHIFT is the one key that says what it will do, which is its only
            // visual difference
            const int16_t mid = (cell.top + cell.bottom) / 2;
            printBold((cell.left + cell.right) / 2, mid - fontSmall.lineHeight(), cell.label, CENTER);
            printAt((cell.left + cell.right) / 2, mid + 2, cell.sub, CENTER, TOP);
        }
    }

    // Row boundaries, drawn after the keys so a legend cannot sit on one. The
    // rows are not all the same height, so the rules come from the frozen table
    // rather than from a step.
    const int16_t *rowTops = T5Keyboard::rowTops(console);
    for (uint8_t r = 1; r < 4; r++)
        fillRect(0, rowTops[r] - 1, keysRight, 1, BLACK);

    if (!console)
        return;

    // The Console rail: DEL over SEND, beside the rows rather than above them
    fillRect(T5Keyboard::CONSOLE_RAIL_LEFT, T5Keyboard::CONSOLE_ROWS_TOP, 2, height() - T5Keyboard::CONSOLE_ROWS_TOP, BLACK);
    fillRect(T5Keyboard::CONSOLE_RAIL_LEFT, T5Keyboard::CONSOLE_RAIL_SPLIT - 1, width() - T5Keyboard::CONSOLE_RAIL_LEFT, 1,
             BLACK);
    setFont(Applet::fontSmall);
    printAt((T5Keyboard::CONSOLE_RAIL_LEFT + width()) / 2, (T5Keyboard::CONSOLE_ROWS_TOP + T5Keyboard::CONSOLE_RAIL_SPLIT) / 2,
            "DEL", CENTER, MIDDLE);
    const bool enabled = !typed.empty();
    if (enabled) {
        fillRect(T5Keyboard::CONSOLE_RAIL_LEFT + 2, T5Keyboard::CONSOLE_RAIL_SPLIT, width() - T5Keyboard::CONSOLE_RAIL_LEFT - 2,
                 height() - T5Keyboard::CONSOLE_RAIL_SPLIT, BLACK);
        setTextColor(WHITE);
    }
    printBold((T5Keyboard::CONSOLE_RAIL_LEFT + width()) / 2, (T5Keyboard::CONSOLE_RAIL_SPLIT + height()) / 2 - 10, "SEND",
              CENTER);
    setTextColor(BLACK);
    if (!enabled)
        fillRect(T5Keyboard::CONSOLE_RAIL_LEFT + 20, (T5Keyboard::CONSOLE_RAIL_SPLIT + height()) / 2 - 1,
                 width() - T5Keyboard::CONSOLE_RAIL_LEFT - 40, 2, BLACK);
}

// The draft, wrapped live, with a 3 px cursor bar after the last glyph. The
// cursor is always at the end and cannot be moved (Messaging Final 07).
void InkHUD::T5ComposeTask::drawField(int16_t left, int16_t top, uint16_t w, uint8_t maxLines, AppletFont font)
{
    setFont(font);
    std::vector<std::string> lines = t5WrapLines(*this, typed, w);

    // Console shows the last three lines, with a leading ellipsis, when a draft
    // runs past the field
    bool clipped = false;
    if (lines.size() > maxLines) {
        lines.erase(lines.begin(), lines.end() - maxLines);
        clipped = true;
    }

    int16_t y = top;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string text = (clipped && i == 0) ? std::string("\x85") + lines[i] : lines[i];
        printAt(left, y, text);
        if (i + 1 == lines.size())
            fillRect(left + getTextWidth(text) + 2, y + 2, 3, font.lineHeight() - 4, BLACK);
        y += font.lineHeight();
    }
    if (lines.empty() || (lines.size() == 1 && lines[0].empty()))
        fillRect(left, top + 2, 3, font.lineHeight() - 4,
                 BLACK); // Empty field: the caret alone
}

// ---------------------------------------------------------------------------------------------
// Composer
// ---------------------------------------------------------------------------------------------

// "REPLYING TO · sender · time" plus the newest incoming line, or the frozen
// warnings for a node with no history, a stale one, or one that is no longer in
// the node list (Messaging Final 04)
std::string InkHUD::T5ComposeTask::contextLine(std::string *second)
{
    second->clear();
    T5MessageModel *model = T5MessageModel::get();
    if (!model)
        return "";

    const StoredMessage *newest = nullptr;
    for (const StoredMessage *m : model->messages(destination)) {
        if (m->sender != nodeDB->getNodeNum())
            newest = m;
    }

    if (newest) {
        const std::string who = t5NodeTag(newest->sender);
        const std::string when = t5ListTime(newest->timestamp, newest->isBootRelative);
        *second = t5Parse(MessageStore::getText(*newest));
        return T5Applet::join(std::string("REPLYING TO ") + T5Applet::SEP + " " + who, when);
    }

    if (destination.isChannel()) {
        // No history on a channel: its privacy line stands in, so the header still
        // says who can read this
        const meshtastic_Channel &ch = channels.getByIndex(destination.channel);
        return ch.settings.psk.size == 0                        ? "NO KEY \xB7 SENT IN THE CLEAR"
               : channels.isDefaultChannel(destination.channel) ? "DEFAULT KEY \xB7 READABLE BY ANY NODE"
                                                                : "PRIVATE KEY";
    }

    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(destination.peer);
    if (!node) {
        *second = "NOT IN NODE LIST \x97 delivery unlikely";
        return "NO EARLIER MESSAGES";
    }
    if (node->last_heard && sinceLastSeen(node) > 2 * 60 * 60) {
        // Stale by the firmware's own two-hour window. Sending is still allowed
        // (Messaging Final 04)
        *second = T5Applet::join("Last heard " + t5ListTime(node->last_heard, false),
                                 node->has_hops_away ? T5Applet::hopsString(node->hops_away) : "") +
                  " \x97 may be slow";
    }
    return "NO EARLIER MESSAGES";
}

void InkHUD::T5ComposeTask::renderComposer()
{
    const bool console = width() > height();
    const int16_t right = width() - T5Applet::MARGIN;
    const std::string tag = t5ConvoTag(destination);
    const std::string title = t5ConvoTitle(destination);
    const std::string kind =
        destination.isChannel() ? "TO \xB7 CHANNEL " + to_string(destination.channel) : "TO \xB7 DIRECT MESSAGE";

    std::string contextSecond;
    const std::string context = contextLine(&contextSecond);

    if (console) {
        // One band: TO, the tag and long name, then the context beside them behind
        // a 2 px rule
        setFont(Applet::fontSmall);
        printBold(T5Applet::MARGIN, 18, "TO");
        int16_t x = T5Applet::MARGIN + getTextWidth("TO") + 12;
        setFont(Applet::fontMedium);
        printBold(x, 10, tag);
        x += getTextWidth(tag) + 12;
        setFont(Applet::fontSmall);
        printAt(x, 20, t5Elide(*this, title, std::max<int16_t>(0, CONSOLE_CONTEXT_LEFT - 16 - x)));

        fillRect(CONSOLE_CONTEXT_LEFT, 8, 2, CONSOLE_HEADER_H - 16, BLACK);
        printAt(CONSOLE_CONTEXT_LEFT + 12, 10, t5Elide(*this, context, CONSOLE_CONTEXT_W - 12));
        if (!contextSecond.empty())
            printAt(CONSOLE_CONTEXT_LEFT + 12, 34, t5Elide(*this, contextSecond, CONSOLE_CONTEXT_W - 12));
        drawCancel();
        drawLine(0, CONSOLE_HEADER_H, width() - 1, CONSOLE_HEADER_H, BLACK);

        drawField(T5Applet::MARGIN, CONSOLE_FIELD_TOP, CONSOLE_FIELD_W, CONSOLE_FIELD_LINES, Applet::fontSmall);

        // The rail top carries the only two status items a composer has, neither of
        // them a target
        fillRect(T5Keyboard::CONSOLE_RAIL_LEFT, CONSOLE_HEADER_H + 1, 2, T5Keyboard::CONSOLE_TOP - CONSOLE_HEADER_H, BLACK);
        drawNewTag(T5Keyboard::CONSOLE_RAIL_LEFT + 12, 74);
        drawCounter();
        drawKeyboard();
        return;
    }

    // Carry: two header lines, the context band, the field, then the control row
    // over the keyboard
    setFont(Applet::fontSmall);
    printBold(T5Applet::MARGIN, 14, kind);
    drawNewTag(230, 12);
    setFont(Applet::fontMedium);
    printBold(T5Applet::MARGIN, 40, tag);
    const int16_t titleLeft = T5Applet::MARGIN + getTextWidth(tag) + 16;
    setFont(Applet::fontSmall);
    printAt(titleLeft, 52, t5Elide(*this, title, std::max<int16_t>(0, CARRY_CANCEL_HIT_LEFT - 12 - titleLeft)));
    drawCancel();
    drawLine(0, CARRY_HEADER_H, width() - 1, CARRY_HEADER_H, BLACK);

    // Context: a 2 px rule, the frozen line, and the newest incoming message
    // under it
    fillRect(T5Applet::MARGIN, CARRY_CONTEXT_TOP, 2, CARRY_CONTEXT_RULE - CARRY_CONTEXT_TOP - 12, BLACK);
    const int16_t contextLeft = T5Applet::MARGIN + 2 + 12;
    printBold(contextLeft, CARRY_CONTEXT_TOP, t5Elide(*this, context, right - contextLeft));
    if (!contextSecond.empty())
        printAt(contextLeft, CARRY_CONTEXT_TOP + 28, t5Elide(*this, contextSecond, right - contextLeft));
    for (int16_t x = 0; x < width(); x += 3)
        drawPixel(x, CARRY_CONTEXT_RULE, BLACK);

    drawField(T5Applet::MARGIN, CARRY_FIELD_TOP, width() - 2 * T5Applet::MARGIN, CARRY_FIELD_LINES, Applet::fontMedium);

    drawLine(0, CARRY_CONTROL_RULE, width() - 1, CARRY_CONTROL_RULE, BLACK);
    drawCounter();
    drawSend(!typed.empty());
    drawKeyboard();
}

// ---------------------------------------------------------------------------------------------
// Recipient picker (Messaging Final 05)
// ---------------------------------------------------------------------------------------------

// Favourites first by last heard, then the nodes we have a DM with by last
// message, de-duplicated and capped at four. Never ourselves, never an ignored
// node.
std::vector<NodeNum> InkHUD::T5ComposeTask::recentNodes()
{
    T5MessageModel *model = T5MessageModel::get();
    std::vector<NodeNum> out;
    if (!model)
        return out;

    std::vector<meshtastic_NodeInfoLite *> favourites;
    for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (node && nodeInfoLiteIsFavorite(node) && model->canMessage(node->num))
            favourites.push_back(node);
    }
    std::stable_sort(
        favourites.begin(), favourites.end(),
        [](const meshtastic_NodeInfoLite *a, const meshtastic_NodeInfoLite *b) { return a->last_heard > b->last_heard; });
    for (const meshtastic_NodeInfoLite *node : favourites) {
        if (out.size() >= RECENT_CAP)
            return out;
        out.push_back(node->num);
    }

    for (const Convo &c : model->conversations()) {
        if (out.size() >= RECENT_CAP)
            break;
        if (c.isChannel() || !model->canMessage(c.peer))
            continue;
        if (std::find(out.begin(), out.end(), c.peer) == out.end())
            out.push_back(c.peer);
    }
    return out;
}

std::string InkHUD::T5ComposeTask::pickMeta(const Convo &c)
{
    if (c.isChannel()) {
        const meshtastic_Channel &ch = channels.getByIndex(c.channel);
        const bool primary = c.channel == channels.getPrimaryIndex();
        std::string line = primary ? "Primary" : "Channel " + to_string(c.channel);
        return T5Applet::join(line, ch.settings.psk.size == 0              ? "no key \xB7 sent in the clear"
                                    : channels.isDefaultChannel(c.channel) ? "default key \xB7 readable by any node"
                                                                           : "private key");
    }

    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(c.peer);
    if (!node)
        return "no node info";
    std::string line;
    if (nodeInfoLiteIsFavorite(node))
        line = "FAVOURITE";
    if (node->last_heard)
        line = T5Applet::join(line, "heard " + t5ListTime(node->last_heard, false));
    if (node->has_hops_away)
        line = T5Applet::join(line, T5Applet::hopsString(node->hops_away));
    return line.empty() ? "no node info" : line;
}

void InkHUD::T5ComposeTask::drawPickRow(int16_t left, int16_t top, uint16_t w, const Convo &c, bool console)
{
    const int16_t rowH = console ? PICK_CONSOLE_ROW_H : PICK_ROW_H;
    const int16_t right = left + (int16_t)w;

    setFont(Applet::fontMedium);
    const std::string tag = t5ConvoTag(c);
    printBold(left + T5Applet::MARGIN, top + (console ? 4 : 8), tag);
    const int16_t column = console ? left + T5Applet::MARGIN + getTextWidth(tag) + 12 : left + 112;

    setFont(Applet::fontSmall);
    printAt(column, top + (console ? 12 : 20), t5Elide(*this, t5ConvoTitle(c), std::max<int16_t>(0, right - 40 - column)));
    printAt(left + (console ? T5Applet::MARGIN : 112), top + (console ? 42 : 52),
            t5Elide(*this, pickMeta(c), std::max<int16_t>(0, right - 40 - (console ? left + T5Applet::MARGIN : left + 112))));
    setFont(Applet::fontMedium);
    printAt(right - 14, top + rowH / 2, T5Applet::FWD, RIGHT, MIDDLE);
    drawLine(left, top + rowH - 1, right - 1, top + rowH - 1, BLACK);
}

void InkHUD::T5ComposeTask::renderPicker()
{
    T5MessageModel *model = T5MessageModel::get();
    const bool console = width() > height();
    const int16_t bandH = console ? CONSOLE_HEADER_H : PICK_HEADER_H;

    setFont(Applet::fontMedium);
    printBold(T5Applet::MARGIN, console ? 10 : 14, "NEW MESSAGE");
    const int16_t tagLeft = T5Applet::MARGIN + getTextWidth("NEW MESSAGE") + 16;
    setFont(Applet::fontSmall);
    drawNewTag(tagLeft, console ? 18 : 22);
    drawCancel();
    drawLine(0, bandH, width() - 1, bandH, BLACK);

    const std::vector<uint8_t> channelList = model ? model->enabledChannels() : std::vector<uint8_t>();
    const std::vector<NodeNum> recent = recentNodes();

    pickRows.clear();
    pickRowTops.clear();
    allNodesShown = false;
    pickPagerShown = false;

    if (console) {
        // Two columns: the channels on the left, the nodes on the right (Messaging
        // Final 05)
        setFont(Applet::fontSmall);
        printBold(T5Applet::MARGIN, bandH + 10, "TO A CHANNEL");
        printBold(PICK_CONSOLE_SPLIT + T5Applet::MARGIN + 2, bandH + 10, "TO A NODE");
        fillRect(PICK_CONSOLE_SPLIT, bandH + 1, 2, height() - bandH - 1, BLACK);

        int16_t y = bandH + PICK_LABEL_H;
        for (uint8_t index : channelList) {
            if (y + PICK_CONSOLE_ROW_H > height())
                break;
            const Convo c = Convo::forChannel(index);
            pickRows.push_back(c);
            pickRowTops.push_back(y);
            drawPickRow(0, y, PICK_CONSOLE_SPLIT, c, true);
            y += PICK_CONSOLE_ROW_H;
        }

        y = bandH + PICK_LABEL_H;
        for (NodeNum num : recent) {
            if (y + PICK_CONSOLE_ROW_H > height() - PICK_CONSOLE_ROW_H)
                break;
            const Convo c = Convo::forPeer(num);
            pickRows.push_back(c);
            pickRowTops.push_back(y);
            drawPickRow(PICK_CONSOLE_SPLIT + 2, y, width() - PICK_CONSOLE_SPLIT - 2, c, true);
            y += PICK_CONSOLE_ROW_H;
        }
        if (recent.empty()) {
            printAt(PICK_CONSOLE_SPLIT + T5Applet::MARGIN + 2, y + 10, "No other nodes heard yet");
            printAt(PICK_CONSOLE_SPLIT + T5Applet::MARGIN + 2, y + 10 + fontSmall.lineHeight() + 4,
                    "You can still write to a channel.");
            y += PICK_CONSOLE_ROW_H;
        }

        allNodesShown = true;
        allNodesTop = height() - PICK_CONSOLE_ROW_H;
        const bool anyNode = !recent.empty();
        setFont(Applet::fontMedium);
        printBold(PICK_CONSOLE_SPLIT + T5Applet::MARGIN + 2, allNodesTop + 20, "ALL NODES");
        setFont(Applet::fontMedium);
        printAt(width() - 14, allNodesTop + PICK_CONSOLE_ROW_H / 2, T5Applet::FWD, RIGHT, MIDDLE);
        if (!anyNode) {
            // Nothing to choose from: the row keeps its place, struck through, and
            // its tap is consumed
            setFont(Applet::fontMedium);
            const int16_t w = getTextWidth("ALL NODES");
            fillRect(PICK_CONSOLE_SPLIT + T5Applet::MARGIN + 2, allNodesTop + 20 + fontMedium.lineHeight() / 2 - 1, w, 2, BLACK);
        }
        drawLine(PICK_CONSOLE_SPLIT + 2, allNodesTop, width() - 1, allNodesTop, BLACK);
        return;
    }

    // Carry: one column of labels and rows, paged, with ALL NODES pinned to the
    // bottom so it is reachable whatever the list above it does
    const std::vector<PickItem> items = pickItems();
    allNodesTop = height() - PICK_ROW_H;

    int16_t total = 0;
    for (const PickItem &item : items)
        total += item.label ? PICK_LABEL_H : PICK_ROW_H;
    pickPagerShown = total > allNodesTop - (bandH + 1);
    const int16_t bottom = pickPagerShown ? allNodesTop - PICK_PAGER_H - 1 : allNodesTop;

    std::vector<uint16_t> heights;
    heights.reserve(items.size());
    for (const PickItem &item : items)
        heights.push_back(item.label ? PICK_LABEL_H : PICK_ROW_H);
    pickPages = t5Paginate(heights, bottom - (bandH + 1));

    uint8_t page = 0;
    for (uint8_t p = 0; p < pickPages.size(); p++) {
        if (firstItem >= pickPages[p])
            page = p;
    }
    firstItem = pickPages[page];

    int16_t y = bandH + 1;
    for (uint16_t i = firstItem; i < items.size(); i++) {
        const PickItem &item = items[i];
        const int16_t h = item.label ? PICK_LABEL_H : PICK_ROW_H;
        if (y + h > bottom && i > firstItem)
            break;
        if (item.label) {
            setFont(Applet::fontSmall);
            // A section continued from the previous page says so, as the Messages
            // list does
            printBold(T5Applet::MARGIN, y + 8,
                      (i == firstItem && firstItem > 0) ? item.text + " " + T5Applet::SEP + " CONTINUED" : item.text);
        } else if (!item.convo.valid()) {
            // The frozen "no usable nodes" block: text, not a target
            setFont(Applet::fontMedium);
            printBold(T5Applet::MARGIN, y + 8, "No other nodes heard yet");
            setFont(Applet::fontSmall);
            int16_t hintY = y + 8 + fontMedium.lineHeight() + 6;
            for (const std::string &line : t5WrapLines(*this,
                                                       "Nodes appear here once their node info reaches you. "
                                                       "You can still write to a channel.",
                                                       width() - 2 * T5Applet::MARGIN)) {
                printAt(T5Applet::MARGIN, hintY, line);
                hintY += fontSmall.lineHeight();
            }
        } else {
            pickRows.push_back(item.convo);
            pickRowTops.push_back(y);
            drawPickRow(0, y, width(), item.convo, false);
        }
        y += h;
    }

    if (pickPagerShown) {
        const int16_t top = allNodesTop - PICK_PAGER_H - 1;
        const int16_t half = width() / 2, mid = top + PICK_PAGER_H / 2;
        drawLine(0, top, width() - 1, top, BLACK);
        drawLine(half, top, half, top + PICK_PAGER_H - 1, BLACK);
        setFont(Applet::fontSmall);
        const bool canPrev = page > 0, canNext = page + 1 < pickPages.size();
        const int16_t prevW = getTextWidth("PREV"), nextW = getTextWidth("NEXT");
        printBold((half - prevW) / 2, mid - fontSmall.lineHeight() / 2, "PREV");
        printBold(half + (half - nextW) / 2, mid - fontSmall.lineHeight() / 2, "NEXT");
        if (!canPrev)
            fillRect((half - prevW) / 2 - 14, mid - 1, prevW + 28, 2, BLACK);
        if (!canNext)
            fillRect(half + (half - nextW) / 2 - 14, mid - 1, nextW + 28, 2, BLACK);
    }

    allNodesShown = true;
    drawLine(0, allNodesTop, width() - 1, allNodesTop, BLACK);
    setFont(Applet::fontMedium);
    const std::string all = "ALL NODES";
    printBold(T5Applet::MARGIN, allNodesTop + 26, all);
    printAt(width() - 14, allNodesTop + PICK_ROW_H / 2, T5Applet::FWD, RIGHT, MIDDLE);
    if (recent.empty()) {
        // Our own node is never offered, so a fresh device has no DM target: the
        // row keeps its place, struck through, and its tap is consumed (Messaging
        // Final 12, X6)
        fillRect(T5Applet::MARGIN, allNodesTop + 26 + fontMedium.lineHeight() / 2 - 1, getTextWidth(all), 2, BLACK);
        setFont(Applet::fontSmall);
        printAt(width() - 60, allNodesTop + 26, "Nothing to choose from", RIGHT);
    }
}

// The picker's lines: the channels, then up to four recent nodes, or the frozen
// block which explains that there are none. An invalid Convo on a non-label
// item marks that block.
std::vector<InkHUD::T5ComposeTask::PickItem> InkHUD::T5ComposeTask::pickItems()
{
    T5MessageModel *model = T5MessageModel::get();
    std::vector<PickItem> items;
    if (!model)
        return items;

    items.push_back({true, "TO A CHANNEL", Convo()});
    for (uint8_t index : model->enabledChannels())
        items.push_back({false, "", Convo::forChannel(index)});

    items.push_back({true, std::string("TO A NODE ") + T5Applet::SEP + " RECENT", Convo()});
    const std::vector<NodeNum> recent = recentNodes();
    for (NodeNum num : recent)
        items.push_back({false, "", Convo::forPeer(num)});
    if (recent.empty())
        items.push_back({false, "", Convo()});
    return items;
}

void InkHUD::T5ComposeTask::pagePicker(int8_t delta)
{
    for (uint8_t p = 0; p < pickPages.size(); p++) {
        if (pickPages[p] != firstItem)
            continue;
        const int16_t target = (int16_t)p + delta;
        if (target < 0 || target >= (int16_t)pickPages.size())
            return;
        firstItem = pickPages[target];
        requestUpdate();
        return;
    }
}

// Swipes: the picker pages its own list, and in Nodes pick mode the list
// underneath is what should move, because we hold the input claim it would
// otherwise have received
void InkHUD::T5ComposeTask::onNavDown()
{
    if (mode == Mode::PICKER)
        pagePicker(1);
    else if (mode == Mode::NODE_LIST)
        T5NodesApplet::pageDown();
}

void InkHUD::T5ComposeTask::onNavUp()
{
    if (mode == Mode::PICKER)
        pagePicker(-1);
    else if (mode == Mode::NODE_LIST)
        T5NodesApplet::pageUp();
}

void InkHUD::T5ComposeTask::onRender(bool full)
{
    (void)full;
    if (mode == Mode::PICKER)
        renderPicker();
    else if (mode == Mode::COMPOSER)
        renderComposer();
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

bool InkHUD::T5ComposeTask::onTouchPoint(uint16_t x, uint16_t y, bool longPress)
{
    // Nodes pick mode draws the screen and handles its own taps; we hold only the
    // Home key
    if (mode == Mode::NODE_LIST || mode == Mode::CLOSED)
        return false;

    // A card or a terminal state is on top of us: its targets win, and the
    // arbiter owns them
    T5SystemLayer *system = T5SystemLayer::getIfBegun();
    if (system && system->coversContent())
        return false;

    const int16_t tx = x - getTile()->getLeft();
    const int16_t ty = y - getTile()->getTop();
    const bool console = width() > height();

    // CANCEL, in the frozen corner of every task
    const int16_t cancelLeft = console ? CONSOLE_CANCEL_HIT_LEFT : CARRY_CANCEL_HIT_LEFT;
    const int16_t bandH = console ? CONSOLE_HEADER_H : (mode == Mode::COMPOSER ? CARRY_HEADER_H : PICK_HEADER_H);
    if (ty < bandH && tx >= cancelLeft) {
        leaveTo(mode == Mode::COMPOSER ? origin : Origin::MESSAGES);
        return true;
    }

    if (mode == Mode::PICKER) {
        if (pickPagerShown && ty >= allNodesTop - PICK_PAGER_H - 1 && ty < allNodesTop) {
            pagePicker(tx < width() / 2 ? -1 : 1);
            return true;
        }
        for (size_t i = 0; i < pickRows.size(); i++) {
            const int16_t top = pickRowTops[i];
            const int16_t rowH = console ? PICK_CONSOLE_ROW_H : PICK_ROW_H;
            const bool inColumn = !console || (pickRows[i].isChannel() ? tx < PICK_CONSOLE_SPLIT : tx > PICK_CONSOLE_SPLIT);
            if (ty >= top && ty < top + rowH && inColumn) {
                openComposer(pickRows[i], Origin::MESSAGES);
                return true;
            }
        }
        if (allNodesShown && ty >= allNodesTop) {
            // Struck through with no node to choose: the tap is consumed and nothing
            // opens
            if (!recentNodes().empty()) {
                mode = Mode::NODE_LIST;
                reconcile();
                T5NodesApplet::openPickMode();
            }
            return true;
        }
        return true;
    }

    // Composer: the keyboard, then SEND on the Carry control row
    if (!console && ty >= CARRY_CONTROL_RULE && ty < CARRY_CONTROL_BOTTOM) {
        if (tx >= CARRY_SEND_HIT_LEFT)
            send();
        return true;
    }

    const T5Keyboard::Key key = T5Keyboard::hit(console, layer, shift, tx, ty);
    switch (key.kind) {
    case T5Keyboard::Key::CHAR:
        if (longPress)
            return true; // A held letter key does nothing: only DEL assigns a long
                         // press
        type(key.ch);
        break;
    case T5Keyboard::Key::DEL:
        backspace(longPress); // The frozen 500 ms long press deletes the previous
                              // whole word, once
        break;
    case T5Keyboard::Key::SHIFT:
        if (!longPress)
            toggleShift();
        break;
    case T5Keyboard::Key::LAYER:
        if (!longPress)
            layer = layer == T5Keyboard::Layer::LETTERS ? T5Keyboard::Layer::SYMBOLS : T5Keyboard::Layer::LETTERS;
        break;
    case T5Keyboard::Key::SEND:
        if (!longPress)
            send();
        return true;
    case T5Keyboard::Key::NONE:
        return true; // The header, the context band and the field are not targets
    }
    requestUpdate();
    return true;
}

#endif
