#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5ThreadApplet.h"

#include "gps/RTC.h"
#include "mesh/Channels.h"
#include "mesh/NodeDB.h"

using namespace NicheGraphics;

namespace
{
// The frozen status words. Only these five exist: QUEUED and SENT belong to
// superseded material, and the firmware cannot report them to an on-device UI
// anyway (Messaging Final 00, 01).
const char *stateWord(InkHUD::T5MessageModel::State s)
{
    switch (s) {
    case InkHUD::T5MessageModel::State::SENDING:
        return "SENDING\x85"; // Win-1253 ellipsis
    case InkHUD::T5MessageModel::State::RELAYED:
        return "RELAYED";
    case InkHUD::T5MessageModel::State::DELIVERED:
        return "DELIVERED";
    case InkHUD::T5MessageModel::State::NOT_HEARD:
        return "NOT HEARD";
    case InkHUD::T5MessageModel::State::FAILED:
        return "FAILED";
    }
    return "";
}
} // namespace

void InkHUD::T5ThreadApplet::enter(Thread &t, const Convo &c)
{
    T5MessageModel *model = T5MessageModel::get();
    t.convo = c;
    t.page = 0; // Opening a conversation always lands on its newest page
                // (Messaging Final 03)
    // Snapshotted here because markSeen() clears the count as soon as the newest
    // page is drawn, while the frozen rule keeps the boundary rule visible until
    // the conversation is left.
    t.enteredUnread = model ? model->unread(c) : 0;
}

// "DIRECT · HEARD 14:29 · DIRECT LINK · 2.1 KM NE" for a DM, the channel's
// privacy line for a channel
std::string InkHUD::T5ThreadApplet::threadSubtitle(const Convo &c)
{
    std::string line;
    if (c.isChannel()) {
        const bool primary = c.channel == channels.getPrimaryIndex();
        line = primary ? "PRIMARY" : "CHANNEL " + to_string(c.channel);
        const meshtastic_Channel &ch = channels.getByIndex(c.channel);
        line = join(line, ch.settings.psk.size == 0              ? "NO KEY \xB7 SENT IN THE CLEAR"
                          : channels.isDefaultChannel(c.channel) ? "DEFAULT KEY \xB7 READABLE BY ANY NODE"
                                                                 : "PRIVATE KEY");
    } else {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(c.peer);
        line = "DIRECT";
        if (node && node->last_heard) {
            const std::string heard = t5ListTime(node->last_heard, false);
            if (!heard.empty())
                line = join(line, "HEARD " + heard);
        }
        if (node && node->has_hops_away) {
            std::string hops = hopsString(node->hops_away);
            for (char &ch : hops)
                ch = toupper(ch);
            line = join(line, node->hops_away == 0 ? "DIRECT LINK" : hops);
        }
        if (!node)
            line = join(line, "NOT IN NODE LIST");
    }

    // The frozen fallback when nothing can be dated: day dividers and times are
    // dropped, and the header says so once rather than leaving every message
    // looking undated by accident.
    if (!getValidTime(RTCQuality::RTCQualityDevice, true))
        line = join(line, "NO CLOCK");
    return line;
}

std::string InkHUD::T5ThreadApplet::dayLabel(const StoredMessage *m)
{
    if (m->isBootRelative)
        return "";
    const uint32_t now = getValidTime(RTCQuality::RTCQualityDevice, true);
    if (!now)
        return "";
    const uint32_t nowDay = now / SEC_PER_DAY, day = m->timestamp / SEC_PER_DAY;
    if (day == nowDay)
        return "TODAY";
    if (day + 1 == nowDay)
        return "YESTERDAY";
    return t5ListTime(m->timestamp,
                      false); // Falls through to the weekday or D MMM form
}

// Everything about one message, measured once so a page holds exactly what it
// can draw
InkHUD::T5ThreadApplet::Block InkHUD::T5ThreadApplet::buildBlock(const StoredMessage *m, uint16_t w, bool console,
                                                                 bool channelThread)
{
    Block b;
    b.message = m;
    b.outgoing = m->sender == nodeDB->getNodeNum();

    const int16_t indent = console ? 80 : 56;
    const uint16_t textW = w > (uint16_t)(indent + PAD + RULE) ? w - indent - PAD - RULE : 1;

    setFont(fontSmall);
    T5MessageModel *model = T5MessageModel::get();
    if (b.outgoing) {
        b.senderTag = "YOU";
        if (const T5MessageModel::Outgoing *o = model ? model->outgoing(m) : nullptr) {
            b.state = stateWord(o->state);
            b.stateOutlined = o->state == T5MessageModel::State::NOT_HEARD;
            b.stateInverted = o->state == T5MessageModel::State::FAILED;
            if (o->reason)
                b.reason = o->reason;
            if (o->retries)
                b.reason = join(b.reason, "RETRY " + to_string(o->retries));
            b.retry = o->state == T5MessageModel::State::FAILED || o->state == T5MessageModel::State::NOT_HEARD;
        }
        // No record: an own message restored from flash after a reboot shows its
        // time alone (Messaging Final 01)
    } else {
        b.senderTag = t5NodeTag(m->sender);
        // A DM already names its peer in the header, so only a channel thread
        // repeats the long name
        if (channelThread)
            b.senderLong = t5NodeTitle(m->sender);
    }
    b.time = t5ListTime(m->timestamp, m->isBootRelative);

    b.lines = t5WrapLines(*this, parse(MessageStore::getText(*m)), textW);

    b.height = fontSmall.lineHeight() + SENDER_GAP + (int16_t)b.lines.size() * fontSmall.lineHeight() + GAP;
    if (b.retry)
        b.height += RETRY_GAP + fontSmall.lineHeight() + 2 * 12 + 2 * RULE;
    return b;
}

void InkHUD::T5ThreadApplet::drawBlock(const Block &b, int16_t left, int16_t top, uint16_t w, bool console, Thread &t)
{
    const int16_t indent = console ? 80 : 56;
    const int16_t lh = fontSmall.lineHeight();
    setFont(fontSmall);

    if (b.outgoing) {
        const int16_t right = left + w;
        const int16_t textRight = right - PAD - RULE;
        const int16_t bodyH = lh + SENDER_GAP + (int16_t)b.lines.size() * lh;
        fillRect(right - RULE, top, RULE, bodyH,
                 BLACK); // The 2 px right rule marks it as ours

        // YOU · time · state · reason, right aligned, built from the right edge
        // back
        int16_t x = textRight;
        if (!b.reason.empty()) {
            printAt(x, top, b.reason, RIGHT);
            x -= getTextWidth(b.reason) + 8;
        }
        if (!b.state.empty()) {
            if (b.stateInverted) {
                const int16_t bw = getTextWidth(b.state) + 16, bh = lh + 4;
                fillRect(x - bw, top - 2, bw, bh, BLACK);
                setTextColor(WHITE);
                printBold(x - 8, top, b.state, RIGHT);
                setTextColor(BLACK);
                x -= bw + 8;
            } else if (b.stateOutlined) {
                const int16_t bw = getTextWidth(b.state) + 14, bh = lh + 4;
                drawRect(x - bw, top - 2, bw, bh, BLACK);
                drawRect(x - bw + 1, top - 1, bw - 2, bh - 2, BLACK);
                printBold(x - 7, top, b.state, RIGHT);
                x -= bw + 8;
            } else {
                printBold(x, top, b.state, RIGHT);
                x -= getTextWidth(b.state) + 8;
                printAt(x, top, SEP, RIGHT);
                x -= getTextWidth(SEP) + 8;
            }
        }
        if (!b.time.empty()) {
            printAt(x, top, b.time, RIGHT);
            x -= getTextWidth(b.time) + 8;
        }
        printBold(x, top, b.senderTag, RIGHT);

        int16_t y = top + lh + SENDER_GAP;
        for (const std::string &line : b.lines) {
            printAt(textRight, y, line, RIGHT);
            y += lh;
        }

        if (b.retry) {
            const int16_t bw = getTextWidth("RETRY") + 2 * 22 + 2 * RULE, bh = lh + 2 * 12 + 2 * RULE;
            const int16_t bx = textRight - bw, by = y + RETRY_GAP;
            drawRect(bx, by, bw, bh, BLACK);
            drawRect(bx + 1, by + 1, bw - 2, bh - 2, BLACK);
            printBold(bx + bw / 2, by + RULE + 12, "RETRY", CENTER);
            // The visible button is under the frozen 64 px minimum, so its target is
            // grown around it
            const int16_t grow = bh < 64 ? (64 - bh) / 2 : 0;
            t.retries.push_back(
                {b.message, (int16_t)(bx - 10), (int16_t)(by - grow), (int16_t)(bx + bw + 10), (int16_t)(by + bh + grow)});
        }
        return;
    }

    // Incoming: the rule on the left, the text indented away from the opposite
    // edge
    const int16_t textLeft = left + RULE + PAD;
    const uint16_t textW = w - indent - PAD - RULE;
    const int16_t bodyH = lh + SENDER_GAP + (int16_t)b.lines.size() * lh;
    fillRect(left, top, RULE, bodyH, BLACK);

    printBold(textLeft, top, b.senderTag);
    int16_t x = textLeft + getTextWidth(b.senderTag) + 8;
    const int16_t timeW = b.time.empty() ? 0 : getTextWidth(b.time) + 8;
    if (!b.senderLong.empty() && x + timeW < textLeft + (int16_t)textW)
        printAt(x, top, t5Elide(*this, b.senderLong, textLeft + textW - timeW - x));
    if (!b.time.empty())
        printAt(textLeft + textW, top, b.time, RIGHT);

    int16_t y = top + lh + SENDER_GAP;
    for (const std::string &line : b.lines) {
        printAt(textLeft, y, line);
        y += lh;
    }
}

// Pages are built backwards from the newest message: the page the user is on
// keeps its contents when an older one is added, and EARLIER lands exactly on
// the message above the current top.
void InkHUD::T5ThreadApplet::drawThread(Thread &t, int16_t left, int16_t top, uint16_t w, uint16_t h)
{
    T5MessageModel *model = T5MessageModel::get();
    const bool console = isConsole();
    const std::vector<const StoredMessage *> msgs = model ? model->messages(t.convo) : std::vector<const StoredMessage *>();

    t.retries.clear();
    t.earlier = 0;
    t.pages = 1;

    if (msgs.empty()) {
        setFont(fontMedium);
        printAt(left, top + 20, "No messages yet");
        setFont(fontSmall);
        printAt(left, top + 20 + fontMedium.lineHeight() + 8, "Tap REPLY below to write the first one.");
        return;
    }

    // The first unread message, from the count taken when the conversation was
    // opened
    size_t firstUnread = msgs.size();
    if (t.enteredUnread) {
        uint16_t left_ = t.enteredUnread;
        for (size_t i = msgs.size(); i-- > 0;) {
            if (msgs[i]->sender == nodeDB->getNodeNum())
                continue;
            firstUnread = i;
            if (--left_ == 0)
                break;
        }
    }

    // Measure every message once, then fill pages backwards. Bounded by the whole store, which holds
    // twenty messages on this build, so wrapping them all on each render is cheaper than tracking heights.
    std::vector<Block> blocks;
    blocks.reserve(msgs.size());
    for (const StoredMessage *m : msgs)
        blocks.push_back(buildBlock(m, w, console, t.convo.isChannel()));

    // A divider above the first message of each day, and the unread rule above
    // the first unread one
    std::vector<int16_t> extra(msgs.size(), 0);
    std::vector<std::string> dividers(msgs.size());
    std::string previousDay;
    for (size_t i = 0; i < msgs.size(); i++) {
        const std::string day = dayLabel(msgs[i]);
        if (!day.empty() && day != previousDay) {
            dividers[i] = day;
            extra[i] += DIVIDER_H;
            previousDay = day;
        }
        if (i == firstUnread)
            extra[i] += NEW_RULE_H;
    }

    // Page boundaries, newest page first
    std::vector<size_t> starts; // starts[p] = index of the first message on page p
    std::vector<size_t> ends;
    size_t end = msgs.size();
    while (end > 0) {
        size_t start = end;
        int16_t used = 0;
        while (start > 0) {
            const int16_t need = blocks[start - 1].height + extra[start - 1];
            if (used + need > (int16_t)h)
                break;
            used += need;
            start--;
        }
        if (start == end)
            start = end - 1; // One message always shows, even if it is taller than the page
        starts.push_back(start);
        ends.push_back(end);
        end = start;
    }
    t.pages = starts.size();
    if (t.page >= t.pages)
        t.page = t.pages - 1;
    const size_t from = starts[t.page], to = ends[t.page];
    t.earlier = from;

    // Bottom anchored: the newest message on the page sits against the bottom of
    // the area
    int16_t used = 0;
    for (size_t i = from; i < to; i++)
        used += blocks[i].height + extra[i];
    int16_t y = top + (int16_t)h - used;
    if (y < top)
        y = top; // A single over-tall message is clipped at the top rather than
                 // overflowing the header

    for (size_t i = from; i < to; i++) {
        if (!dividers[i].empty()) {
            setFont(fontSmall);
            const int16_t mid = y + DIVIDER_H / 2;
            for (int16_t x = left; x < left + (int16_t)w; x += 4)
                drawPixel(x, mid + 10, BLACK);
            printBold(left, y, dividers[i]);
            y += DIVIDER_H;
        }
        if (i == firstUnread) {
            setFont(fontSmall);
            const std::string label = to_string(t.enteredUnread) + " NEW";
            const int16_t labelW = getTextWidth(label) + 20;
            const int16_t mid = y + NEW_RULE_H / 2;
            fillRect(left, mid, (int16_t)w - labelW, 2, BLACK);
            fillRect(left + (int16_t)w - labelW, y + 2, labelW, fontSmall.lineHeight() + 4, BLACK);
            setTextColor(WHITE);
            printBold(left + (int16_t)w - 10, y + 4, label, RIGHT);
            setTextColor(BLACK);
            y += NEW_RULE_H;
        }
        const int16_t indent = console ? 80 : 56;
        drawBlock(blocks[i], blocks[i].outgoing ? left + indent : left, y, w - indent, console, t);
        y += blocks[i].height;
    }

    // Its newest page is on screen, so this conversation is read (Messaging Final
    // 01)
    if (t.page == 0 && model)
        model->markSeen(t.convo);
}

// Left half pages back, right half jumps to the newest. The unavailable one
// keeps its place, struck through, and its tap is consumed (Messaging Final
// 03).
void InkHUD::T5ThreadApplet::drawPagingStrip(const Thread &t, int16_t left, int16_t top, uint16_t w, uint16_t h)
{
    const int16_t half = w / 2;
    const int16_t mid = top + h / 2;
    setFont(fontSmall);

    const bool canEarlier = t.page + 1 < t.pages;
    const bool atNewest = t.page == 0;

    // Left: EARLIER · n while there is more above, START at the oldest page
    const std::string earlier = canEarlier ? "EARLIER " + std::string(SEP) + " " + to_string(t.earlier) : "START";
    const int16_t earlierW = getTextWidth(earlier);
    const int16_t earlierX = left + (half - earlierW) / 2;
    if (canEarlier)
        drawTriangle(earlierX - 20, mid, true);
    printBold(earlierX, mid - fontSmall.lineHeight() / 2, earlier);
    if (!canEarlier)
        strikeThrough(earlierX - 14, mid, earlierW + 28);

    // Right: NEWEST, with the frozen count of what arrived while an older page
    // was showing
    T5MessageModel *model = T5MessageModel::get();
    const uint16_t pending = (!atNewest && model) ? model->unread(t.convo) : 0;
    std::string newest = "NEWEST";
    if (pending)
        newest += std::string(" ") + SEP + " " + to_string(pending) + " NEW";
    const int16_t newestW = getTextWidth(newest);
    const int16_t newestX = left + half + (half - newestW) / 2;
    printBold(newestX, mid - fontSmall.lineHeight() / 2, newest);
    drawTriangle(newestX + newestW + 20, mid, false);
    if (atNewest)
        strikeThrough(newestX - 14, mid, newestW + 28);

    drawLine(left + half, top, left + half, top + h - 1, BLACK);
    for (int16_t x = left; x < left + (int16_t)w; x += 4)
        drawPixel(x, top + h - 1, BLACK); // The dotted rule under the strip
}

bool InkHUD::T5ThreadApplet::retryTapped(const Thread &t, int16_t x, int16_t y)
{
    T5MessageModel *model = T5MessageModel::get();
    if (!model)
        return false;
    for (const Thread::RetryTarget &target : t.retries) {
        if (x >= target.left && x < target.right && y >= target.top && y < target.bottom) {
            model->retry(target.message);
            return true;
        }
    }
    return false;
}

bool InkHUD::T5ThreadApplet::stripTapped(Thread &t, int16_t left, int16_t top, uint16_t w, uint16_t h, int16_t x, int16_t y)
{
    if (y < top || y >= top + (int16_t)h || x < left || x >= left + (int16_t)w)
        return false;
    if (x < left + (int16_t)w / 2)
        pageEarlier(t);
    else
        pageNewest(t);
    return true;
}

void InkHUD::T5ThreadApplet::pageEarlier(Thread &t)
{
    if (t.page + 1 >= t.pages)
        return; // Already at the oldest page: the tap is consumed and nothing moves
    t.page++;
    requestUpdate();
}

void InkHUD::T5ThreadApplet::pageNewest(Thread &t)
{
    if (t.page == 0)
        return;
    t.page = 0;
    requestUpdate();
}

#endif
