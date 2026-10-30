#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

The frozen thread renderer, shared by the two surfaces that draw one (Messaging
Final 03): the Carry conversation screen, and the right-hand pane of the Console
split.

A thread is a stack of pages, not a scroll. Pages are built backwards from the
newest message, each holding as many whole messages as fit; a message is never
split across a page boundary.

*/

#pragma once

#include "configuration.h"

#include "./T5Applet.h"

namespace NicheGraphics::InkHUD
{

class T5ThreadApplet : public T5Applet
{
  protected:
    // Everything a drawn thread remembers between renders and taps
    struct Thread {
        Convo convo;
        uint16_t page = 0;          // Pages from the newest: 0 is the newest page
        uint16_t enteredUnread = 0; // Unread when the conversation was opened:
                                    // where the "n NEW" rule goes

        // A RETRY button and the target it grew around. A page can hold several
        // messages which ended badly, and every one of them offers its own RETRY
        // (Messaging Final 12, X1).
        struct RetryTarget {
            const StoredMessage *message = nullptr;
            int16_t left = 0, top = 0, right = 0, bottom = 0;
        };

        // Filled in by drawThread, for the touch map and the paging strip
        uint16_t earlier = 0; // Messages on pages before this one
        uint16_t pages = 1;
        std::vector<RetryTarget> retries;
    };

    void enter(Thread &t,
               const Convo &c); // Open a conversation: newest page, unread snapshot taken

    // The messages themselves. left/top/w/h is the frozen message area, and the
    // page is drawn bottom-anchored inside it: the newest message sits directly
    // above the REPLY bar.
    void drawThread(Thread &t, int16_t left, int16_t top, uint16_t w, uint16_t h);

    // Always lands on the newest page: a conversation is never reopened part way
    // up its own history

    // EARLIER · n | NEWEST, with the unavailable half struck through and its tap
    // consumed
    void drawPagingStrip(const Thread &t, int16_t left, int16_t top, uint16_t w, uint16_t h);

    bool retryTapped(const Thread &t, int16_t x, int16_t y);
    bool stripTapped(Thread &t, int16_t left, int16_t top, uint16_t w, uint16_t h, int16_t x, int16_t y);
    void pageEarlier(Thread &t);
    void pageNewest(Thread &t);

    // Header line 2 of a conversation: what this destination is, and the frozen
    // NO CLOCK suffix
    std::string threadSubtitle(const Convo &c);
    std::string replyLabel(const Convo &c); // "REPLY TO <target>", or the draft form, in both modes (D13)

  private:
    // One message, measured and drawn the same way, so a page's capacity is exact
    struct Block {
        const StoredMessage *message = nullptr;
        bool outgoing = false;
        std::string senderTag, senderLong, time, state, reason;
        bool stateOutlined = false, stateInverted = false;
        std::vector<std::string> lines;
        bool retry = false;
        const char *divider = nullptr; // A day divider drawn above this message, or null
        std::string dividerLabel;
        int16_t height = 0;
    };

    Block buildBlock(const StoredMessage *m, uint16_t w, bool console, bool channelThread);
    void drawBlock(const Block &b, int16_t left, int16_t top, uint16_t w, bool console, Thread &t);
    std::string dayLabel(const StoredMessage *m);

    // A message is 24 (small sender line) + 34 per medium body line + 8 (Panel Fit D10)
    static constexpr int16_t GAP = 8;         // Between message blocks
    static constexpr int16_t SENDER_H = 24;   // The sender line's pitch
    static constexpr int16_t BODY_PITCH = 34; // Medium body lines
    static constexpr int16_t RULE = 2;        // The in/out rule
    static constexpr int16_t PAD = 12;        // Between the rule and the text
    static constexpr int16_t RETRY_GAP = 10;
    static constexpr int16_t DIVIDER_H = 32;  // Dotted rule plus its label
    static constexpr int16_t NEW_RULE_H = 30; // The unread boundary
};

} // namespace NicheGraphics::InkHUD

#endif
