#pragma once
#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_MESHBOT
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

/**
 * A text bot addressed by @ mention, the "@!<node id>" token the apps send when you pick a node
 * after typing @. On a channel a message is a command only when it opens with the bot's mention
 * ("@!a1b2c3d4 sun"); in a DM the mention is optional ("sun"). The bot listens on every channel the
 * node has, answers on the channel the command came in on, and threads each answer to the command
 * with reply_id. Commands that only need acknowledging (a vote, a stored rule) get an emoji reaction
 * instead of a text reply, which keeps them out of the chat log.
 *
 * Commands live in kCommands (see the .cpp). A per-sender cooldown keeps one node from flooding the
 * mesh, and a global airtime/rate backstop does the same for the bot as a whole. An admin key, the
 * same credential AdminModule's remote admin uses, is needed for commands that make the bot
 * transmit on someone's behalf (ackping, poll, notify) and for a seen lookup aimed at another node; a
 * bare seen reports on whoever asked, so the bot is no window onto peers it learned elsewhere.
 *
 * ackping waits for its routing ack in a single slot, correlated on the id of the packet it sent.
 * poll runs one poll at a time, announced and closed on the primary channel. notify keeps up to 8
 * admin rules in RAM, "when <node> <condition>, tell <who>" or a plain reminder, judged on what NodeDB
 * already holds; rules lists and deletes them. runOnce sweeps all three and sleeps to the next deadline.
 */
class MeshBotModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    MeshBotModule();

  protected:
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    int32_t runOnce() override;

    // Handler signature: given the received packet and any text after the command word,
    // write the reply into out (a caller-owned buffer of outLen bytes).
    using CommandHandler = void (MeshBotModule::*)(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);

    struct Command {
        const char *name; // the word after the bot's mention
        CommandHandler handler;
        bool privileged; // transmits or mutates state; gated on isPrivilegedSenderAuthorized()
    };

    // Dispatch table; defined in the .cpp. A class member so it can reference the private handlers.
    static const Command kCommands[];

    // Split a text payload into a command word and its argument tail. A broadcast must open with our
    // "@!<id>" mention; in a DM the mention is optional. Returns nullptr unless a command matches.
    const Command *parseCommand(const char *msg, bool isDM, const char **argsOut) const;

    // May this packet run a privileged (transmitting/mutating) command? Mirrors AdminModule's
    // remote-admin gate: PKI-decrypted, carrying a 32-byte public key listed in security.admin_key.
    static bool isPrivilegedSenderAuthorized(const meshtastic_MeshPacket &mp);

    // Strict "[ws][@][!]1-8 hex digits[ws]" node id. Rejects a sign, an 0x prefix, overflow and any
    // trailing junk - strtoul took all of those and silently aimed the command at another node.
    static bool parseNodeId(const char *s, uint32_t &out);

    // parseNodeId plus the target rules every action command shares: an id is required, and
    // broadcast, node 0 and our own NodeNum are refused. Writes the error into out when false.
    static bool requireNodeId(const char *args, uint32_t &num, char *out, size_t outLen);

    // Which node seen may report on. No argument means the caller; an explicit id is a peer lookup
    // and needs the same admin key the privileged commands do. Refused before the id is even parsed, so the refusal says nothing
    // about the node asked after - not whether it is known to us, and not whether it is a well-formed id. Unlike requireNodeId
    // this allows our own NodeNum: reading the bot's own record is not an action.
    static bool resolveLookupTarget(const meshtastic_MeshPacket &mp, const char *args, uint32_t &num, char *out, size_t outLen);

    // Answer rx where it came from, threaded to it (reply_id = rx.id) so apps show it as a reply.
    void sendReply(const meshtastic_MeshPacket &rx, const char *text);

    // Set by a handler, instead of writing out, when the whole answer is an acknowledgement: it goes
    // out as this emoji reacting to the command, which apps show on the message, not in the chat.
    const char *ackReaction = nullptr;

    // Send a text packet to an explicit destination/channel, bypassing the request/reply pairing.
    // False when nothing reached the router, so no caller reports a reply it never sent.
    // `replyId` threads the text to that packet; with `emoji` the text is a reaction to it instead.
    // `wantAck` has ReliableRouter retry it, for a text that exists nowhere else once sent.
    bool sendTextTo(uint32_t dest, uint8_t channel, const char *text, uint32_t replyId = 0, bool emoji = false,
                    bool wantAck = false);

    // Build and send one request packet toward a node. `payload`/`payloadLen` is the encoded request
    // body, empty for the portnums that answer a bare request. `wantAck` asks for reliable delivery
    // and the routing ack that comes with it; `wantResponse` asks the target's module for a reply.
    // `sentId`, when given, receives our packet's id - the only exact handle on its reply, which
    // carries it back in decoded.request_id. False (with the reason in out) if nothing was sent.
    bool sendRequestTo(const meshtastic_MeshPacket &rx, uint32_t dest, meshtastic_PortNum portnum, bool wantAck,
                       bool wantResponse, const uint8_t *payload, size_t payloadLen, uint32_t *sentId, char *out, size_t outLen);

    // Global backstop on everything MeshBot transmits. The per-sender cooldown bounds how often one
    // node can command the bot; this bounds the bot's own share of the channel, which no per-sender
    // window can do. Gates on the same airtime figures the periodic modules use, asked politely,
    // plus a minimum gap between our transmits.
    static bool txBackstopAllows();
    // Stamp a transmit for the rate limb of txBackstopAllows().
    static void noteTx();

    // Sunrise and sunset (UTC epoch seconds) for the local solar day containing utcNow at this
    // longitude. 0 for an ordinary day, 1 when the sun stays up all day, -1 when it stays down.
    static int sunRiseSet(double latDeg, double lonDeg, uint32_t utcNow, uint32_t &riseUtc, uint32_t &setUtc);
    // UTC epoch of 12:00 local on the local calendar day containing utcNow.
    static uint32_t clockDayNoonUtc(uint32_t utcNow, int32_t tzOffsetSecs);
    // Days since the last new moon, in [0, synodic month).
    static float moonAgeDays(uint32_t utcNow);

    // notify's token grammar. "<n>m|h|d" up to 7 days, "<n>m|km", and the seconds from a local
    // epoch second to the next local minuteOfDay, in (0, 86400].
    static bool parseDuration(const char *tok, uint32_t &secs);
    static bool parseDistance(const char *tok, uint32_t &meters);
    static uint32_t secondsUntil(uint32_t localNowSecs, uint16_t minuteOfDay);

    // Command handlers.
    void cmdHelp(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdPing(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdSignal(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdEcho(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdWhoami(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdVer(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdSeen(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdStats(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdSun(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdMoon(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdVote(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    // Action commands: trigger a transmit or mutate local state.
    void cmdAckPing(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdPoll(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdNotify(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdEvery(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdRules(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdRoll(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdFlip(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmd8ball(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);
    void cmdJoke(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen);

  private:
    // The one outstanding request the bot tracks, and where to deliver its answer: ackping waits for
    // the ROUTING_APP ack of its own packet and ends in one text message to whoever asked, or in a
    // timeout report. One slot, so a second request while one
    // is in flight is refused rather than queued - the bot has one radio and one voice.
    struct Pending {
        uint32_t requester = 0;
        uint32_t target = 0;
        uint32_t requestId = 0; // id of the packet we sent; a reply's request_id must equal it
        uint32_t replyId = 0;   // the command's id, so the answer threads to it
        uint32_t startedMs = 0;
        uint32_t timeoutMs = 0;
        uint8_t channel = 0;
        bool broadcast = false;
        bool active = false;
        meshtastic_PortNum waitingOn = meshtastic_PortNum_UNKNOWN_APP;
    };
    Pending pending;

    // Record a just-sent request so its reply can be matched and relayed. Starts the timeout clock.
    void armPending(const meshtastic_MeshPacket &rx, uint32_t target, uint32_t requestId, meshtastic_PortNum waitingOn,
                    uint32_t timeoutMs);

    // Is mp the reply our outstanding request is waiting for? Exact: same portnum, addressed to us,
    // request_id equal to the id of the packet we sent, and still inside that request's timeout -
    // so a reply arriving after we gave up cannot be credited to whatever is pending by then.
    bool matchesPending(const meshtastic_MeshPacket &mp) const;

    // Send the outstanding request's answer to whoever asked, and clear the slot.
    void finishPending(const char *text);

    // Decode the ROUTING_APP ack/nak for our ackping and report what it actually proves.
    void handleAckReply(const meshtastic_MeshPacket &mp);

    // A text message from the mesh: parse it, gate it, run the command and send its answer.
    void handleCommand(const meshtastic_MeshPacket &mp);

    // An admin's "when <node> <condition>, tell <who>", or a reminder (In/At). RAM only.
    struct Rule {
        enum Kind : uint8_t { Heard, Silent, Battery, Snr, Hops, Moved, Near, In, At };
        Kind kind;
        bool used, repeat, armed, alertDue;
        uint32_t owner;   // admin who made it
        uint32_t watched; // 0 for In/At
        uint32_t other;   // Near's second node
        uint32_t to;      // recipient node; 0 when channel alert
        uint8_t channel;  // the alert's channel; for a DM, the one the command came in on
        bool toChannel;
        char unit;              // 'm', 'h' or 'd' as typed, so a duration is shown the way it was written
        int32_t threshold;      // battery %, snr dB, hops, metres, seconds (Silent/In), minute-of-day (At)
        int32_t refLat, refLon; // Moved reference, 1e-7 deg; 0/0 = not yet known
        uint32_t createdMs, lastFireMs, lastHeardMs, dueMs;
        char note[160];

        // "notify @!x when @!x heard: msg", the old tell: the alert is the note alone.
        bool tellForm() const { return kind == Heard && !toChannel && to == watched && to != owner; }
    };
    Rule rules[8] = {};

    // Does any rule watch node n, as its node or as Near's second one?
    bool watches(NodeNum n) const;

    // Stamp what a packet from a watched node means for its rules, and wake runOnce to evaluate them:
    // NodeDB may not hold this packet's values until the modules after us have run.
    void noteHeard(const meshtastic_MeshPacket &mp);

    // Drop expired one-shot rules, decide which are due, and send due alerts the backstop allows.
    void evaluateRules();

    // Compose and send r's alert. True once it reached the router.
    bool sendAlert(const Rule &r, const char *facts);

    // notify and every notify: parse args into a free slot, or write why not into out.
    void addRule(const meshtastic_MeshPacket &mp, const char *args, bool repeat, char *out, size_t outLen);

    // "<cond> <node>><who>" for rules listing.
    void formatRule(const Rule &r, char *out, size_t outLen) const;

    // The one open poll, if any. RAM only, so a reboot drops it.
    struct Poll {
        struct Voter {
            uint32_t node;
            uint8_t choice;
        };
        bool open = false;
        uint32_t startedMs = 0;
        uint32_t durationMs = 0;
        char question[80] = {};
        char options[4][24] = {};
        uint8_t optionCount = 0;
        Voter voters[32] = {};
        uint8_t voterCount = 0;
    };
    Poll poll;

    // "<prefix><question> | a:3 b:5" for the poll as it stands.
    void formatTally(char *out, size_t outLen, const char *prefix) const;
};
#endif // MESHTASTIC_EXCLUDE_MESHBOT
