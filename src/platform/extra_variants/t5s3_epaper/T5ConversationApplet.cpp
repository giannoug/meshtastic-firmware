#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5ConversationApplet.h"
#include "./T5ComposeTask.h"
#include "./T5MessagesApplet.h"
#include "./T5SystemLayer.h"

#include "mesh/NodeDB.h"

#include <algorithm>

using namespace NicheGraphics;

bool InkHUD::T5ConversationApplet::open(const Convo &c)
{
    T5MessageModel *model = T5MessageModel::get();
    if (!model || !c.valid())
        return false;

    InkHUD *hud = InkHUD::getInstance();
    const int8_t i = indexOf("Conversation");
    if (i < 0)
        return false;

    T5ConversationApplet *applet = static_cast<T5ConversationApplet *>(hud->userApplets[i]);
    applet->enter(applet->thread, c);
    model->selected = c; // The Console split picks the same conversation up after a mode change
    model->clearTag();   // Entering a conversation clears the frozen "n NEW" tag
    return hud->showApplet(i, EInk::UpdateTypes::UNSPECIFIED, true);
}

void InkHUD::T5ConversationApplet::onActivate()
{
    setInputsSubscribed(NAV_UP | NAV_DOWN,
                        true); // Vertical swipes page the thread
}

void InkHUD::T5ConversationApplet::onDeactivate()
{
    setInputsSubscribed(NAV_UP | NAV_DOWN, false);
}

// Leaving the conversation drops the unread boundary rule and the header tag
// (Messaging Final 03, 08)
void InkHUD::T5ConversationApplet::onBackground()
{
    thread.enteredUnread = 0;
    if (T5MessageModel *model = T5MessageModel::get())
        model->clearTag();
}

// A model change only earns a redraw while we are the screen showing it, and
// not while a system surface sits on top: our redraw would erase a surface
// which still takes input (System UI R2).
void InkHUD::T5ConversationApplet::onMessagingChanged()
{
    const T5SystemLayer *system = T5SystemLayer::getIfBegun();
    if (isForeground() && !(system && system->coversContent()) && !T5MessageModel::taskIsForeground())
        requestUpdate();
}

void InkHUD::T5ConversationApplet::onRender(bool full)
{
    (void)full;
    const Convo &c = thread.convo;
    const int16_t right = width() - MARGIN;

    // Header: "‹ TARGET" in medium bold, the address ("CH n" or the node id) in the status slot (Panel
    // Fit D14). The target is the reply bar's, in stored case
    carryBackRight = drawSubPageTitle(c.isChannel() ? t5ConvoTitle(c) : t5ConvoTag(c),
                                      c.isChannel() ? "CH " + to_string(c.channel) : hexifyNodeNum(c.peer));

    // Line 2, plus the frozen "n NEW" tag at its right end while something
    // arrived elsewhere
    T5MessageModel *model = T5MessageModel::get();
    setFont(fontSmall);
    int16_t line2Right = right;
    if (model && model->tag()) {
        const std::string tag = to_string(model->tag()) + " NEW";
        const int16_t tw = getTextWidth(tag) + 20;
        fillRect(right - tw, 80 - 19 - 3, tw, fontSmall.lineHeight() + 6, BLACK);
        setTextColor(WHITE);
        printBold(right - 10, 80 - 19, tag, RIGHT);
        setTextColor(BLACK);
        line2Right = right - tw - 12;
    }
    setCrop(MARGIN, 56, std::max<int16_t>(0, line2Right - MARGIN), 43);
    printAt(MARGIN, 80 - 19, t5Elide(*this, threadSubtitle(c), std::max<int16_t>(0, line2Right - MARGIN))); // Baseline 80
    resetCrop();
    drawLine(0, HEADER_H, width() - 1, HEADER_H, BLACK);

    drawThread(thread, MARGIN, THREAD_TOP, width() - 2 * MARGIN, THREAD_BOTTOM - THREAD_TOP);
    drawPagingStrip(thread, 0, STRIP_TOP, width(), STRIP_H);

    // REPLY bar
    drawLine(0, REPLY_TOP, width() - 1, REPLY_TOP, BLACK);
    setFont(fontMedium);
    // A channel target is user data and elides at 476; a short name never needs to (D14)
    printBold(width() / 2, REPLY_TOP + (navTop() - 1 - REPLY_TOP - fontMedium.lineHeight()) / 2,
              t5Elide(*this, replyLabel(thread.convo), width() - 2 * MARGIN - 1), CENTER);

    drawNav();
}

bool InkHUD::T5ConversationApplet::onTouchPoint(uint16_t x, uint16_t y, bool longPress)
{
    if (longPress)
        return true; // No T5 screen assigns a long press, and the stock fallback
                     // would open the shared menu

    const int16_t tx = x - getTile()->getLeft();
    const int16_t ty = y - getTile()->getTop();

    if (handleNavTap(tx, ty))
        return true;

    if (ty < HEADER_H && tx < carryBackRight) {
        T5MessagesApplet::open(); // Structural Back: the Messages root, never Home
                                  // and never here again
        return true;
    }

    // RETRY before the strip: its grown target may reach up into the thread area
    if (retryTapped(thread, tx, ty)) {
        requestUpdate();
        return true;
    }

    if (stripTapped(thread, 0, STRIP_TOP, width(), STRIP_TOUCH_BOTTOM - STRIP_TOP, tx, ty))
        return true;

    if (ty >= REPLY_TOP && ty < navTop())
        T5ComposeTask::openReply(thread.convo);

    // Consume every tap: the button fallback would cycle applets
    return true;
}

// Swipes page the thread, with the direction Nodes already uses: down moves
// forward, into older messages
void InkHUD::T5ConversationApplet::onNavDown()
{
    pageEarlier(thread);
}

void InkHUD::T5ConversationApplet::onNavUp()
{
    if (thread.page == 0)
        return;
    thread.page--;
    requestUpdate();
}

#endif
