#ifdef MESHTASTIC_INCLUDE_INKHUD

/*

The one authoritative T5 messaging model (Messaging Final 01, 13).

Conversations, ordering and previews are derived from MessageStore on every read
- there is no second message store. What the store cannot hold is kept here, and
only that:

  - unread counts, per conversation, session only (Messaging Final 01: nothing
is sent, nothing persists)
  - the delivery state of our own sends, keyed by packet id
  - drafts, RAM only, at most eight

Home, the Messages root, a conversation and the T5 system layer all read these
numbers from here, so no two surfaces can disagree about what is new (Messaging
Final 08, System UI M1).

State only: the strings a screen draws are built by T5Applet, which can parse()
them.

*/

#pragma once

#include "configuration.h"

#include "MessageStore.h"
#include "Observer.h"
#include "concurrency/OSThread.h"
#include "mesh/MeshTypes.h"
#include "mesh/SinglePortModule.h"

namespace NicheGraphics::InkHUD
{

// A messaging surface the model can ask to redraw. Each one decides for itself
// whether a redraw is due: only the surface knows whether it is showing, and
// whether a system surface is sitting on top of it.
class T5Redrawable
{
  public:
    virtual ~T5Redrawable() = default;
    virtual void onMessagingChanged() = 0;
};

class T5MessageModel : public SinglePortModule, public concurrency::OSThread
{
  public:
    T5MessageModel();

    static void begin();          // Construct and register. Call before the messaging applets
    static T5MessageModel *get(); // Null before begin()

    // The frozen composer limit, in UTF-8 bytes. Deliberately below the protocol
    // ceiling: 200 bytes fit a channel packet and a PKI DM alike, so one limit
    // covers every destination (V5).
    static constexpr uint16_t MAX_TEXT_BYTES = 200;

    // Which conversation. A channel index, or the peer of a DM - never both.
    struct Convo {
        int16_t channel = -1; // >= 0: a channel conversation
        NodeNum peer = 0;     // Otherwise: the other end of a DM

        static Convo forChannel(uint8_t index) { return {(int16_t)index, 0}; }
        static Convo forPeer(NodeNum num) { return {-1, num}; }
        bool isChannel() const { return channel >= 0; }
        bool valid() const { return channel >= 0 || peer != 0; }
        bool operator==(const Convo &o) const { return channel == o.channel && peer == o.peer; }
        bool operator!=(const Convo &o) const { return !(*this == o); }
    };

    // The frozen delivery states (Messaging Final 01). Session only: own messages
    // restored from flash after a reboot have no state at all, and show their
    // time alone.
    enum class State : uint8_t {
        SENDING,   // Sent, no response yet
        RELAYED,   // Implicit ACK: a neighbour rebroadcast it. Final on a channel,
                   // intermediate for a DM
        DELIVERED, // Routing ACK from the destination. DM only
        NOT_HEARD, // Channel only: the firmware gave up without hearing a
                   // rebroadcast
        FAILED     // DM only: a routing error, with a reason in words
    };

    // One of our own sends, while this session lasts.
    struct Outgoing {
        const StoredMessage *message = nullptr; // Its record in the store, for as long as the store keeps it
        uint32_t packetId = 0;                  // The packet we are waiting on. RETRY replaces it
        Convo convo;
        State state = State::SENDING;
        const char *reason = nullptr; // FAILED only: the frozen word
        uint8_t retries = 0;          // RETRY n
        uint32_t deadlineMs = 0;      // WALL: no terminal response by here means the frozen fallback
        std::string text;             // Kept for RETRY: the store's text pool is a ring and may
                                      // have wrapped
    };

    // Every enabled channel in index order, then DM peers by last message, newest
    // first (Messaging Final 02)
    std::vector<Convo> conversations();

    Convo convoOf(const StoredMessage &m) const;                 // Which conversation a stored message belongs to
    const StoredMessage *newest(const Convo &c);                 // Newest visible message, or null
    std::vector<const StoredMessage *> messages(const Convo &c); // Oldest first, ignored senders filtered
    const Outgoing *outgoing(const StoredMessage *m) const;      // Null unless we sent it this session

    // --- what is new
    // -----------------------------------------------------------------------------

    uint16_t unread() const; // Across every conversation
    uint16_t unread(const Convo &c) const;
    uint16_t unreadDMs() const;    // Home's "Direct messages" card
    void markSeen(const Convo &c); // Its newest page is on screen: the frozen clearing rule
    Convo newestUnread() const;    // An invalid Convo when nothing is unread

    // Messages that arrived while a Messaging screen or task was busy: the frozen
    // "n NEW" tag. Counted apart from unread, which survives being looked at from
    // somewhere else.
    uint16_t tag() const { return tagNew; }
    void clearTag() { tagNew = 0; }

    // --- drafts (RAM only, Messaging Final 04)
    // ---------------------------------------------------

    const std::string *draft(const Convo &c) const;
    void setDraft(const Convo &c, const std::string &text);
    void clearDraft(const Convo &c);

    // --- sending
    // ---------------------------------------------------------------------------------

    bool send(const Convo &c,
              const std::string &text); // False if the radio would not take it
    void retry(const StoredMessage *m); // A new packet id for the same text, same row

    // --- destinations
    // ----------------------------------------------------------------------------

    bool channelEnabled(uint8_t index) const;
    std::vector<uint8_t> enabledChannels() const;
    bool canMessage(NodeNum num) const; // A usable DM target: never us, never ignored (Messaging Final 05)
    bool hasPublicKey(NodeNum num) const;

    // --- foreground, for the system layer's frozen suppression (System UI M4)
    // --------------------

    static bool foregroundIsMessaging();   // Messages root, a conversation, or any
                                           // Messaging task
    static bool taskIsForeground();        // Composer, recipient picker, or Nodes in pick mode
    static Convo foregroundConversation(); // The thread on screen, or an invalid Convo

    // Carry <-> Console: the frozen rule swaps which applet holds the selection
    // (Messaging Final 09)
    static void onModeChanged();

    // The Console split's selection, which also carries a Carry conversation
    // across a mode change
    Convo selected;

    void attach(T5Redrawable *surface); // Every messaging surface registers once,
                                        // at startup

  private:
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    int32_t runOnce() override;

    int onReceiveTextMessage(const meshtastic_MeshPacket *p);
    CallbackObserver<T5MessageModel, const meshtastic_MeshPacket *> textMessageObserver =
        CallbackObserver<T5MessageModel, const meshtastic_MeshPacket *>(this, &T5MessageModel::onReceiveTextMessage);

    struct Unread {
        Convo convo;
        uint16_t count = 0;
        uint32_t newestAtMs = 0; // For "the newest unread conversation", which the DM banner opens
    };
    std::vector<Unread> unreadList;
    uint16_t tagNew = 0;

    struct Draft {
        Convo convo;
        std::string text;
        uint32_t touchedMs = 0; // The oldest draft is evicted when a ninth is created
    };
    std::vector<Draft> drafts;

    std::vector<Outgoing> sends;

    bool noteRoutingReply(const meshtastic_MeshPacket &mp); // Match a routing reply to the send it answers
    void storeBroadcast(const meshtastic_MeshPacket &mp);   // Channel text into MessageStore: nothing else does it
    bool stillStored(const StoredMessage *m) const;
    void pruneSends();
    meshtastic_MeshPacket *buildPacket(const Convo &c, const std::string &text);
    void record(const Convo &c, const std::string &text, uint32_t packetId);
    void settle(Outgoing &o, State state, const char *reason);
    std::vector<T5Redrawable *> surfaces;
    void changed(); // Model state moved: let every attached surface decide
                    // whether to redraw

    // The frozen no-response fallback. WALL time: a send does not stop being
    // overdue because the display slept through the wait (Messaging Final 01,
    // V3).
    static constexpr uint32_t NO_RESPONSE_MS = 180 * 1000UL;
    static constexpr uint8_t MAX_SENDS = 12;  // Oldest records drop first; the message keeps its text
    static constexpr uint8_t MAX_UNREAD = 24; // Conversations tracked at once
    static constexpr uint8_t MAX_DRAFTS = 8;  // Frozen
};

} // namespace NicheGraphics::InkHUD

#endif
