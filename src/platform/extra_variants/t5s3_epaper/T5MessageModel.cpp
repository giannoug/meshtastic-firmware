#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5MessageModel.h"
#include "./T5Applet.h"
#include "./T5ComposeTask.h"
#include "./T5ConversationApplet.h"
#include "./T5MessagesApplet.h"
#include "./T5NodesApplet.h"

#include "Throttle.h"
#include "UptimeClock.h"
#include "gps/RTC.h"
#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
#include "mesh/mesh-pb-constants.h"
#include "modules/TextMessageModule.h"

#include <algorithm>

using namespace NicheGraphics;

namespace
{
InkHUD::T5MessageModel *instance = nullptr;

// The frozen FAILED reasons, in words (Messaging Final 01). Everything the enum
// can carry which is not one of the four named causes is reported as ERROR
// rather than invented into a fifth reason.
const char *reasonFor(meshtastic_Routing_Error error)
{
    switch (error) {
    case meshtastic_Routing_Error_MAX_RETRANSMIT:
        return "NO ACK";
    case meshtastic_Routing_Error_NO_ROUTE:
        return "NO ROUTE";
    case meshtastic_Routing_Error_PKI_FAILED:
    case meshtastic_Routing_Error_PKI_UNKNOWN_PUBKEY:
    case meshtastic_Routing_Error_PKI_SEND_FAIL_PUBLIC_KEY:
        return "KEY ERROR";
    case meshtastic_Routing_Error_NO_INTERFACE:
        return "RADIO OFF";
    default:
        return "ERROR";
    }
}
} // namespace

InkHUD::T5MessageModel::T5MessageModel()
    : SinglePortModule("T5Messages", meshtastic_PortNum_TEXT_MESSAGE_APP), concurrency::OSThread("T5MessageModel")
{
    // Messages the node itself originated - from the phone, or from the canned
    // message module - come back through the module chain as loopback.
    // ThreadedMessageApplet took them for the same reason, so that "You: ..."
    // previews are right; ours are matched against our own packet ids so they are
    // not stored twice.
    loopbackOk = true;
    OSThread::disable(); // Woken only while a send is outstanding
}

void InkHUD::T5MessageModel::begin()
{
    assert(!instance);
    instance = new T5MessageModel;
    instance->textMessageObserver.observe(textMessageModule);
}

InkHUD::T5MessageModel *InkHUD::T5MessageModel::get()
{
    return instance;
}

void InkHUD::T5MessageModel::attach(T5Redrawable *surface)
{
    surfaces.push_back(surface);
}

void InkHUD::T5MessageModel::changed()
{
    for (T5Redrawable *s : surfaces)
        s->onMessagingChanged();
}

// ---------------------------------------------------------------------------------------------
// Conversations
// ---------------------------------------------------------------------------------------------

bool InkHUD::T5MessageModel::channelEnabled(uint8_t index) const
{
    if (index >= channels.getNumChannels())
        return false;
    const meshtastic_Channel &ch = channels.getByIndex(index);
    return ch.has_settings && ch.role != meshtastic_Channel_Role_DISABLED;
}

std::vector<uint8_t> InkHUD::T5MessageModel::enabledChannels() const
{
    std::vector<uint8_t> list;
    for (uint8_t i = 0; i < channels.getNumChannels(); i++) {
        if (channelEnabled(i))
            list.push_back(i);
    }
    return list;
}

InkHUD::T5MessageModel::Convo InkHUD::T5MessageModel::convoOf(const StoredMessage &m) const
{
    if (m.type != MessageType::DM_TO_US)
        return Convo::forChannel(m.channelIndex);
    const NodeNum us = nodeDB->getNodeNum();
    return Convo::forPeer(m.sender == us ? m.dest : m.sender);
}

// Channels in index order first - fixed places, so a busy default channel can
// never push the people you talk to off page one - then DM peers by their last
// message (Messaging Final 02).
std::vector<InkHUD::T5MessageModel::Convo> InkHUD::T5MessageModel::conversations()
{
    std::vector<Convo> list;
    for (uint8_t index : enabledChannels())
        list.push_back(Convo::forChannel(index));

    // Walk the store newest first, so each peer lands in the order of its own
    // newest visible message
    std::vector<Convo> peers;
    const std::deque<StoredMessage> &stored = messageStore.getLiveMessages();
    for (auto it = stored.rbegin(); it != stored.rend(); ++it) {
        if (it->type != MessageType::DM_TO_US || !messageStore.isMessageVisible(*it))
            continue;
        const Convo c = convoOf(*it);
        if (!c.valid())
            continue;
        if (std::find(peers.begin(), peers.end(), c) == peers.end())
            peers.push_back(c);
    }
    list.insert(list.end(), peers.begin(), peers.end());
    return list;
}

std::vector<const StoredMessage *> InkHUD::T5MessageModel::messages(const Convo &c)
{
    std::vector<const StoredMessage *> list;
    if (!c.valid())
        return list;
    for (const StoredMessage &m : messageStore.getLiveMessages()) {
        if (!messageStore.isMessageVisible(m) || convoOf(m) != c)
            continue;
        if (c.isChannel() && m.type == MessageType::DM_TO_US)
            continue;
        list.push_back(&m);
    }
    return list;
}

const StoredMessage *InkHUD::T5MessageModel::newest(const Convo &c)
{
    const std::vector<const StoredMessage *> list = messages(c);
    return list.empty() ? nullptr : list.back();
}

// ---------------------------------------------------------------------------------------------
// Unread, and the "n NEW" tag
// ---------------------------------------------------------------------------------------------

uint16_t InkHUD::T5MessageModel::unread() const
{
    uint16_t total = 0;
    for (const Unread &u : unreadList)
        total += u.count;
    return total;
}

uint16_t InkHUD::T5MessageModel::unread(const Convo &c) const
{
    for (const Unread &u : unreadList) {
        if (u.convo == c)
            return u.count;
    }
    return 0;
}

uint16_t InkHUD::T5MessageModel::unreadDMs() const
{
    uint16_t total = 0;
    for (const Unread &u : unreadList) {
        if (!u.convo.isChannel())
            total += u.count;
    }
    return total;
}

void InkHUD::T5MessageModel::markSeen(const Convo &c)
{
    for (auto it = unreadList.begin(); it != unreadList.end(); ++it) {
        if (it->convo == c) {
            unreadList.erase(it);
            return;
        }
    }
}

InkHUD::T5MessageModel::Convo InkHUD::T5MessageModel::newestUnread() const
{
    const Unread *best = nullptr;
    for (const Unread &u : unreadList) {
        if (!best || u.newestAtMs > best->newestAtMs)
            best = &u;
    }
    return best ? best->convo : Convo();
}

// Only messages that arrive after boot can be new: a conversation that already
// existed starts as seen.
int InkHUD::T5MessageModel::onReceiveTextMessage(const meshtastic_MeshPacket *p)
{
    if (isFromUs(p))
        return 0;

    const Convo c = isBroadcast(p->to) ? Convo::forChannel(p->channel) : Convo::forPeer(p->from);
    if (!c.valid())
        return 0;

    // An ignored sender is hidden from every surface alike, so it must not raise
    // a count either (Messaging Final 02). The store applies the same rule when
    // it declines to keep the packet.
    const meshtastic_NodeInfoLite *sender = nodeDB->getMeshNode(p->from);
    if (nodeInfoLiteIsIgnored(sender))
        return 0;

    // Always counted here. The clearing rule is the frozen one and belongs to
    // rendering: a conversation is read when its newest page is actually
    // displayed, which is not the same as being foreground - an older page, or a
    // gated screen, leaves the count standing (Messaging Final 01).
    Unread *entry = nullptr;
    for (Unread &u : unreadList) {
        if (u.convo == c)
            entry = &u;
    }
    if (!entry) {
        if (unreadList.size() >= MAX_UNREAD)
            unreadList.erase(unreadList.begin());
        unreadList.push_back({c, 0, 0});
        entry = &unreadList.back();
    }
    if (entry->count < 999)
        entry->count++;
    entry->newestAtMs = Time::getMillis();

    // The frozen tag counts what arrived while the user was busy somewhere else
    // inside Messaging. A message for the conversation on screen is not
    // "elsewhere", and outside Messaging the system layer's banner is the
    // indicator, so this stays at zero there.
    if (foregroundIsMessaging() && foregroundConversation() != c && tagNew < 99)
        tagNew++;

    changed();
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Drafts
// ---------------------------------------------------------------------------------------------

const std::string *InkHUD::T5MessageModel::draft(const Convo &c) const
{
    for (const Draft &d : drafts) {
        if (d.convo == c)
            return &d.text;
    }
    return nullptr;
}

void InkHUD::T5MessageModel::setDraft(const Convo &c, const std::string &text)
{
    if (text.empty()) {
        clearDraft(c);
        return;
    }
    for (Draft &d : drafts) {
        if (d.convo == c) {
            d.text = text;
            d.touchedMs = Time::getMillis();
            return;
        }
    }
    if (drafts.size() >= MAX_DRAFTS) {
        auto oldest = std::min_element(drafts.begin(), drafts.end(),
                                       [](const Draft &a, const Draft &b) { return a.touchedMs < b.touchedMs; });
        drafts.erase(oldest);
    }
    drafts.push_back({c, text, Time::getMillis()});
}

void InkHUD::T5MessageModel::clearDraft(const Convo &c)
{
    for (auto it = drafts.begin(); it != drafts.end(); ++it) {
        if (it->convo == c) {
            drafts.erase(it);
            return;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------------------------

bool InkHUD::T5MessageModel::canMessage(NodeNum num) const
{
    if (!num || num == NODENUM_BROADCAST || num == nodeDB->getNodeNum())
        return false;
    return !nodeInfoLiteIsIgnored(nodeDB->getMeshNode(num));
}

bool InkHUD::T5MessageModel::hasPublicKey(NodeNum num) const
{
    const meshtastic_NodeInfoLite *node = nodeDB->getMeshNode(num);
    return node && node->public_key.size == 32;
}

// The public send path, as CannedMessageModule uses it: allocate, address, ask
// for an ack, copy the text, keep the packet id. PKI is left to the router - it
// decides from the destination's key, and reports its own failure as a routing
// error we then show on the message (Messaging Final 09, V4).
meshtastic_MeshPacket *InkHUD::T5MessageModel::buildPacket(const Convo &c, const std::string &text)
{
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return nullptr;

    if (c.isChannel()) {
        p->to = NODENUM_BROADCAST;
        p->channel = c.channel;
    } else {
        p->to = c.peer;
        p->channel = 0;
    }
    p->decoded.dest = p->to;

    // Both destinations ask for an ack. A broadcast needs it for the implicit ACK
    // that becomes RELAYED, and for the MAX_RETRANSMIT that becomes NOT HEARD;
    // the router clears the on-air flag itself.
    p->want_ack = true;

    p->decoded.payload.size = text.size();
    memcpy(p->decoded.payload.bytes, text.data(), text.size());
    return p;
}

// Our own message, in the store, so it survives a reboot as the frozen model
// expects - with its delivery state kept here, because that part is session
// only.
void InkHUD::T5MessageModel::record(const Convo &c, const std::string &text, uint32_t packetId)
{
    StoredMessage sm;
    const uint32_t nowSecs = getValidTime(RTCQuality::RTCQualityDevice, true);
    sm.timestamp = nowSecs ? nowSecs : Time::getUptimeSecs();
    sm.isBootRelative = nowSecs == 0;
    sm.sender = nodeDB->getNodeNum();
    sm.channelIndex = c.isChannel() ? c.channel : 0;
    sm.dest = c.isChannel() ? NODENUM_BROADCAST : c.peer;
    sm.type = c.isChannel() ? MessageType::BROADCAST : MessageType::DM_TO_US;
    sm.textOffset = MessageStore::storeText(text.c_str(), text.size());
    sm.textLength = text.size();
    sm.ackStatus = AckStatus::NONE; // The T5 states live here; the store's own
                                    // field is left alone
    messageStore.addLiveMessage(sm);

    // A push_back may have evicted the oldest message, and with it the record an
    // older send pointed at
    pruneSends();
    if (sends.size() >= MAX_SENDS)
        sends.erase(sends.begin());

    Outgoing o;
    o.message = &messageStore.getLiveMessages().back();
    o.packetId = packetId;
    o.convo = c;
    o.deadlineMs = Time::timerEndsAtMillis(NO_RESPONSE_MS);
    o.text = text;
    sends.push_back(o);

    OSThread::enabled = true;
    OSThread::setIntervalFromNow(1000);
}

bool InkHUD::T5MessageModel::send(const Convo &c, const std::string &text)
{
    if (!c.valid() || text.empty() || text.size() > MAX_TEXT_BYTES)
        return false;

    meshtastic_MeshPacket *p = buildPacket(c, text);
    if (!p)
        return false;

    // Recorded before it goes out. The router naks a rejected send synchronously,
    // from inside sendToMesh, so a record made afterwards would miss its own
    // routing reply and sit at SENDING until the timeout.
    record(c, text, p->id);
    service->sendToMesh(p, RX_SRC_LOCAL, true);
    changed();
    return true;
}

// A new packet for the same text. The message keeps its place and its original
// time, so the thread does not reflow: only its status line changes (Messaging
// Final 01, 11).
void InkHUD::T5MessageModel::retry(const StoredMessage *m)
{
    for (Outgoing &o : sends) {
        if (o.message != m)
            continue;
        if (o.state != State::FAILED && o.state != State::NOT_HEARD)
            return; // Only a message which ended badly offers RETRY

        meshtastic_MeshPacket *p = buildPacket(o.convo, o.text);
        if (!p)
            return;
        o.packetId = p->id; // Re-associated before the send, so a fast local nak
                            // cannot miss it
        o.state = State::SENDING;
        o.reason = nullptr;
        o.retries++;
        o.deadlineMs = Time::timerEndsAtMillis(NO_RESPONSE_MS);
        service->sendToMesh(p, RX_SRC_LOCAL, true);

        OSThread::enabled = true;
        OSThread::setIntervalFromNow(1000);
        changed();
        return;
    }
}

// ---------------------------------------------------------------------------------------------
// Foreground context (System UI M4, 24)
// ---------------------------------------------------------------------------------------------

// Every Messaging context: the root, a conversation, and the task layers. The
// system layer asks this before raising a message banner - inside Messaging
// nothing ever takes over the screen, and the frozen "n NEW" indicators take
// its place (Messaging Final 08).
bool InkHUD::T5MessageModel::foregroundIsMessaging()
{
    if (taskIsForeground())
        return true;
    for (const char *name : {"Messages", "Conversation"}) {
        const int8_t i = T5Applet::indexOf(name);
        if (i >= 0 && InkHUD::getInstance()->userApplets[i]->isForeground())
            return true;
    }
    return false;
}

bool InkHUD::T5MessageModel::taskIsForeground()
{
    return T5ComposeTask::taskOpen();
}

// The conversation whose thread is on screen, or an invalid Convo. Carry shows
// one on its own screen; Console shows the split's selection. A task covers
// both, so nothing is on screen then.
InkHUD::T5MessageModel::Convo InkHUD::T5MessageModel::foregroundConversation()
{
    T5MessageModel *model = get();
    if (!model || taskIsForeground())
        return Convo();

    InkHUD *hud = InkHUD::getInstance();
    const int8_t conversation = T5Applet::indexOf("Conversation");
    if (conversation >= 0 && hud->userApplets[conversation]->isForeground())
        return static_cast<T5ConversationApplet *>(hud->userApplets[conversation])->conversation();

    const int8_t messages = T5Applet::indexOf("Messages");
    if (hud->width() > hud->height() && messages >= 0 && hud->userApplets[messages]->isForeground())
        return model->selected;
    return Convo();
}

// Carry <-> Console. The split holds a selection where Carry has a whole
// screen, so the frozen rule hands the conversation from one to the other;
// everything else is applet state, which survives a re-layout for free
// (Messaging Final 09).
void InkHUD::T5MessageModel::onModeChanged()
{
    T5MessageModel *model = get();
    if (!model)
        return;
    if (T5ComposeTask *task = T5ComposeTask::get())
        task->onModeChanged();

    InkHUD *hud = InkHUD::getInstance();
    const bool console = hud->width() > hud->height();
    const int8_t messages = T5Applet::indexOf("Messages"), conversation = T5Applet::indexOf("Conversation");
    if (messages < 0 || conversation < 0)
        return;

    // Carry conversation -> Console split with it selected; Console split -> the
    // Carry conversation screen
    if (console && hud->userApplets[conversation]->isForeground())
        T5MessagesApplet::open(model->selected);
    else if (!console && hud->userApplets[messages]->isForeground() && model->selected.valid())
        T5ConversationApplet::open(model->selected);
}

// ---------------------------------------------------------------------------------------------
// Routing outcomes (Messaging Final 01, V1-V3)
// ---------------------------------------------------------------------------------------------

bool InkHUD::T5MessageModel::stillStored(const StoredMessage *m) const
{
    for (const StoredMessage &s : messageStore.getLiveMessages()) {
        if (&s == m)
            return true;
    }
    return false;
}

// The store is a deque: appending leaves references to the other elements
// valid, and only an eviction from the front invalidates one. Drop the records
// whose message has gone rather than hold a stale pointer.
void InkHUD::T5MessageModel::pruneSends()
{
    for (auto it = sends.begin(); it != sends.end();)
        it = stillStored(it->message) ? it + 1 : sends.erase(it);
}

const InkHUD::T5MessageModel::Outgoing *InkHUD::T5MessageModel::outgoing(const StoredMessage *m) const
{
    for (const Outgoing &o : sends) {
        if (o.message == m)
            return &o;
    }
    return nullptr;
}

void InkHUD::T5MessageModel::settle(Outgoing &o, State state, const char *reason)
{
    o.state = state;
    o.reason = reason;
    o.deadlineMs = 0; // Nothing further is owed: RELAYED on a DM rests here too,
                      // it is not a failure
    changed();
}

bool InkHUD::T5MessageModel::wantPacket(const meshtastic_MeshPacket *p)
{
    // Broadcast text, so it can be stored, and routing replies while we are
    // waiting on one of our sends
    if (p->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP)
        return isBroadcast(p->to);
    if (p->decoded.portnum != meshtastic_PortNum_ROUTING_APP)
        return false;
    for (const Outgoing &o : sends) {
        if (o.state == State::SENDING)
            return true;
    }
    return false;
}

ProcessMessage InkHUD::T5MessageModel::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP)
        storeBroadcast(mp);
    else
        noteRoutingReply(mp);
    return ProcessMessage::CONTINUE;
}

// Channel messages reach MessageStore from here.
//
// On an InkHUD build nothing else stores them: TextMessageModule's own call is
// inside IF_SCREEN, which the build compiles out, and
// Events::onReceiveTextMessage deliberately stores DMs only - it left
// broadcasts to ThreadedMessageApplet, which existed for two channels and is
// now retired on T5. Doing it here also fixes what that arrangement could never
// do: the frozen Messages root lists every enabled channel, and now every one
// of them can actually hold history (Messaging Final 02, V6).
void InkHUD::T5MessageModel::storeBroadcast(const meshtastic_MeshPacket &mp)
{
    if (!isBroadcast(mp.to) || !messageStore.shouldStorePacket(mp))
        return;

    // Our own sends are already in the store, with their delivery state attached:
    // the loopback copy of one must not become a second row (Messaging Final 22,
    // on keying by packet id).
    for (const Outgoing &o : sends) {
        if (o.packetId == mp.id)
            return;
    }

    if (!channelEnabled(mp.channel))
        return; // Not a conversation the frozen list would show, so not worth a
                // slot in a store this small

    messageStore.tryAddFromPacket(mp);
    pruneSends(); // The push may have evicted the oldest message, and a send
                  // record may have pointed at it
    changed();
}

bool InkHUD::T5MessageModel::noteRoutingReply(const meshtastic_MeshPacket &mp)
{
    if (mp.decoded.portnum != meshtastic_PortNum_ROUTING_APP || !mp.decoded.request_id)
        return false;

    meshtastic_Routing routing = meshtastic_Routing_init_default;
    if (!pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, meshtastic_Routing_fields, &routing))
        return false;

    for (Outgoing &o : sends) {
        // Keyed by packet id, never by "the last message sent": two sends can be
        // outstanding at once, and a retry carries a new id for a message that
        // already has a row.
        if (o.packetId != mp.decoded.request_id || o.state != State::SENDING)
            continue;

        if (routing.error_reason != meshtastic_Routing_Error_NONE) {
            // A channel message has no FAILED state: direct neighbours may well have
            // received it, so the frozen word is NOT HEARD whatever the router's
            // reason was.
            if (o.convo.isChannel())
                settle(o, State::NOT_HEARD, nullptr);
            else
                settle(o, State::FAILED, reasonFor(routing.error_reason));
            return true;
        }

        // An ack from the node we addressed is a receipt; an ack from anyone else
        // is a neighbour we heard rebroadcasting us. A channel message can only
        // ever get the second kind.
        const bool fromDestination = !o.convo.isChannel() && getFrom(&mp) == o.convo.peer;
        settle(o, fromDestination ? State::DELIVERED : State::RELAYED, nullptr);
        return true;
    }
    return false;
}

// The frozen fallback, for the sends the router never answers at all: a queue
// rejection it did not nak, or a reply lost on the way back (V3). WALL time, so
// sleeping through the wait still counts.
int32_t InkHUD::T5MessageModel::runOnce()
{
    pruneSends();

    bool waiting = false;
    for (Outgoing &o : sends) {
        if (o.state != State::SENDING || !o.deadlineMs)
            continue;
        if (Throttle::deadlinePassed(o.deadlineMs))
            settle(o, o.convo.isChannel() ? State::NOT_HEARD : State::FAILED, o.convo.isChannel() ? nullptr : "NO RESPONSE");
        else
            waiting = true;
    }

    if (!waiting)
        OSThread::disable();
    return 5 * 1000UL;
}

#endif
