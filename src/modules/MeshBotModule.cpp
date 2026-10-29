#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_MESHBOT
// A mention-addressed bot for the mesh; see MeshBotModule.h. Excluded by default in platformio.ini -
// `#undef MESHTASTIC_EXCLUDE_MESHBOT` in a variant.h to enable it. Its privileged commands then need
// an admin key in config.security.admin_key, the same credential AdminModule's remote admin uses.

#include "Channels.h"
#include "MeshBotModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "RadioLibInterface.h"
#include "Router.h"
#include "UptimeClock.h"
#include "airtime.h"
#include "gps/GeoCoord.h"
#include "gps/RTC.h"
#include "mesh/MeshTypes.h"
#include "mesh/Throttle.h"
#include "mesh/mesh-pb-constants.h"

#include <Arduino.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

// Per-sender cooldown windows (milliseconds). The limiter is keyed on the sender, so it throttles
// how often any one node can command the bot. Override in a variant.h or with a build flag.
#ifndef MESHBOT_DM_COOLDOWN_MS
#define MESHBOT_DM_COOLDOWN_MS 3000
#endif
#ifndef MESHBOT_BROADCAST_COOLDOWN_MS
#define MESHBOT_BROADCAST_COOLDOWN_MS 8000
#endif

// Action commands transmit or mutate state, so they get a longer per-sender cooldown regardless of
// whether they arrived as a DM or a broadcast. Override in a variant.h or with a build flag.
#ifndef MESHBOT_PRIVILEGED_COOLDOWN_MS
#define MESHBOT_PRIVILEGED_COOLDOWN_MS 15000
#endif

// Minimum gap between any two MeshBot transmits, whoever asked for them. The per-sender cooldowns
// above bound one node; eight nodes clear them independently, and the bot's traffic is the most
// optional traffic on the channel. Override in a variant.h or with a build flag.
#ifndef MESHBOT_MIN_TX_GAP_MS
#define MESHBOT_MIN_TX_GAP_MS 2000
#endif

// Per-sender cooldown, tracked in a small ring buffer. Broadcasts get a longer window than DMs
// because a broadcast reply costs the whole channel's airtime.
namespace
{
struct CooldownEntry {
    uint32_t from = 0;
    uint32_t lastMs = 0;
};

constexpr uint8_t kCooldownSlots = 8;
constexpr uint32_t kDmCooldownMs = MESHBOT_DM_COOLDOWN_MS;
constexpr uint32_t kBroadcastCooldownMs = MESHBOT_BROADCAST_COOLDOWN_MS;
constexpr uint32_t kPrivilegedCooldownMs = MESHBOT_PRIVILEGED_COOLDOWN_MS;
constexpr uint32_t kMinTxGapMs = MESHBOT_MIN_TX_GAP_MS;

// Time of our last transmit, for the rate limb of txBackstopAllows(). 0 means "nothing sent yet".
uint32_t lastTxMs = 0;

CooldownEntry cooldown[kCooldownSlots];
uint8_t cooldownIdx = 0;

// How long an ackping waits for its routing ack before we report that none came.
constexpr uint32_t kAckTimeoutMs = 30000;

// How long a one-shot rule waits to fire before it is dropped, and the most often a repeating
// rule may fire.
constexpr uint32_t kRuleTtlMs = 7u * 24 * 3600 * 1000;
constexpr uint32_t kRepeatFloorMs = 15u * 60 * 1000;

constexpr const char *kNotifyUsage = "notify me|@!id|#N when @!id|me <cond> [: note] | in 2h | at 18:00";

constexpr const char *kReactQueued = "\xE2\x9C\x85";   // ✅
constexpr const char *kReactVote = "\xF0\x9F\x91\x8D"; // 👍

// Seed the PRNG once, lazily, on first use of a random command.
void ensureSeeded()
{
    static bool seeded = false;
    if (!seeded) {
        randomSeed(Time::getMillis());
        seeded = true;
    }
}

bool rateLimited(uint32_t from, uint32_t cooldownMs)
{
    const uint32_t now = Time::getMillis();
    for (auto &e : cooldown) {
        if (e.from == from) {
            if ((uint32_t)(now - e.lastMs) < cooldownMs)
                return true;
            e.lastMs = now;
            return false;
        }
    }
    cooldown[cooldownIdx].from = from;
    cooldown[cooldownIdx].lastMs = now;
    cooldownIdx = (cooldownIdx + 1) % kCooldownSlots;
    return false;
}

// snprintf at out+pos, advancing pos. Stops appending once the buffer is full.
void appendf(char *out, size_t outLen, size_t &pos, const char *fmt, ...)
{
    if (pos + 1 >= outLen)
        return;
    va_list ap;
    va_start(ap, fmt);
    const int written = vsnprintf(out + pos, outLen - pos, fmt, ap);
    va_end(ap);
    if (written > 0)
        pos = (pos + (size_t)written < outLen) ? pos + (size_t)written : outLen - 1;
}

// Short label for one hop. NODENUM_BROADCAST is the placeholder for a hop that stayed anonymous.
const char *hopLabel(uint32_t num, char *scratch, size_t scratchLen)
{
    if (num == NODENUM_BROADCAST)
        return "?";
    meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(num);
    if (n && nodeInfoLiteHasUser(n) && n->short_name[0] != '\0')
        return n->short_name;
    snprintf(scratch, scratchLen, "%04x", (unsigned)(num & 0xffff));
    return scratch;
}

// Map a relay_node byte back to a node. Only the low byte travels on the air, so several nodes can
// share one; an ambiguous or unmatched byte resolves to 0.
uint32_t resolveRelay(uint8_t relayByte)
{
    uint32_t found = 0;
    for (size_t i = 0; i < nodeDB->getNumMeshNodes(); i++) {
        meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!n || nodeDB->getLastByteOfNodeNum(n->num) != relayByte)
            continue;
        if (found)
            return 0; // byte collides across nodes, so we can't name one
        found = n->num;
    }
    return found;
}

// The caller's last known position, else the bot's own. 0,0 is what a node without a fix reports.
bool positionFor(NodeNum from, double &lat, double &lon)
{
    meshtastic_PositionLite p = meshtastic_PositionLite_init_default;
    for (NodeNum n : {from, nodeDB->getNodeNum()}) {
        if (nodeDB->copyNodePosition(n, p) && (p.latitude_i || p.longitude_i)) {
            lat = p.latitude_i * 1e-7;
            lon = p.longitude_i * 1e-7;
            return true;
        }
    }
    return false;
}

// A node's last known position. 0,0 is what a node without a fix reports.
bool knownPosition(NodeNum n, meshtastic_PositionLite &p)
{
    return nodeDB->copyNodePosition(n, p) && (p.latitude_i || p.longitude_i);
}

uint32_t metersBetween(const meshtastic_PositionLite &a, int32_t latI, int32_t lonI)
{
    return (uint32_t)lroundf(GeoCoord::latLongToMeter(a.latitude_i * 1e-7, a.longitude_i * 1e-7, latI * 1e-7, lonI * 1e-7));
}

// Strip leading and trailing whitespace in place.
char *trim(char *s)
{
    while (std::isspace(static_cast<unsigned char>(*s)))
        s++;
    char *end = s + strlen(s);
    while (end > s && std::isspace(static_cast<unsigned char>(end[-1])))
        *--end = '\0';
    return s;
}

// Split s in place on whitespace into tok. Returns the word count, or max + 1 when there are more.
int splitWords(char *s, char **tok, int max)
{
    int n = 0;
    for (;;) {
        while (std::isspace(static_cast<unsigned char>(*s)))
            s++;
        if (*s == '\0')
            return n;
        if (n == max)
            return max + 1;
        tok[n++] = s;
        while (*s != '\0' && !std::isspace(static_cast<unsigned char>(*s)))
            s++;
        if (*s != '\0')
            *s++ = '\0';
    }
}

// A whole token as a decimal integer, followed by exactly suffix.
bool parseLong(const char *tok, long &v, const char *suffix = "")
{
    char *end = nullptr;
    v = strtol(tok, &end, 10);
    return end != tok && strcmp(end, suffix) == 0;
}

// A node's short name, else "!" and its last 4 hex digits.
const char *nodeLabel(uint32_t num, char *scratch, size_t scratchLen)
{
    meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(num);
    if (n && nodeInfoLiteHasUser(n) && n->short_name[0] != '\0')
        return n->short_name;
    snprintf(scratch, scratchLen, "!%04x", (unsigned)(num & 0xffff));
    return scratch;
}

// A duration in the unit it was typed in: "60m", "2h", "1d".
void formatDuration(uint32_t secs, char unit, char *out, size_t outLen)
{
    snprintf(out, outLen, "%u%c", (unsigned)(secs / (unit == 'd' ? 86400 : unit == 'h' ? 3600 : 60)), unit);
}

// "HH:MM" of a UTC epoch second, shifted into the node's timezone.
void formatHm(uint32_t utc, int32_t tzOffset, char *out, size_t outLen)
{
    const uint32_t t = (uint32_t)(utc + tzOffset) % 86400;
    snprintf(out, outLen, "%02u:%02u", (unsigned)(t / 3600), (unsigned)(t % 3600 / 60));
}
} // namespace

// Nothing bounded what the bot as a whole put on the air: the cooldowns above are per sender, and
// eight senders clear them independently. A bot answers only because someone asked, so it yields
// first - this is the airtime check the periodic modules make, taken at the polite threshold (25%
// channel utilization rather than 40%, half the duty cycle rather than all of it), plus a floor on
// the gap between our own transmits.
//
// This is an admission gate on a whole command, checked once in handleReceived(), not a gate on each
// packet: one command legitimately transmits more than once - a request plus its confirmation, and
// for ackping the answer that arrives later - and a per-packet gate would drop the second half of
// its own command. What bounds those is the command: three packets at most, and the later ones only
// on the single pending slot, which no second command can arm. The two transmits that answer no
// command, a rule's alert and a poll's result, check this gate themselves before sending. A new
// command that transmits repeatedly, or a second pending slot, would break that and needs its own
// limit.
bool MeshBotModule::txBackstopAllows()
{
    if (lastTxMs && !Throttle::hasElapsed(lastTxMs, kMinTxGapMs))
        return false;
    // Null until main() builds it, and absent on a host test run; it gates nothing in that window.
    if (airTime && (!airTime->isTxAllowedChannelUtil(true) || !airTime->isTxAllowedAirUtil()))
        return false;
    return true;
}

void MeshBotModule::noteTx()
{
    lastTxMs = Time::stampMillis();
}

// AdminModule's remote-admin gate (AdminModule.cpp, the mp.pki_encrypted branch): PKI decryption
// binds mp.from into the AEAD nonce, so a 32-byte public_key matching a configured admin_key is the
// only sender proof we have. Fails closed, which makes the privileged commands DM-only in practice.
bool MeshBotModule::isPrivilegedSenderAuthorized(const meshtastic_MeshPacket &mp)
{
    if (!mp.pki_encrypted || mp.public_key.size != 32)
        return false;
    for (const auto &key : config.security.admin_key)
        if (key.size == 32 && memcmp(mp.public_key.bytes, key.bytes, 32) == 0)
            return true;
    return false;
}

bool MeshBotModule::parseNodeId(const char *s, uint32_t &out)
{
    if (!s)
        return false;
    while (std::isspace(static_cast<unsigned char>(*s)))
        s++;
    if (*s == '@') // a mention token, "@!433a1b2c"
        s++;
    if (*s == '!')
        s++;
    uint32_t v = 0;
    int digits = 0;
    for (; std::isxdigit(static_cast<unsigned char>(*s)); s++) {
        if (++digits > 8) // 8 hex digits is a whole NodeNum, so a 9th means the caller mistyped it
            return false;
        const char c = (char)std::tolower(static_cast<unsigned char>(*s));
        v = (v << 4) | (uint32_t)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    if (digits == 0)
        return false;
    while (std::isspace(static_cast<unsigned char>(*s)))
        s++;
    if (*s != '\0')
        return false;
    out = v;
    return true;
}

bool MeshBotModule::requireNodeId(const char *args, uint32_t &num, char *out, size_t outLen)
{
    if (!args || args[0] == '\0') {
        snprintf(out, outLen, "Need a node id");
        return false;
    }
    if (!parseNodeId(args, num)) {
        snprintf(out, outLen, "Bad node id");
        return false;
    }
    // Pointing an action command at the broadcast address turns one text message into a mesh-wide
    // request storm and a NAK from every node; node 0 and ourselves are requests with no answer.
    if (num == 0 || isBroadcast(num) || num == nodeDB->getNodeNum()) {
        snprintf(out, outLen, "Bad target");
        return false;
    }
    return true;
}

// Taking any NodeNum from anyone made the bot a public window onto every node it had ever heard.
// A caller may ask about themselves - the question they can already answer - or hold the admin key.
bool MeshBotModule::resolveLookupTarget(const meshtastic_MeshPacket &mp, const char *args, uint32_t &num, char *out,
                                        size_t outLen)
{
    if (!args || args[0] == '\0') {
        // mp.from is the only identity the packet carries, and on a channel-encrypted packet it is a
        // claim, not a proof - so a self lookup is only as self as the sender is honest. What it
        // costs an impersonator is that the answer goes to the node they named, on that node's
        // channel, rather than to them. 0 is a locally-injected packet and the broadcast address is
        // a forgery outright; neither names a node to report on.
        if (mp.from == 0 || isBroadcast(mp.from)) {
            snprintf(out, outLen, "No sender to look up");
            return false;
        }
        num = mp.from;
        return true;
    }
    // Ahead of parseNodeId, so the one refusal is all an unauthorized caller ever learns: a
    // well-formed id and a typo answer alike, and neither says whether we hold that record.
    if (!isPrivilegedSenderAuthorized(mp)) {
        snprintf(out, outLen, "Peer lookup is admin only");
        return false;
    }
    if (!parseNodeId(args, num)) {
        snprintf(out, outLen, "Bad node id");
        return false;
    }
    // Our own NodeNum is deliberately allowed - reading the bot's own record is not an action.
    if (num == 0 || isBroadcast(num)) {
        snprintf(out, outLen, "Bad target");
        return false;
    }
    return true;
}

MeshBotModule::MeshBotModule() : SinglePortModule("meshbot", meshtastic_PortNum_TEXT_MESSAGE_APP), OSThread("MeshBot")
{
    // Promiscuous so we also see channel broadcasts that mention us, not just DMs to us.
    isPromiscuous = true;
}

// The command table; order only affects how help lists them. The privileged flag marks a command
// that transmits or mutates state, which handleReceived gates on an admin key and a longer cooldown.
// clang-format off
const MeshBotModule::Command MeshBotModule::kCommands[] = {
    {"help", &MeshBotModule::cmdHelp, false},
    {"ping", &MeshBotModule::cmdPing, false},
    {"signal", &MeshBotModule::cmdSignal, false},
    {"echo", &MeshBotModule::cmdEcho, false},
    {"whoami", &MeshBotModule::cmdWhoami, false},
    {"ver", &MeshBotModule::cmdVer, false},
    {"seen", &MeshBotModule::cmdSeen, false},
    {"stats", &MeshBotModule::cmdStats, false},
    {"sun", &MeshBotModule::cmdSun, false},
    {"moon", &MeshBotModule::cmdMoon, false},
    {"ackping", &MeshBotModule::cmdAckPing, true},
    {"poll", &MeshBotModule::cmdPoll, true},
    {"notify", &MeshBotModule::cmdNotify, true},
    {"every", &MeshBotModule::cmdEvery, true},
    {"rules", &MeshBotModule::cmdRules, true},
    {"vote", &MeshBotModule::cmdVote, false},
    {"roll", &MeshBotModule::cmdRoll, false},
    {"flip", &MeshBotModule::cmdFlip, false},
    {"8ball", &MeshBotModule::cmd8ball, false},
    {"joke", &MeshBotModule::cmdJoke, false},
};
// clang-format on

bool MeshBotModule::wantPacket(const meshtastic_MeshPacket *p)
{
    if (!p)
        return false;
    // Commands, the one reply our outstanding request is waiting for, and any packet from a node a
    // rule watches. Anyone else's routing traffic isn't ours to touch, and matching requests here
    // would put us in the running to answer them.
    return p->decoded.portnum == ourPortNum || matchesPending(*p) || watches(p->from);
}

bool MeshBotModule::matchesPending(const meshtastic_MeshPacket &mp) const
{
    // request_id is the id of the packet we sent, so equality is proof of correlation. Matching on
    // the peer's NodeNum instead - as this did - also accepted its ack of somebody else's packet,
    // its ack of an earlier ping of ours, and anything from a node that cared to forge `from`.
    return pending.active && mp.decoded.portnum == pending.waitingOn && mp.decoded.request_id == pending.requestId &&
           mp.to == nodeDB->getNodeNum() && Throttle::isWithinTimespanMs(pending.startedMs, pending.timeoutMs);
}

ProcessMessage MeshBotModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Ahead of the command, so the packet that creates a heard rule does not also fire it.
    noteHeard(mp);
    if (mp.decoded.portnum != ourPortNum) {
        if (matchesPending(mp))
            handleAckReply(mp);
    } else {
        handleCommand(mp);
    }
    return ProcessMessage::CONTINUE;
}

void MeshBotModule::handleCommand(const meshtastic_MeshPacket &mp)
{
    // The whole receive policy: a DM, or a broadcast on any channel this node has. parseCommand then
    // requires a broadcast to open with our mention, so channel chat is never read as a command.
    const bool isDM = (mp.to == nodeDB->getNodeNum());
    if (!isDM && !isBroadcast(mp.to))
        return;

    if (mp.decoded.payload.size == 0)
        return;

    // Copy the payload into a null-terminated buffer.
    char buf[256];
    size_t n = mp.decoded.payload.size;
    if (n > sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    memcpy(buf, mp.decoded.payload.bytes, n);
    buf[n] = '\0';

    const char *args = nullptr;
    const Command *cmd = parseCommand(buf, isDM, &args);
    if (!cmd)
        return;

    // Read-only commands answer anyone; privileged (transmitting/mutating) commands need an admin
    // key, so a random node can't spend our airtime or edit our state by typing someone else's id.
    if (cmd->privileged && !isPrivilegedSenderAuthorized(mp))
        return;

    // Ahead of the per-sender window, which has the side effect of consuming the sender's turn: a
    // command the backstop drops should not also cost them their next one. Silent, because the only
    // thing we could say about it would itself be the transmit we are declining.
    if (!txBackstopAllows())
        return;

    const uint32_t cooldownMs = cmd->privileged ? kPrivilegedCooldownMs : (isDM ? kDmCooldownMs : kBroadcastCooldownMs);
    if (rateLimited(mp.from, cooldownMs))
        return;

    char reply[200];
    reply[0] = '\0';
    ackReaction = nullptr;
    (this->*(cmd->handler))(mp, args, reply, sizeof(reply));
    if (ackReaction)
        sendTextTo(isBroadcast(mp.to) ? NODENUM_BROADCAST : mp.from, mp.channel, ackReaction, mp.id, true);
    else if (reply[0] != '\0')
        sendReply(mp, reply);
}

const MeshBotModule::Command *MeshBotModule::parseCommand(const char *msg, bool isDM, const char **argsOut) const
{
    if (!msg)
        return nullptr;
    while (*msg == ' ' || *msg == '\t')
        msg++;
    // A mention is plain text on the wire, "@!" and our NodeNum in hex (meshtastic/design#21). Apps
    // emit lowercase but Android can emit uppercase, hence strncasecmp.
    char mention[12];
    const int mentionLen = snprintf(mention, sizeof(mention), "@!%08x", (unsigned)nodeDB->getNodeNum());
    if (strncasecmp(msg, mention, mentionLen) == 0 && std::isspace(static_cast<unsigned char>(msg[mentionLen]))) {
        msg += mentionLen;
        while (std::isspace(static_cast<unsigned char>(*msg)))
            msg++;
    } else if (!isDM) {
        return nullptr;
    }

    // The command word ends at the first whitespace or end-of-string.
    const char *wordEnd = msg;
    while (*wordEnd != '\0' && !std::isspace(static_cast<unsigned char>(*wordEnd)))
        wordEnd++;
    const size_t wordLen = wordEnd - msg;
    if (wordLen == 0)
        return nullptr;

    for (const auto &c : kCommands) {
        if (strlen(c.name) == wordLen && strncasecmp(msg, c.name, wordLen) == 0) {
            const char *args = wordEnd;
            while (*args == ' ' || *args == '\t')
                args++;
            if (argsOut)
                *argsOut = args;
            return &c;
        }
    }
    return nullptr;
}

void MeshBotModule::sendReply(const meshtastic_MeshPacket &rx, const char *text)
{
    // Reply in kind: a channel broadcast is answered on-channel, a DM is answered as a DM.
    sendTextTo(isBroadcast(rx.to) ? NODENUM_BROADCAST : rx.from, rx.channel, text, rx.id);
}

bool MeshBotModule::sendTextTo(uint32_t dest, uint8_t channel, const char *text, uint32_t replyId, bool emoji, bool wantAck)
{
    // service and router are null until the radio comes up; allocDataPacket() dereferences router.
    if (!text || text[0] == '\0' || !service || !router)
        return false;
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return false;
    p->to = dest;
    p->channel = channel;
    p->want_ack = wantAck;
    p->decoded.want_response = false;
    p->decoded.reply_id = replyId;
    p->decoded.emoji = emoji;
    size_t len = strlen(text);
    if (len > sizeof(p->decoded.payload.bytes))
        len = sizeof(p->decoded.payload.bytes);
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, text, len);
    const ErrorCode res = service->sendToMesh(p);
    // ERRNO_SHOULD_RELEASE is "delivered locally, caller frees", not a failure; sendToMesh() has
    // already released p either way, so there is nothing to clean up here.
    if (res != ERRNO_OK && res != ERRNO_SHOULD_RELEASE) {
        LOG_WARN("MeshBot reply to 0x%08x dropped, sendToMesh=%d", (unsigned)dest, (int)res);
        return false;
    }
    noteTx();
    return true;
}

void MeshBotModule::cmdHelp(const meshtastic_MeshPacket &, const char *args, char *out, size_t outLen)
{
    // "help" lists public commands; "help all" lists the privileged action commands. Splitting
    // keeps each reply under the single-packet payload limit.
    const bool wantActions = args && strncmp(args, "all", 3) == 0;
    size_t pos = 0;
    int written = snprintf(out, outLen, "%s", wantActions ? "Actions:" : "Commands:");
    if (written > 0)
        pos = (size_t)written;
    for (const auto &c : kCommands) {
        if (c.privileged != wantActions)
            continue;
        if (pos >= outLen)
            break;
        written = snprintf(out + pos, outLen - pos, " %s", c.name);
        if (written <= 0)
            break;
        pos += (size_t)written;
    }
    if (!wantActions && pos < outLen)
        snprintf(out + pos, outLen - pos, " +help all | lookup: bare=you, !id=admin");
}

void MeshBotModule::cmdPing(const meshtastic_MeshPacket &, const char *, char *out, size_t outLen)
{
    snprintf(out, outLen, "pong");
}

void MeshBotModule::cmdSignal(const meshtastic_MeshPacket &mp, const char *, char *out, size_t outLen)
{
    const int hops = getHopsAway(mp);
    if (hops == 0) {
        int rssi = mp.rx_rssi;
        if (rssi > 0)
            rssi -= 200;
        snprintf(out, outLen, "Direct | RSSI %d | SNR %.1f", rssi, (double)mp.rx_snr);
        return;
    }

    // Relayed: our RSSI/SNR measure the last leg, not the sender, so name the relay instead.
    size_t pos = 0;
    if (hops < 0)
        appendf(out, outLen, pos, "? hops");
    else
        appendf(out, outLen, pos, "%d hops", hops);

    if (mp.relay_node == NO_RELAY_NODE) {
        appendf(out, outLen, pos, " | relay unknown");
        return;
    }
    const uint32_t relay = resolveRelay(mp.relay_node);
    meshtastic_NodeInfoLite *n = relay ? nodeDB->getMeshNode(relay) : nullptr;
    if (n && nodeInfoLiteHasUser(n) && n->short_name[0])
        appendf(out, outLen, pos, " | via %s !%08x", n->short_name, (unsigned)relay);
    else if (relay)
        appendf(out, outLen, pos, " | via !%08x", (unsigned)relay);
    else
        appendf(out, outLen, pos, " | via relay 0x%02x", (unsigned)mp.relay_node);
}

void MeshBotModule::cmdEcho(const meshtastic_MeshPacket &, const char *args, char *out, size_t outLen)
{
    if (args && args[0] != '\0')
        snprintf(out, outLen, "%s", args);
    else
        snprintf(out, outLen, "echo what?");
}

void MeshBotModule::cmdWhoami(const meshtastic_MeshPacket &mp, const char *, char *out, size_t outLen)
{
    snprintf(out, outLen, "You !%08x | Me %s !%08x", (unsigned)mp.from, owner.long_name, (unsigned)nodeDB->getNodeNum());
}

void MeshBotModule::cmdVer(const meshtastic_MeshPacket &, const char *, char *out, size_t outLen)
{
    snprintf(out, outLen, "FW %s", optstr(APP_VERSION));
}

void MeshBotModule::cmdSeen(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen)
{
    uint32_t num;
    if (!resolveLookupTarget(mp, args, num, out, outLen))
        return;
    meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(num);
    if (!n) {
        snprintf(out, outLen, "Unknown node !%08x", (unsigned)num);
        return;
    }
    if (n->last_heard == 0) {
        snprintf(out, outLen, "Never heard");
        return;
    }
    uint32_t secs = sinceLastSeen(n);
    uint32_t days = secs / 86400;
    uint32_t hours = (secs % 86400) / 3600;
    uint32_t mins = (secs % 3600) / 60;
    if (days > 0)
        snprintf(out, outLen, "Seen %ud %uh ago", (unsigned)days, (unsigned)hours);
    else if (hours > 0)
        snprintf(out, outLen, "Seen %uh %um ago", (unsigned)hours, (unsigned)mins);
    else if (mins > 0)
        snprintf(out, outLen, "Seen %um ago", (unsigned)mins);
    else
        snprintf(out, outLen, "Seen %us ago", (unsigned)secs);
}

void MeshBotModule::cmdStats(const meshtastic_MeshPacket &, const char *, char *out, size_t outLen)
{
    const uint32_t secs = Time::getUptimeSecs();
    size_t pos = 0;
    appendf(out, outLen, pos, "Up %ud%uh%um", (unsigned)(secs / 86400), (unsigned)(secs % 86400 / 3600),
            (unsigned)(secs % 3600 / 60));
    // The counters DeviceTelemetry's local stats report, left out rather than printed as 0 where this
    // node has no such source.
    if (RadioLibInterface::instance && router) {
        const RadioLibInterface *r = RadioLibInterface::instance;
        appendf(out, outLen, pos, " | RX %u bad %u dupe %u | TX %u", (unsigned)(r->rxGood + r->rxBad), (unsigned)r->rxBad,
                (unsigned)router->rxDupe, (unsigned)r->txGood);
    }
    if (airTime)
        appendf(out, outLen, pos, " | ChUtil %.1f%% AirTx %.1f%%", (double)airTime->channelUtilizationPercent(),
                (double)airTime->utilizationTXPercent());
    appendf(out, outLen, pos, " | Nodes %u/%u", (unsigned)nodeDB->getNumOnlineMeshNodes(), (unsigned)nodeDB->getNumMeshNodes());
}

int MeshBotModule::sunRiseSet(double latDeg, double lonDeg, uint32_t utcNow, uint32_t &riseUtc, uint32_t &setUtc)
{
    // The NOAA sunrise equation. Day numbers count from J2000 (JD 2451545.0, 2000-01-01 12:00 UTC);
    // the Unix epoch is JD 2440587.5. Accurate to a minute or two, which is all a sunrise needs.
    constexpr double kUnixJd = 2440587.5, kJ2000 = 2451545.0, kRad = M_PI / 180.0;
    // The solar noon nearest now at this longitude, so the day reported is the local one: at 01:00
    // in Sydney the UTC date is still yesterday.
    const double n = floor(utcNow / 86400.0 + kUnixJd - kJ2000 + lonDeg / 360.0 + 0.5);
    const double js = n - lonDeg / 360.0;
    const double m = fmod(357.5291 + 0.98560028 * js, 360.0) * kRad;
    const double c = 1.9148 * sin(m) + 0.02 * sin(2 * m) + 0.0003 * sin(3 * m);
    const double l = fmod(m / kRad + c + 180.0 + 102.9372, 360.0) * kRad;
    const double transit = kJ2000 + js + 0.0053 * sin(m) - 0.0069 * sin(2 * l);
    const double decl = asin(sin(l) * sin(23.4397 * kRad));
    const double cosW = (sin(-0.833 * kRad) - sin(latDeg * kRad) * sin(decl)) / (cos(latDeg * kRad) * cos(decl));
    if (cosW < -1)
        return 1;
    if (cosW > 1)
        return -1;
    const double halfDay = acos(cosW) / (2 * M_PI); // in days
    riseUtc = (uint32_t)((transit - halfDay - kUnixJd) * 86400.0);
    setUtc = (uint32_t)((transit + halfDay - kUnixJd) * 86400.0);
    return 0;
}

uint32_t MeshBotModule::clockDayNoonUtc(uint32_t utcNow, int32_t tzOffsetSecs)
{
    const uint32_t local = utcNow + tzOffsetSecs;
    return local - local % 86400 + 43200 - tzOffsetSecs;
}

float MeshBotModule::moonAgeDays(uint32_t utcNow)
{
    // Mean synodic month from the new moon of 2000-01-06 18:14 UTC; drifts under a day from the true one.
    constexpr double kSynodicDays = 29.530588853;
    constexpr uint32_t kNewMoonUtc = 947182440;
    return (float)fmod((utcNow - kNewMoonUtc) / 86400.0, kSynodicDays);
}

void MeshBotModule::cmdSun(const meshtastic_MeshPacket &mp, const char *, char *out, size_t outLen)
{
    double lat, lon;
    if (!positionFor(mp.from, lat, lon)) {
        snprintf(out, outLen, "no position");
        return;
    }
    const uint32_t now = getValidTime(RTCQualityDevice);
    if (!now) {
        snprintf(out, outLen, "no valid time");
        return;
    }
    const int32_t tz = (int32_t)(getValidTime(RTCQualityDevice, true) - now);
    uint32_t rise = 0, set = 0;
    const int day = sunRiseSet(lat, lon, clockDayNoonUtc(now, tz), rise, set);
    if (day != 0) {
        snprintf(out, outLen, "sun %s all day", day > 0 ? "up" : "down");
        return;
    }
    char riseHm[8], setHm[8];
    formatHm(rise, tz, riseHm, sizeof(riseHm));
    formatHm(set, tz, setHm, sizeof(setHm));
    const uint32_t mins = (set - rise) / 60;
    snprintf(out, outLen, "Sunrise %s Sunset %s (%uh%02um)", riseHm, setHm, (unsigned)(mins / 60), (unsigned)(mins % 60));
}

void MeshBotModule::cmdMoon(const meshtastic_MeshPacket &, const char *, char *out, size_t outLen)
{
    static const char *const phases[] = {"New moon",  "Waxing crescent", "First quarter", "Waxing gibbous",
                                         "Full moon", "Waning gibbous",  "Last quarter",  "Waning crescent"};
    constexpr float kSynodicDays = 29.530588853f;
    const uint32_t now = getValidTime(RTCQualityDevice);
    if (!now) {
        snprintf(out, outLen, "no valid time");
        return;
    }
    const float age = moonAgeDays(now);
    const int phase = (int)(age / kSynodicDays * 8 + 0.5f) % 8; // centred on the four named points
    const unsigned lit = (unsigned)lroundf((1 - cosf(2 * (float)M_PI * age / kSynodicDays)) / 2 * 100);
    // Outside the full phase the countdown is at least ~2d and at most ~28d, so it never reads 0d or
    // rounds past a whole cycle.
    if (phase == 4) {
        snprintf(out, outLen, "%s %u%%", phases[phase], lit);
        return;
    }
    float toFull = kSynodicDays / 2 - age;
    if (toFull < 0)
        toFull += kSynodicDays;
    snprintf(out, outLen, "%s %u%% | full in %ud", phases[phase], lit, (unsigned)lroundf(toFull));
}

bool MeshBotModule::sendRequestTo(const meshtastic_MeshPacket &rx, uint32_t dest, meshtastic_PortNum portnum, bool wantAck,
                                  bool wantResponse, const uint8_t *payload, size_t payloadLen, uint32_t *sentId, char *out,
                                  size_t outLen)
{
    if (!service || !router) {
        snprintf(out, outLen, "Radio unavailable");
        return false;
    }
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p) {
        snprintf(out, outLen, "No packet buffer");
        return false;
    }
    p->to = dest;
    p->channel = rx.channel;
    p->decoded.portnum = portnum;
    p->want_ack = wantAck;
    p->decoded.want_response = wantResponse;
    p->decoded.payload.size = 0;
    if (payload && payloadLen > 0) {
        if (payloadLen > sizeof(p->decoded.payload.bytes)) {
            packetPool.release(p);
            snprintf(out, outLen, "Request too big");
            return false;
        }
        memcpy(p->decoded.payload.bytes, payload, payloadLen);
        p->decoded.payload.size = (pb_size_t)payloadLen;
    }
    // Read the id before the send: sendToMesh() may have returned p to the pool by the time it ends.
    const uint32_t id = p->id;
    const ErrorCode res = service->sendToMesh(p);
    if (res != ERRNO_OK && res != ERRNO_SHOULD_RELEASE) {
        snprintf(out, outLen, "Send failed");
        return false;
    }
    noteTx();
    if (sentId)
        *sentId = id;
    return true;
}

void MeshBotModule::armPending(const meshtastic_MeshPacket &rx, uint32_t target, uint32_t requestId, meshtastic_PortNum waitingOn,
                               uint32_t timeoutMs)
{
    pending.requester = rx.from;
    pending.target = target;
    pending.requestId = requestId;
    pending.replyId = rx.id;
    pending.startedMs = Time::stampMillis();
    pending.timeoutMs = timeoutMs;
    pending.channel = rx.channel;
    pending.broadcast = isBroadcast(rx.to);
    pending.waitingOn = waitingOn;
    pending.active = true;
    setIntervalFromNow(1000);
}

void MeshBotModule::finishPending(const char *text)
{
    const uint32_t dest = pending.broadcast ? NODENUM_BROADCAST : pending.requester;
    const uint8_t channel = pending.channel;
    pending.active = false; // before the send, so a failed send cannot leave the slot stuck
    sendTextTo(dest, channel, text, pending.replyId);
}

// What a routing ack actually proves, which is less than "delivered" in two of the three cases.
void MeshBotModule::handleAckReply(const meshtastic_MeshPacket &mp)
{
    meshtastic_Routing r = meshtastic_Routing_init_default;
    if (!pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_Routing_msg, &r))
        return;

    const uint32_t target = pending.target;
    const uint32_t elapsed = Time::getMillis() - pending.startedMs;
    char reply[96];
    size_t pos = 0;

    if (r.which_variant == meshtastic_Routing_error_reason_tag && r.error_reason != meshtastic_Routing_Error_NONE) {
        // ReliableRouter raises NO_CHANNEL and PKI_UNKNOWN_PUBKEY from inside its isToUs() branch,
        // so a nak carrying the target's own NodeNum means the packet did arrive and only the
        // decrypt failed. "reached" keeps that apart from a nak our own router raised - which is
        // MAX_RETRANSMIT, once the retries are spent - and which proves nothing about the far end.
        // The errors raised before the packet is queued at all (NO_INTERFACE, DUTY_CYCLE_LIMIT, an
        // encode failure) never arrive here: sendRequestTo() sees them as a failed send and arms no
        // pending slot, so they are reported as "Send failed" instead.
        appendf(reply, sizeof(reply), pos, "Ack !%08x: %s, err %u", (unsigned)target, mp.from == target ? "reached" : "no ack",
                (unsigned)r.error_reason);
    } else if (mp.from == target) {
        // Unauthenticated: any node on the channel can forge `from`, so this is the firmware's own
        // delivery confirmation and no stronger. See mesh/AckProof.h for the pairwise proof.
        appendf(reply, sizeof(reply), pos, "Ack !%08x: ok %ums", (unsigned)target, (unsigned)elapsed);
        const int hops = getHopsAway(mp);
        if (hops >= 0)
            appendf(reply, sizeof(reply), pos, " | %dhops", hops);
    } else {
        // An implicit ack: ReliableRouter::perhapsGenerateImplicitAckForOwnOverheard() fires when we
        // hear a neighbour rebroadcast our own packet, and acks it to us on the strength of that -
        // so this ack comes from us, carrying the relaying node in relay_node. It proves forwarding
        // progress on the first leg, not that the target has the packet. Anything else that ends up
        // here (no intermediate router emits a success ack for a packet it only relays, and a forged
        // one could) is reported the same conservative way rather than as delivery.
        char scratch[8];
        const uint32_t relay = (mp.relay_node == NO_RELAY_NODE) ? 0 : resolveRelay(mp.relay_node);
        appendf(reply, sizeof(reply), pos, "Ack !%08x: relayed %ums", (unsigned)target, (unsigned)elapsed);
        if (relay)
            appendf(reply, sizeof(reply), pos, " by %s", hopLabel(relay, scratch, sizeof(scratch)));
    }

    finishPending(reply);
}

int32_t MeshBotModule::runOnce()
{
    if (pending.active && !Throttle::isWithinTimespanMs(pending.startedMs, pending.timeoutMs)) {
        char reply[48];
        snprintf(reply, sizeof(reply), "Ack !%08x: no response", (unsigned)pending.target);
        finishPending(reply);
    }
    // Past the deadline the poll takes no more votes; the result goes out once the backstop allows.
    if (poll.open && !Throttle::isWithinTimespanMs(poll.startedMs, poll.durationMs) && txBackstopAllows()) {
        char text[220];
        formatTally(text, sizeof(text), "RESULT: ");
        if (sendTextTo(NODENUM_BROADCAST, channels.getPrimaryIndex(), text))
            poll.open = false;
    }
    evaluateRules();
    if (pending.active)
        return 1000;

    // Sleep to the earliest deadline; something past due but held by the backstop retries after a gap.
    uint32_t wait = UINT32_MAX;
    const auto until = [&](uint32_t ms) { wait = std::min(wait, ms ? ms : kMinTxGapMs); };
    if (poll.open)
        until(Throttle::remainingMs(poll.startedMs, poll.durationMs));
    const uint32_t now = Time::getMillis();
    for (const auto &r : rules) {
        if (!r.used)
            continue;
        if (r.alertDue)
            until(0);
        else if (!r.repeat)
            until(Throttle::remainingMs(r.createdMs, kRuleTtlMs));
        if (r.kind == Rule::In || r.kind == Rule::At)
            until(Throttle::deadlinePassedAt(now, r.dueMs) ? 0 : r.dueMs - now);
        // The silence deadline, and the end of a repeat floor that may be holding a condition back.
        if (const uint32_t ms = r.kind == Rule::Silent && r.armed ? Throttle::remainingMs(r.lastHeardMs, r.threshold * 1000u) : 0)
            until(ms);
        if (const uint32_t ms = r.repeat ? Throttle::remainingMs(r.lastFireMs, kRepeatFloorMs) : 0)
            until(ms);
    }
    return wait == UINT32_MAX ? INT32_MAX : (int32_t)std::max(wait, (uint32_t)100);
}

void MeshBotModule::noteHeard(const meshtastic_MeshPacket &mp)
{
    if (!watches(mp.from))
        return;
    for (auto &r : rules) {
        if (!r.used || r.watched != mp.from)
            continue;
        r.lastHeardMs = Time::getMillis();
        // A repeating heard rule re-arms on the first packet a repeat floor after its last alert.
        if (r.kind == Rule::Heard && Throttle::hasElapsed(r.lastFireMs, kRepeatFloorMs))
            r.alertDue = true;
    }
    setIntervalFromNow(500);
}

void MeshBotModule::evaluateRules()
{
    for (auto &r : rules) {
        if (!r.used)
            continue;
        // Level-checked: a condition already true when the rule is made fires on the first sweep.
        // fire is "the condition holds", clear is "it has cleared by the re-arm margin".
        char facts[48], d[12];
        bool fire = false, clear = false;
        const meshtastic_NodeInfoLite *info = nodeDB->getMeshNode(r.watched);
        meshtastic_DeviceMetrics m = meshtastic_DeviceMetrics_init_zero;
        meshtastic_PositionLite here = meshtastic_PositionLite_init_zero, there = meshtastic_PositionLite_init_zero;
        const bool placed = (r.kind == Rule::Moved || r.kind == Rule::Near) && knownPosition(r.watched, here);
        snprintf(facts, sizeof(facts), "!%08x", (unsigned)r.watched); // if the node's data is gone by now
        switch (r.kind) {
        case Rule::Heard:
            snprintf(facts, sizeof(facts), "!%08x heard", (unsigned)r.watched);
            break;
        case Rule::Silent:
            formatDuration((uint32_t)r.threshold, r.unit, d, sizeof(d));
            snprintf(facts, sizeof(facts), "!%08x silent %s", (unsigned)r.watched, d);
            fire = Throttle::hasElapsed(r.lastHeardMs, (uint32_t)r.threshold * 1000);
            clear = !fire; // heard again
            break;
        case Rule::Battery: // 101 means external power, which is below no threshold
            if (nodeDB->copyNodeTelemetry(r.watched, m) && m.has_battery_level) {
                snprintf(facts, sizeof(facts), "!%08x battery %u%%", (unsigned)r.watched, (unsigned)m.battery_level);
                fire = m.battery_level < (uint32_t)r.threshold;
                clear = m.battery_level >= std::min((uint32_t)r.threshold + 5, (uint32_t)100); // full or on power counts
            }
            break;
        case Rule::Snr: // 0 is no reading until the bot has heard the node over LoRa
            if (nodeInfoLiteHasSnr(info)) {
                snprintf(facts, sizeof(facts), "!%08x SNR %.1f", (unsigned)r.watched, (double)info->snr);
                fire = info->snr < r.threshold;
                clear = info->snr >= r.threshold + 3;
            }
            break;
        case Rule::Hops:
            if (info && info->has_hops_away) {
                snprintf(facts, sizeof(facts), "!%08x %u hops", (unsigned)r.watched, (unsigned)info->hops_away);
                fire = info->hops_away > r.threshold;
                clear = !fire;
            }
            break;
        case Rule::Moved:
            if (placed && !r.refLat && !r.refLon) { // no position when the rule was made: this is the spot
                r.refLat = here.latitude_i;
                r.refLon = here.longitude_i;
            } else if (placed) {
                const uint32_t dist = metersBetween(here, r.refLat, r.refLon);
                snprintf(facts, sizeof(facts), "!%08x moved %um", (unsigned)r.watched, (unsigned)dist);
                fire = dist > (uint32_t)r.threshold;
                clear = !fire;
            }
            break;
        case Rule::Near:
            if (placed && knownPosition(r.other, there)) {
                const uint32_t dist = metersBetween(here, there.latitude_i, there.longitude_i);
                snprintf(facts, sizeof(facts), "!%08x near !%08x (%um)", (unsigned)r.watched, (unsigned)r.other, (unsigned)dist);
                fire = dist <= (uint32_t)r.threshold;
                clear = dist > (uint32_t)r.threshold * 3 / 2;
            }
            break;
        case Rule::In:
        case Rule::At:
            snprintf(facts, sizeof(facts), "reminder");
            r.alertDue |= Throttle::deadlinePassed(r.dueMs);
            break;
        default:
            break;
        }
        r.armed |= clear;
        if (fire && r.armed && Throttle::hasElapsed(r.lastFireMs, kRepeatFloorMs))
            r.alertDue = true;
        // Expired only after the condition had its say, so one due as the TTL runs out still fires;
        // a due alert the backstop holds outlives it, since it was held, not refused.
        if (!r.repeat && !r.alertDue && Throttle::hasElapsed(r.createdMs, kRuleTtlMs)) {
            r.used = false;
            continue;
        }
        if (!r.alertDue || !txBackstopAllows() || !sendAlert(r, facts))
            continue;
        r.alertDue = false;
        if (!r.repeat) {
            r.used = false;
            continue;
        }
        r.lastFireMs = Time::getMillis();
        r.armed = false;
        if (r.kind == Rule::Moved) { // the next move is measured from here
            r.refLat = here.latitude_i;
            r.refLon = here.longitude_i;
        }
        if (r.kind == Rule::In)
            r.dueMs += (uint32_t)r.threshold * 1000;
        else if (r.kind == Rule::At) {
            // Back on the local clock, which follows DST and clock corrections. A fire a moment early
            // by that clock would make the same time due again, hence the floor.
            const uint32_t local = getValidTime(RTCQualityDevice, true);
            uint32_t secs = local ? secondsUntil(local, (uint16_t)r.threshold) : 86400;
            if (secs < kRepeatFloorMs / 1000)
                secs += 86400;
            r.dueMs = r.lastFireMs + secs * 1000;
        }
    }
}

bool MeshBotModule::sendAlert(const Rule &r, const char *facts)
{
    char text[240];
    size_t pos = 0;
    if (r.toChannel || r.to != r.owner) {
        meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(r.owner);
        if (n && nodeInfoLiteHasUser(n) && n->short_name[0])
            appendf(text, sizeof(text), pos, "From %s: ", n->short_name);
        else
            appendf(text, sizeof(text), pos, "From !%08x: ", (unsigned)r.owner);
    }
    if (r.tellForm())
        appendf(text, sizeof(text), pos, "%s", r.note);
    else if (r.note[0])
        appendf(text, sizeof(text), pos, "%s: %s", facts, r.note);
    else
        appendf(text, sizeof(text), pos, "%s", facts);
    // A DM alert is the rule's only copy once sent, so ReliableRouter retries it across hops.
    return sendTextTo(r.toChannel ? NODENUM_BROADCAST : r.to, r.channel, text, 0, false, !r.toChannel);
}

void MeshBotModule::cmdPoll(const meshtastic_MeshPacket &, const char *args, char *out, size_t outLen)
{
    static const char *const kUsage = "poll <1-1440 min> question | a | b";
    if (poll.open) {
        snprintf(out, outLen, "poll already open");
        return;
    }
    char *rest = nullptr;
    const long mins = strtol(args ? args : "", &rest, 10);
    if (mins < 1 || mins > 1440 || !std::isspace(static_cast<unsigned char>(*rest))) {
        snprintf(out, outLen, "%s", kUsage);
        return;
    }
    // "question | a | b": one question and up to four options, split on '|'. A sixth field means
    // too many options, which is all we need to know about the fields past it.
    char buf[200];
    strncpy(buf, rest, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *fields[6];
    int count = 0;
    for (char *f = buf; f && count < 6;) {
        char *bar = strchr(f, '|');
        if (bar)
            *bar = '\0';
        fields[count++] = trim(f);
        f = bar ? bar + 1 : nullptr;
    }
    if (count == 6) {
        snprintf(out, outLen, "max 4 options");
        return;
    }
    for (int i = 0; i < count; i++) {
        if (fields[i][0] == '\0' || count == 2) { // an empty part, or a single option to choose from
            snprintf(out, outLen, "%s", kUsage);
            return;
        }
        if (strlen(fields[i]) >= (i == 0 ? sizeof(poll.question) : sizeof(poll.options[0]))) {
            snprintf(out, outLen, "too long");
            return;
        }
    }

    poll = Poll{};
    strncpy(poll.question, fields[0], sizeof(poll.question) - 1);
    if (count == 1) {
        strcpy(poll.options[0], "yes");
        strcpy(poll.options[1], "no");
        poll.optionCount = 2;
    } else {
        for (int i = 1; i < count; i++)
            strncpy(poll.options[i - 1], fields[i], sizeof(poll.options[0]) - 1);
        poll.optionCount = (uint8_t)(count - 1);
    }

    char text[220];
    size_t pos = 0;
    appendf(text, sizeof(text), pos, "POLL: %s", poll.question);
    for (uint8_t i = 0; i < poll.optionCount; i++)
        appendf(text, sizeof(text), pos, " %u)%s", (unsigned)(i + 1), poll.options[i]);
    appendf(text, sizeof(text), pos, " | reply @!%08x vote N (%ldm)", (unsigned)nodeDB->getNodeNum(), mins);
    if (pos + 1 >= sizeof(text)) {
        poll = Poll{};
        snprintf(out, outLen, "too long");
        return;
    }
    // The announcement is the opener's confirmation; a reply on top would hit the minimum TX gap.
    if (!sendTextTo(NODENUM_BROADCAST, channels.getPrimaryIndex(), text)) {
        snprintf(out, outLen, "Send failed");
        return;
    }
    poll.startedMs = Time::getMillis();
    poll.durationMs = (uint32_t)mins * 60 * 1000;
    poll.open = true;
    setIntervalFromNow(1000);
}

void MeshBotModule::cmdVote(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen)
{
    // Past the deadline is closed, even before runOnce has posted the result.
    if (!poll.open || !Throttle::isWithinTimespanMs(poll.startedMs, poll.durationMs)) {
        snprintf(out, outLen, "no poll");
        return;
    }
    if (!args || args[0] == '\0') {
        formatTally(out, outLen, "");
        return;
    }
    char *end = nullptr;
    const long n = strtol(args, &end, 10);
    while (std::isspace(static_cast<unsigned char>(*end)))
        end++;
    if (n < 1 || n > poll.optionCount || *end != '\0') {
        snprintf(out, outLen, "vote 1-%u", (unsigned)poll.optionCount);
        return;
    }
    Poll::Voter *slot = nullptr;
    for (uint8_t i = 0; i < poll.voterCount && !slot; i++)
        if (poll.voters[i].node == mp.from)
            slot = &poll.voters[i]; // a second vote replaces the first
    if (!slot) {
        if (poll.voterCount == sizeof(poll.voters) / sizeof(poll.voters[0])) {
            snprintf(out, outLen, "poll full");
            return;
        }
        slot = &poll.voters[poll.voterCount++];
        slot->node = mp.from;
    }
    slot->choice = (uint8_t)(n - 1);
    ackReaction = kReactVote;
}

void MeshBotModule::formatTally(char *out, size_t outLen, const char *prefix) const
{
    size_t pos = 0;
    appendf(out, outLen, pos, "%s%s |", prefix, poll.question);
    for (uint8_t o = 0; o < poll.optionCount; o++) {
        unsigned votes = 0;
        for (uint8_t i = 0; i < poll.voterCount; i++)
            votes += poll.voters[i].choice == o;
        appendf(out, outLen, pos, " %s:%u", poll.options[o], votes);
    }
}

bool MeshBotModule::parseDuration(const char *tok, uint32_t &secs)
{
    if (!std::isdigit(static_cast<unsigned char>(tok[0])))
        return false;
    char *end = nullptr;
    const unsigned long n = strtoul(tok, &end, 10);
    const uint32_t unit = *end == 'm' ? 60 : *end == 'h' ? 3600 : *end == 'd' ? 86400 : 0;
    // Capped at the one-shot TTL, which also keeps every deadline inside the millisecond clock.
    if (!unit || end[1] != '\0' || n < 1 || n > 7ul * 86400 / unit)
        return false;
    secs = (uint32_t)n * unit;
    return true;
}

bool MeshBotModule::parseDistance(const char *tok, uint32_t &meters)
{
    if (!std::isdigit(static_cast<unsigned char>(tok[0])))
        return false;
    char *end = nullptr;
    const unsigned long n = strtoul(tok, &end, 10);
    const uint32_t unit = strcmp(end, "m") == 0 ? 1 : strcmp(end, "km") == 0 ? 1000 : 0;
    if (!unit || n < 1 || n > 20000000ul / unit) // half the equator is as far apart as two nodes get
        return false;
    meters = (uint32_t)n * unit;
    return true;
}

uint32_t MeshBotModule::secondsUntil(uint32_t localNowSecs, uint16_t minuteOfDay)
{
    const uint32_t d = (minuteOfDay * 60u + 86400 - localNowSecs % 86400) % 86400;
    return d ? d : 86400;
}

bool MeshBotModule::watches(NodeNum n) const
{
    for (const auto &r : rules)
        if (r.used && n && (r.watched == n || r.other == n))
            return true;
    return false;
}

void MeshBotModule::cmdNotify(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen)
{
    addRule(mp, args, false, out, outLen);
}

void MeshBotModule::cmdEvery(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen)
{
    const char *a = args ? args : "";
    if (strncasecmp(a, "notify", 6) == 0 && (a[6] == '\0' || std::isspace(static_cast<unsigned char>(a[6]))))
        addRule(mp, a + 6, true, out, outLen);
    else
        snprintf(out, outLen, "%s", kNotifyUsage);
}

void MeshBotModule::addRule(const meshtastic_MeshPacket &mp, const char *args, bool repeat, char *out, size_t outLen)
{
    char buf[256];
    strncpy(buf, args ? args : "", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    // The note follows the first ':' not between two digits, so "at 18:00" keeps its colon.
    char *colon = buf;
    while (*colon != '\0' && !(*colon == ':' && !(colon > buf && std::isdigit(static_cast<unsigned char>(colon[-1])) &&
                                                  std::isdigit(static_cast<unsigned char>(colon[1])))))
        colon++;
    const char *note = "";
    if (*colon != '\0') {
        *colon = '\0';
        note = trim(colon + 1);
    }
    char *tok[6];
    const int n = splitWords(buf, tok, 6);
    if (n < 3) {
        snprintf(out, outLen, "%s", kNotifyUsage);
        return;
    }

    Rule r = {};
    r.owner = mp.from;
    r.channel = mp.channel;
    r.repeat = repeat;
    if (strcasecmp(tok[0], "me") == 0) {
        r.to = mp.from;
    } else if (tok[0][0] == '#') {
        long ch;
        if (!parseLong(tok[0] + 1, ch) || ch < 0 || ch >= channels.getNumChannels() ||
            channels.getByIndex((ChannelIndex)ch).role == meshtastic_Channel_Role_DISABLED) {
            snprintf(out, outLen, "Bad channel");
            return;
        }
        r.toChannel = true;
        r.channel = (uint8_t)ch;
    } else if (!requireNodeId(tok[0], r.to, out, outLen)) {
        return;
    }
    auto node = [&](const char *t, uint32_t &num) {
        if (strcasecmp(t, "me") != 0)
            return requireNodeId(t, num, out, outLen);
        num = mp.from;
        return true;
    };

    // "in <dur>", "at <HH:MM>" or "when <node> <cond> [args]", each with an exact word count.
    const char *w = tok[1];
    const int extra = n - 4; // words after "when <node> <cond>"
    uint32_t v = 0;
    long lv = 0;
    bool ok = false;
    if (n == 3 && strcasecmp(w, "in") == 0) {
        r.kind = Rule::In;
        ok = parseDuration(tok[2], v);
        r.threshold = (int32_t)v;
        r.unit = tok[2][strlen(tok[2]) - 1];
    } else if (n == 3 && strcasecmp(w, "at") == 0) {
        unsigned hh = 0, mm = 0;
        char tail;
        const size_t len = strlen(tok[2]);
        r.kind = Rule::At;
        ok = len >= 4 && tok[2][len - 3] == ':' && sscanf(tok[2], "%2u:%2u%c", &hh, &mm, &tail) == 2 && hh < 24 && mm < 60;
        r.threshold = (int32_t)(hh * 60 + mm);
    } else if (n >= 4 && strcasecmp(w, "when") == 0) {
        if (!node(tok[2], r.watched))
            return;
        const char *c = tok[3];
        if (extra == 0 && strcasecmp(c, "heard") == 0) {
            r.kind = Rule::Heard;
            ok = true;
        } else if (extra == 1 && strcasecmp(c, "silent") == 0) {
            r.kind = Rule::Silent;
            ok = parseDuration(tok[4], v);
            r.threshold = (int32_t)v;
            r.unit = tok[4][strlen(tok[4]) - 1];
        } else if (extra == 2 && strcasecmp(c, "battery") == 0 && strcmp(tok[4], "<") == 0) {
            r.kind = Rule::Battery;
            ok = (parseLong(tok[5], lv, "%") || parseLong(tok[5], lv)) && lv >= 1 && lv <= 100;
            r.threshold = (int32_t)lv;
        } else if (extra == 2 && strcasecmp(c, "snr") == 0 && strcmp(tok[4], "<") == 0) {
            r.kind = Rule::Snr;
            ok = parseLong(tok[5], lv) && lv > -100 && lv < 100;
            r.threshold = (int32_t)lv;
        } else if (extra == 2 && strcasecmp(c, "hops") == 0 && strcmp(tok[4], ">") == 0) {
            r.kind = Rule::Hops;
            ok = parseLong(tok[5], lv) && lv >= 0 && lv <= 7;
            r.threshold = (int32_t)lv;
        } else if (extra == 1 && strcasecmp(c, "moved") == 0) {
            r.kind = Rule::Moved;
            ok = parseDistance(tok[4], v);
            r.threshold = (int32_t)v;
        } else if (extra == 2 && strcasecmp(c, "near") == 0) {
            if (!node(tok[4], r.other))
                return;
            r.kind = Rule::Near;
            ok = parseDistance(tok[5], v);
            r.threshold = (int32_t)v;
        }
    }
    if (ok && r.tellForm() && note[0] == '\0') // the tell form is all note
        ok = false;
    if (!ok) {
        snprintf(out, outLen, "%s", kNotifyUsage);
        return;
    }
    // Refused rather than cut: with the note under 160 bytes every alert fits one packet.
    if (strlen(note) >= sizeof(r.note)) {
        snprintf(out, outLen, "too long");
        return;
    }
    if (repeat && (r.kind == Rule::In || r.kind == Rule::Silent) && r.threshold < 15 * 60) {
        snprintf(out, outLen, "min 15m");
        return;
    }
    const uint32_t now = Time::getMillis();
    if (r.kind == Rule::At) {
        const uint32_t local = getValidTime(RTCQualityDevice, true);
        if (!local) {
            snprintf(out, outLen, "no clock");
            return;
        }
        r.dueMs = now + secondsUntil(local, (uint16_t)r.threshold) * 1000;
    } else if (r.kind == Rule::In) {
        r.dueMs = now + (uint32_t)r.threshold * 1000;
    }
    r.lastHeardMs = now;
    if (r.kind == Rule::Silent) {
        // Already quiet for a while counts toward the silence, up to the TTL.
        const meshtastic_NodeInfoLite *info = nodeDB->getMeshNode(r.watched);
        if (info && info->last_heard)
            r.lastHeardMs = now - std::min(sinceLastSeen(info), kRuleTtlMs / 1000) * 1000;
    }
    r.createdMs = now;
    r.lastFireMs = now - kRepeatFloorMs; // as if a floor ago, so the first alert is not held back
    meshtastic_PositionLite pos;
    if (r.kind == Rule::Moved && knownPosition(r.watched, pos)) {
        r.refLat = pos.latitude_i;
        r.refLon = pos.longitude_i;
    }
    r.armed = true;
    r.used = true;
    strncpy(r.note, note, sizeof(r.note) - 1);
    for (auto &slot : rules) {
        if (!slot.used) {
            slot = r;
            setIntervalFromNow(1000); // runOnce then picks the sweep interval
            // Accepted either way; the admin is told when there is nothing to judge it on yet.
            meshtastic_DeviceMetrics m;
            const meshtastic_NodeInfoLite *info = nodeDB->getMeshNode(r.watched);
            if (r.kind == Rule::Battery && !(nodeDB->copyNodeTelemetry(r.watched, m) && m.has_battery_level))
                snprintf(out, outLen, "ok, but no battery data from !%08x yet", (unsigned)r.watched);
            else if ((r.kind == Rule::Snr && !nodeInfoLiteHasSnr(info)) ||
                     (r.kind == Rule::Hops && !(info && info->has_hops_away)))
                snprintf(out, outLen, "ok, but never heard !%08x", (unsigned)r.watched);
            else if ((r.kind == Rule::Moved || r.kind == Rule::Near) && !knownPosition(r.watched, pos))
                snprintf(out, outLen, "ok, but no position from !%08x yet", (unsigned)r.watched);
            else if (r.kind == Rule::Near && !knownPosition(r.other, pos))
                snprintf(out, outLen, "ok, but no position from !%08x yet", (unsigned)r.other);
            else
                ackReaction = kReactQueued;
            return;
        }
    }
    snprintf(out, outLen, "rules full");
}

void MeshBotModule::formatRule(const Rule &r, char *out, size_t outLen) const
{
    char d[12], a[12], b[12];
    size_t pos = 0;
    switch (r.kind) {
    case Rule::Heard:
        appendf(out, outLen, pos, "heard");
        break;
    case Rule::Silent:
        formatDuration((uint32_t)r.threshold, r.unit, d, sizeof(d));
        appendf(out, outLen, pos, "silent%s", d);
        break;
    case Rule::Battery:
        appendf(out, outLen, pos, "bat<%d", (int)r.threshold);
        break;
    case Rule::Snr:
        appendf(out, outLen, pos, "snr<%d", (int)r.threshold);
        break;
    case Rule::Hops:
        appendf(out, outLen, pos, "hops>%d", (int)r.threshold);
        break;
    case Rule::Moved:
        appendf(out, outLen, pos, "moved%dm", (int)r.threshold);
        break;
    case Rule::Near:
        appendf(out, outLen, pos, "near%dm %s,%s", (int)r.threshold, nodeLabel(r.watched, a, sizeof(a)),
                nodeLabel(r.other, b, sizeof(b)));
        break;
    case Rule::In:
        formatDuration((uint32_t)r.threshold, r.unit, d, sizeof(d));
        appendf(out, outLen, pos, "in%s", d);
        break;
    case Rule::At:
        appendf(out, outLen, pos, "at%02d:%02d", (int)(r.threshold / 60), (int)(r.threshold % 60));
        break;
    }
    if (r.kind != Rule::Near && r.kind != Rule::In && r.kind != Rule::At)
        appendf(out, outLen, pos, " %s", nodeLabel(r.watched, a, sizeof(a)));
    if (r.toChannel)
        appendf(out, outLen, pos, ">#%u", (unsigned)r.channel);
    else
        appendf(out, outLen, pos, ">%s", nodeLabel(r.to, a, sizeof(a)));
}

void MeshBotModule::cmdRules(const meshtastic_MeshPacket &, const char *args, char *out, size_t outLen)
{
    constexpr int kSlots = sizeof(rules) / sizeof(rules[0]);
    char buf[64];
    strncpy(buf, args ? args : "", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *tok[2];
    const int n = splitWords(buf, tok, 2);
    long num = 1;
    if (n == 2 && strcasecmp(tok[0], "del") == 0 && parseLong(tok[1], num)) {
        if (num < 1 || num > kSlots || !rules[num - 1].used) {
            snprintf(out, outLen, "no rule %ld", num);
            return;
        }
        rules[num - 1].used = false;
        ackReaction = kReactQueued;
        return;
    }
    if (n > 1 || (n == 1 && (!parseLong(tok[0], num) || num < 1))) {
        snprintf(out, outLen, "rules [N] | rules del N");
        return;
    }
    // Whole entries only; when the next would not fit with room for the " +K: rules M" tail, stop there.
    size_t pos = 0;
    for (int i = (int)std::min(num, (long)kSlots + 1) - 1; i < kSlots; i++) { // a long N can wrap an int
        if (!rules[i].used)
            continue;
        char e[64];
        const int len = snprintf(e, sizeof(e), "%d %s", i + 1, rules[i].repeat ? "every " : "");
        formatRule(rules[i], e + len, sizeof(e) - len);
        int left = 0;
        for (int j = i + 1; j < kSlots; j++)
            left += rules[j].used;
        if (pos + 3 + strlen(e) + (left ? 12 : 0) >= outLen) {
            appendf(out, outLen, pos, " +%d: rules %d", left + 1, i + 1);
            return;
        }
        appendf(out, outLen, pos, "%s%s", pos ? " | " : "", e);
    }
    if (pos == 0)
        snprintf(out, outLen, "no rules");
}

void MeshBotModule::cmdAckPing(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen)
{
    uint32_t num;
    if (!requireNodeId(args, num, out, outLen))
        return;
    if (pending.active) {
        snprintf(out, outLen, "Busy, try again");
        return;
    }
    // want_ack with no want_response: the routing ack is the whole answer, and it is the one the
    // firmware produces for us without the target running any particular module. want_response would
    // add a NO_RESPONSE nak from any target with no REPLY_APP module, which reads as a failed ping
    // and is not one. REPLY_APP rather than TEXT_MESSAGE_APP so nothing renders as a blank message.
    uint32_t id = 0;
    if (!sendRequestTo(mp, num, meshtastic_PortNum_REPLY_APP, true, false, nullptr, 0, &id, out, outLen))
        return;
    armPending(mp, num, id, meshtastic_PortNum_ROUTING_APP, kAckTimeoutMs);
    snprintf(out, outLen, "Pinging !%08x", (unsigned)num);
}

void MeshBotModule::cmdRoll(const meshtastic_MeshPacket &, const char *args, char *out, size_t outLen)
{
    ensureSeeded();
    long sides = 6;
    if (args && args[0] != '\0') {
        long v = strtol(args, nullptr, 10);
        if (v >= 2 && v <= 1000000)
            sides = v;
    }
    snprintf(out, outLen, "Rolled %ld (d%ld)", (long)random(1, sides + 1), sides);
}

void MeshBotModule::cmdFlip(const meshtastic_MeshPacket &, const char *, char *out, size_t outLen)
{
    ensureSeeded();
    snprintf(out, outLen, "%s", random(2) ? "heads" : "tails");
}

void MeshBotModule::cmd8ball(const meshtastic_MeshPacket &, const char *, char *out, size_t outLen)
{
    static const char *const answers[] = {"Yes",       "No",    "Maybe", "Definitely", "Doubtful", "Ask again later",
                                          "Certainly", "No way"};
    ensureSeeded();
    snprintf(out, outLen, "%s", answers[random(sizeof(answers) / sizeof(answers[0]))]);
}

void MeshBotModule::cmdJoke(const meshtastic_MeshPacket &, const char *, char *out, size_t outLen)
{
    static const char *const jokes[] = {
        "Why did the node cross the mesh? To relay the other side.",
        "I'd tell a UDP joke, but you might not get it.",
        "There are 10 kinds of people: those who read binary and those who don't.",
    };
    ensureSeeded();
    snprintf(out, outLen, "%s", jokes[random(sizeof(jokes) / sizeof(jokes[0]))]);
}
#endif // MESHTASTIC_EXCLUDE_MESHBOT
