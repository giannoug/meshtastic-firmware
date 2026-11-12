#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

T5 Home: identity, status, messages and recently heard nodes, the mode control and the T5 nav.
One applet, two compositions picked from the live dimensions: Carry (portrait) and Console (landscape).

*/

#pragma once

#include "configuration.h"

#include "./T5Applet.h"
#include "./T5NodesApplet.h"

#include "MessageStore.h"
#include "concurrency/OSThread.h"

namespace NicheGraphics::InkHUD
{

class T5HomeApplet : public T5Applet, public concurrency::OSThread, public T5Redrawable
{
  public:
    T5HomeApplet();
    static void begin(); // Call after InkHUD::begin(), which loads saved settings over the addApplet() defaults

    void onActivate() override;
    void onDeactivate() override;
    void onRender(bool full) override;
    bool onTouchPoint(uint16_t x, uint16_t y, bool longPress) override;

    void onMessagingChanged() override;

  private:
    Destination currentDestination() override { return HOME; }

    int32_t runOnce() override;

    // Everything Home shows, read fresh on each render. Strings are already parse()d
    struct Status {
        std::string shortName, nodeId, clock, region, preset, gps, bt;
        uint16_t nodes = 0, heardCount = 0, direct = 0, oneHop = 0, multiHop = 0, channelUtil = 0;
        std::vector<T5NodesApplet::Row> heard; // Heard in the last 10 minutes, most recent first, at most six

        const StoredMessage *latest = nullptr; // Most recent visible incoming message
        std::string latestKind, latestSender, latestSenderLong, latestClock, latestText;
        std::string channel0Title, channel0Subtitle, dmSubtitle;

        // Read from the one messaging model, so Home, the Messages root and the DM
        // banner can never disagree about what is new (Messaging Final 30)
        uint16_t unread = 0, unreadChannel0 = 0, unreadDMs = 0;
    };

    Status readStatus();
    void renderCarry(const Status &s);
    void renderConsole(const Status &s);
    void drawConversation(int16_t left, int16_t top, uint16_t w, const std::string &title, const std::string &subtitle,
                          uint16_t unseen);
    void drawModeControl(int16_t x, int16_t y, uint16_t w, uint16_t h);
    void printClipped(int16_t left, int16_t firstBaseline, uint16_t w, uint8_t lines, int16_t pitch, const std::string &text);
    // Width of text in a face; leaves that face set
    int16_t textW(AppletFont f, const std::string &text)
    {
        setFont(f);
        return getTextWidth(text);
    }
    std::string senderName(NodeNum num);
    const StoredMessage *latestIncoming(bool dmOnly = false);
    void drawHeardRow(int16_t left, int16_t top, uint16_t w, const T5NodesApplet::Row &row);

    // The frozen Home deep links (Messaging Final 13)
    void openLatest();   // The unread card: whichever conversation the newest
                         // message belongs to
    void openChannel0(); // The channel card
    void openDirect();   // The newest DM, or the DM empty state
};

} // namespace NicheGraphics::InkHUD

#endif
