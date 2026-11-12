#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

T5 Conversation: one thread, full screen, in Carry (Messaging Final 03).

A deeper screen rather than a destination, like Node detail: the nav bar keeps
showing MSGS, and the header is the Back target to the Messages root. Console
shows the same thread in the split's right-hand pane instead, so a mode change
hands the conversation over to T5MessagesApplet (Messaging Final 09).

*/

#pragma once

#include "configuration.h"

#include "./T5ThreadApplet.h"

namespace NicheGraphics::InkHUD
{

class T5ConversationApplet : public T5ThreadApplet, public T5Redrawable
{
  public:
    T5ConversationApplet() { hideFromAppSwitcher = true; } // Opened from a Messages row, never by cycling

    static bool open(const Convo &c); // Show this conversation. False, and
                                      // nothing shown, if it can't be

    void onActivate() override;
    void onDeactivate() override;
    void onRender(bool full) override;
    bool onTouchPoint(uint16_t x, uint16_t y, bool longPress) override;
    void onNavUp() override;
    void onNavDown() override;
    void onBackground() override;

    void onMessagingChanged() override;

    const Convo &conversation() const { return thread.convo; }

  private:
    Destination currentDestination() override { return MSGS; }

    Thread thread;

    // The frozen Carry geometry (Messaging Final 03, 13)
    static constexpr int16_t HEADER_H = 99;
    static constexpr int16_t STRIP_TOP = 100, STRIP_H = 56, STRIP_TOUCH_BOTTOM = 163;
    static constexpr int16_t THREAD_TOP = 164, THREAD_BOTTOM = 744;
    static constexpr int16_t REPLY_TOP = 752;
    int16_t carryBackRight = 200; // The Back target ends here: the drawn title's right edge + 12 (D12, D14)
};

} // namespace NicheGraphics::InkHUD

#endif
