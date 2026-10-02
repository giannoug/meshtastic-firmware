#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_MESHBOT
// A slash-command bot for the mesh; see MeshBotModule.h. Excluded by default in platformio.ini -
// `#undef MESHTASTIC_EXCLUDE_MESHBOT` in a variant.h and set MESHBOT_ALLOWED_SENDERS to enable it.

#include "Channels.h"
#include "MeshBotModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "mesh/MeshTypes.h"
#include "mesh/Throttle.h"
#include "mesh/mesh-pb-constants.h"
#include "modules/NodeInfoModule.h"
#include "modules/PositionModule.h"
#include "modules/TraceRouteModule.h"

#include <Arduino.h>
#include <cctype>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

// Comma-separated node ids allowed to run the privileged (transmitting/mutating) commands, e.g.
// -D MESHBOT_ALLOWED_SENDERS=0x12345678,0xabcdef01. Empty by default, which denies every one.
#ifndef MESHBOT_ALLOWED_SENDERS
#define MESHBOT_ALLOWED_SENDERS 0
#endif

// Escape hatch: set to 1 to let any node run the privileged commands. Off by default because those
// commands let a caller spend our airtime and aim traffic at a node of their choosing.
#ifndef MESHBOT_ALLOW_ANY_SENDER
#define MESHBOT_ALLOW_ANY_SENDER 0
#endif

// Channel index the bot listens for command broadcasts on. -1 (the default) uses the primary
// channel. DMs are always accepted. Replies always go back on the channel the command arrived on.
#ifndef MESHBOT_COMMAND_CHANNEL
#define MESHBOT_COMMAND_CHANNEL -1
#endif

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

CooldownEntry cooldown[kCooldownSlots];
uint8_t cooldownIdx = 0;

// Configured from MESHBOT_ALLOWED_SENDERS; the trailing 0 terminates the list.
const uint32_t kAllowedSenders[] = {MESHBOT_ALLOWED_SENDERS, 0};

// Configured from MESHBOT_COMMAND_CHANNEL; negative means "the primary channel".
constexpr int kCommandChannel = MESHBOT_COMMAND_CHANNEL;

// How long /trace waits for the TRACEROUTE_APP response before reporting a timeout.
constexpr uint32_t kTraceTimeoutMs = 45000;

bool senderAllowed(uint32_t from)
{
    if (MESHBOT_ALLOW_ANY_SENDER)
        return true;
    for (const uint32_t *p = kAllowedSenders; *p != 0; ++p)
        if (*p == from)
            return true;
    return false;
}

// Parse a node id like "!433a1b2c" or "433a1b2c" (hex, optional leading '!') into a NodeNum.
bool parseNodeId(const char *s, uint32_t &out)
{
    if (!s)
        return false;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '!')
        s++;
    if (*s == '\0')
        return false;
    char *end = nullptr;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s)
        return false;
    out = (uint32_t)v;
    return true;
}

// Seed the PRNG once, lazily, on first use of a random command.
void ensureSeeded()
{
    static bool seeded = false;
    if (!seeded) {
        randomSeed(millis());
        seeded = true;
    }
}

bool rateLimited(uint32_t from, uint32_t cooldownMs)
{
    const uint32_t now = millis();
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

// " > NAME(snr)", omitting the SNR when the hop didn't report one.
void appendHop(char *out, size_t outLen, size_t &pos, uint32_t num, int8_t snrQuarterDb)
{
    char scratch[8];
    const char *label = hopLabel(num, scratch, sizeof(scratch));
    if (snrQuarterDb == INT8_MIN)
        appendf(out, outLen, pos, " > %s", label);
    else
        appendf(out, outLen, pos, " > %s(%.1f)", label, (double)snrQuarterDb / 4.0);
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
} // namespace

MeshBotModule::MeshBotModule() : SinglePortModule("meshbot", meshtastic_PortNum_TEXT_MESSAGE_APP), OSThread("MeshBot")
{
    // Promiscuous so we also see broadcasts on the configured bot channel, not just DMs to us.
    isPromiscuous = true;
}

// The command table; order only affects how /help lists them. The privileged flag marks a command
// that transmits or mutates state, which handleReceived gates on the allowlist and a longer cooldown.
const MeshBotModule::Command MeshBotModule::kCommands[] = {
    {"help", &MeshBotModule::cmdHelp, "list commands", false},
    {"ping", &MeshBotModule::cmdPing, "reply pong", false},
    {"signal", &MeshBotModule::cmdSignal, "link RSSI/SNR/hops", false},
    {"nodes", &MeshBotModule::cmdNodes, "known node count", false},
    {"uptime", &MeshBotModule::cmdUptime, "bot uptime", false},
    {"echo", &MeshBotModule::cmdEcho, "echo <text> back", false},
    {"online", &MeshBotModule::cmdOnline, "online/total nodes", false},
    {"whoami", &MeshBotModule::cmdWhoami, "your + bot id", false},
    {"ver", &MeshBotModule::cmdVer, "firmware version", false},
    {"node", &MeshBotModule::cmdNode, "node info [!id]", false},
    {"pos", &MeshBotModule::cmdPos, "position [!id]", false},
    {"seen", &MeshBotModule::cmdSeen, "last heard [!id]", false},
    {"pbatt", &MeshBotModule::cmdPeerBatt, "peer battery [!id]", false},
    {"putil", &MeshBotModule::cmdPeerUtil, "peer util [!id]", false},
    {"trace", &MeshBotModule::cmdTrace, "traceroute !id", true, "traceroute"},
    {"reqpos", &MeshBotModule::cmdReqPos, "ask pos !id", true},
    {"reqtel", &MeshBotModule::cmdReqTel, "ask telem !id", true},
    {"announce", &MeshBotModule::cmdAnnounce, "broadcast nodeinfo", true},
    {"sharepos", &MeshBotModule::cmdSharePos, "broadcast position", true},
    {"fav", &MeshBotModule::cmdFav, "favorite !id", true},
    {"unfav", &MeshBotModule::cmdUnfav, "unfavorite !id", true},
    {"ackping", &MeshBotModule::cmdAckPing, "reachability !id", true},
    {"roll", &MeshBotModule::cmdRoll, "roll dice [N]", false},
    {"flip", &MeshBotModule::cmdFlip, "coin flip", false},
    {"8ball", &MeshBotModule::cmd8ball, "magic 8-ball", false},
    {"joke", &MeshBotModule::cmdJoke, "tell a joke", false},
};

bool MeshBotModule::wantPacket(const meshtastic_MeshPacket *p)
{
    if (!p)
        return false;
    // Only the response to our own outstanding /trace; anyone else's traceroute traffic isn't ours
    // to touch, and matching requests here would put us in the running to answer them.
    if (pendingTrace.active && p->decoded.portnum == meshtastic_PortNum_TRACEROUTE_APP && p->decoded.request_id != 0 &&
        p->from == pendingTrace.target && p->to == nodeDB->getNodeNum())
        return true;
    return p->decoded.portnum == ourPortNum;
}

ProcessMessage MeshBotModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (mp.decoded.portnum == meshtastic_PortNum_TRACEROUTE_APP) {
        handleTraceRouteReply(mp);
        return ProcessMessage::CONTINUE;
    }

    const uint32_t ourNode = nodeDB->getNodeNum();
    const ChannelIndex botChannel = (kCommandChannel < 0) ? channels.getPrimaryIndex() : (ChannelIndex)kCommandChannel;
    const bool isDM = (mp.to == ourNode);
    const bool isBotChannelBroadcast = isBroadcast(mp.to) && (mp.channel == botChannel);
    if (!isDM && !isBotChannelBroadcast)
        return ProcessMessage::CONTINUE;

    if (mp.decoded.payload.size == 0)
        return ProcessMessage::CONTINUE;

    // Copy the payload into a null-terminated buffer.
    char buf[256];
    size_t n = mp.decoded.payload.size;
    if (n > sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    memcpy(buf, mp.decoded.payload.bytes, n);
    buf[n] = '\0';

    const char *args = nullptr;
    const Command *cmd = parseCommand(buf, &args);
    if (!cmd)
        return ProcessMessage::CONTINUE;

    // Read-only commands answer anyone; privileged (transmitting/mutating) commands are gated
    // behind the sender allowlist so a random node can't spend our airtime or edit our state.
    if (cmd->privileged && !senderAllowed(mp.from))
        return ProcessMessage::CONTINUE;

    const uint32_t cooldownMs = cmd->privileged ? kPrivilegedCooldownMs : (isDM ? kDmCooldownMs : kBroadcastCooldownMs);
    if (rateLimited(mp.from, cooldownMs))
        return ProcessMessage::CONTINUE;

    char reply[200];
    reply[0] = '\0';
    (this->*(cmd->handler))(mp, args, reply, sizeof(reply));
    if (reply[0] != '\0')
        sendReply(mp, reply);

    return ProcessMessage::CONTINUE;
}

const MeshBotModule::Command *MeshBotModule::parseCommand(const char *msg, const char **argsOut) const
{
    if (!msg)
        return nullptr;
    while (*msg == ' ' || *msg == '\t')
        msg++;
    if (*msg != '/')
        return nullptr;
    msg++; // skip '/'

    // The command word ends at the first whitespace or end-of-string.
    const char *wordEnd = msg;
    while (*wordEnd != '\0' && !std::isspace(static_cast<unsigned char>(*wordEnd)))
        wordEnd++;
    const size_t wordLen = wordEnd - msg;
    if (wordLen == 0)
        return nullptr;

    for (const auto &c : kCommands) {
        const bool match = (strlen(c.name) == wordLen && strncmp(msg, c.name, wordLen) == 0) ||
                           (c.alias && strlen(c.alias) == wordLen && strncmp(msg, c.alias, wordLen) == 0);
        if (match) {
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
    sendTextTo(isBroadcast(rx.to) ? NODENUM_BROADCAST : rx.from, rx.channel, text);
}

void MeshBotModule::sendTextTo(uint32_t dest, uint8_t channel, const char *text)
{
    if (!text || text[0] == '\0')
        return;
    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return;
    p->to = dest;
    p->channel = channel;
    p->want_ack = false;
    p->decoded.want_response = false;
    size_t len = strlen(text);
    if (len > sizeof(p->decoded.payload.bytes))
        len = sizeof(p->decoded.payload.bytes);
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, text, len);
    service->sendToMesh(p);
}

void MeshBotModule::cmdHelp(const meshtastic_MeshPacket &, const char *args, char *out, size_t outLen)
{
    // "/help" lists public commands; "/help all" lists the privileged action commands. Splitting
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
        written = snprintf(out + pos, outLen - pos, " /%s", c.name);
        if (written <= 0)
            break;
        pos += (size_t)written;
    }
    if (!wantActions && pos < outLen)
        snprintf(out + pos, outLen - pos, " +/help all");
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

void MeshBotModule::cmdNodes(const meshtastic_MeshPacket &, const char *, char *out, size_t outLen)
{
    snprintf(out, outLen, "Known nodes: %u", (unsigned)nodeDB->getNumMeshNodes());
}

void MeshBotModule::cmdUptime(const meshtastic_MeshPacket &, const char *, char *out, size_t outLen)
{
    uint32_t secs = millis() / 1000;
    uint32_t days = secs / 86400;
    uint32_t hours = (secs % 86400) / 3600;
    uint32_t mins = (secs % 3600) / 60;
    snprintf(out, outLen, "Uptime: %ud %uh %um", (unsigned)days, (unsigned)hours, (unsigned)mins);
}

void MeshBotModule::cmdEcho(const meshtastic_MeshPacket &, const char *args, char *out, size_t outLen)
{
    if (args && args[0] != '\0')
        snprintf(out, outLen, "%s", args);
    else
        snprintf(out, outLen, "echo what?");
}

void MeshBotModule::cmdOnline(const meshtastic_MeshPacket &, const char *, char *out, size_t outLen)
{
    snprintf(out, outLen, "Nodes %u/%u online", (unsigned)nodeDB->getNumOnlineMeshNodes(), (unsigned)nodeDB->getNumMeshNodes());
}

void MeshBotModule::cmdWhoami(const meshtastic_MeshPacket &mp, const char *, char *out, size_t outLen)
{
    snprintf(out, outLen, "You !%08x | Me %s !%08x", (unsigned)mp.from, owner.long_name, (unsigned)nodeDB->getNodeNum());
}

void MeshBotModule::cmdVer(const meshtastic_MeshPacket &, const char *, char *out, size_t outLen)
{
    snprintf(out, outLen, "FW %s", optstr(APP_VERSION));
}

void MeshBotModule::cmdNode(const meshtastic_MeshPacket &, const char *args, char *out, size_t outLen)
{
    uint32_t num = nodeDB->getNodeNum();
    if (args && args[0] != '\0' && !parseNodeId(args, num)) {
        snprintf(out, outLen, "Bad node id");
        return;
    }
    meshtastic_NodeInfoLite *n = nodeDB->getMeshNode(num);
    if (!n) {
        snprintf(out, outLen, "Unknown node !%08x", (unsigned)num);
        return;
    }
    size_t pos = 0;
    if (nodeInfoLiteHasUser(n) && n->long_name[0])
        pos += snprintf(out + pos, outLen - pos, "%s", n->long_name);
    else
        pos += snprintf(out + pos, outLen - pos, "!%08x", (unsigned)num);
    if (pos < outLen && n->has_hops_away)
        pos += snprintf(out + pos, outLen - pos, " | %uhops", (unsigned)n->hops_away);
    if (pos < outLen)
        pos += snprintf(out + pos, outLen - pos, " | SNR %.1f", (double)n->snr);
    meshtastic_DeviceMetrics metrics = meshtastic_DeviceMetrics_init_default;
    if (pos < outLen && nodeDB->copyNodeTelemetry(num, metrics) && metrics.has_battery_level) {
        const uint32_t pct = metrics.battery_level;
        if (pct > 100) // the 101 sentinel means externally powered, not a charge level
            snprintf(out + pos, outLen - pos, " | USB");
        else
            snprintf(out + pos, outLen - pos, " | %u%%", (unsigned)pct);
    }
}

void MeshBotModule::cmdPos(const meshtastic_MeshPacket &, const char *args, char *out, size_t outLen)
{
    uint32_t num = nodeDB->getNodeNum();
    if (args && args[0] != '\0' && !parseNodeId(args, num)) {
        snprintf(out, outLen, "Bad node id");
        return;
    }
    meshtastic_PositionLite p = meshtastic_PositionLite_init_default;
    if (!nodeDB->copyNodePosition(num, p)) {
        snprintf(out, outLen, "No position");
        return;
    }
    snprintf(out, outLen, "%.5f, %.5f, %dm", p.latitude_i * 1e-7, p.longitude_i * 1e-7, (int)p.altitude);
}

void MeshBotModule::cmdSeen(const meshtastic_MeshPacket &, const char *args, char *out, size_t outLen)
{
    uint32_t num = nodeDB->getNodeNum();
    if (args && args[0] != '\0' && !parseNodeId(args, num)) {
        snprintf(out, outLen, "Bad node id");
        return;
    }
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

void MeshBotModule::cmdPeerBatt(const meshtastic_MeshPacket &, const char *args, char *out, size_t outLen)
{
    uint32_t num = nodeDB->getNodeNum();
    if (args && args[0] != '\0' && !parseNodeId(args, num)) {
        snprintf(out, outLen, "Bad node id");
        return;
    }
    meshtastic_DeviceMetrics m = meshtastic_DeviceMetrics_init_default;
    if (!nodeDB->copyNodeTelemetry(num, m)) {
        snprintf(out, outLen, "No metrics");
        return;
    }
    if (!m.has_battery_level && !m.has_voltage) {
        snprintf(out, outLen, "No battery data");
        return;
    }
    size_t pos = 0;
    if (m.has_battery_level)
        pos += snprintf(out + pos, outLen - pos, "Batt %u%%", (unsigned)m.battery_level);
    if (pos < outLen && m.has_voltage)
        snprintf(out + pos, outLen - pos, "%s%.2fV", pos ? " " : "", (double)m.voltage);
}

void MeshBotModule::cmdPeerUtil(const meshtastic_MeshPacket &, const char *args, char *out, size_t outLen)
{
    uint32_t num = nodeDB->getNodeNum();
    if (args && args[0] != '\0' && !parseNodeId(args, num)) {
        snprintf(out, outLen, "Bad node id");
        return;
    }
    meshtastic_DeviceMetrics m = meshtastic_DeviceMetrics_init_default;
    if (!nodeDB->copyNodeTelemetry(num, m)) {
        snprintf(out, outLen, "No metrics");
        return;
    }
    if (!m.has_channel_utilization && !m.has_air_util_tx) {
        snprintf(out, outLen, "No util data");
        return;
    }
    size_t pos = 0;
    if (m.has_channel_utilization)
        pos += snprintf(out + pos, outLen - pos, "Ch %.1f%%", (double)m.channel_utilization);
    if (pos < outLen && m.has_air_util_tx)
        snprintf(out + pos, outLen - pos, "%sTxAir %.1f%%", pos ? " | " : "", (double)m.air_util_tx);
}

// Parse a required "!id" argument shared by the action commands. Writes an error into out and
// returns false when the argument is missing or malformed.
static bool requireNodeId(const char *args, uint32_t &num, char *out, size_t outLen)
{
    if (!args || args[0] == '\0') {
        snprintf(out, outLen, "Need a node id");
        return false;
    }
    if (!parseNodeId(args, num)) {
        snprintf(out, outLen, "Bad node id");
        return false;
    }
    return true;
}

// Build and send a bare request packet (empty payload) on a given portnum toward one node.
// Returns false (and fills out) if the radio isn't up yet.
bool MeshBotModule::sendRequestTo(const meshtastic_MeshPacket &rx, uint32_t dest, meshtastic_PortNum portnum, bool wantAck,
                                  char *out, size_t outLen)
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
    p->decoded.want_response = true;
    p->decoded.payload.size = 0;
    service->sendToMesh(p);
    return true;
}

void MeshBotModule::cmdTrace(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen)
{
    uint32_t num;
    if (!requireNodeId(args, num, out, outLen))
        return;
    if (!traceRouteModule) {
        snprintf(out, outLen, "Trace unavailable");
        return;
    }
    if (!traceRouteModule->startTraceRoute(num)) {
        snprintf(out, outLen, "Trace failed");
        return;
    }

    pendingTrace.requester = mp.from;
    pendingTrace.target = num;
    pendingTrace.startedMs = millis();
    pendingTrace.channel = mp.channel;
    pendingTrace.broadcast = isBroadcast(mp.to);
    pendingTrace.active = true;
    setIntervalFromNow(1000);
    snprintf(out, outLen, "Tracing !%08x", (unsigned)num);
}

// We decode RouteDiscovery out of the raw packet like the phone app does, rather than reaching into
// TraceRouteModule - which, constructed first, has already padded unknown hops and appended our SNR.
void MeshBotModule::handleTraceRouteReply(const meshtastic_MeshPacket &mp)
{
    if (!pendingTrace.active || mp.decoded.request_id == 0 || mp.from != pendingTrace.target || mp.to != nodeDB->getNodeNum())
        return;
    if (!Throttle::isWithinTimespanMs(pendingTrace.startedMs, kTraceTimeoutMs)) {
        pendingTrace.active = false;
        return;
    }

    meshtastic_RouteDiscovery route = meshtastic_RouteDiscovery_init_zero;
    if (!pb_decode_from_bytes(mp.decoded.payload.bytes, mp.decoded.payload.size, &meshtastic_RouteDiscovery_msg, &route))
        return;

    const uint32_t ourNode = nodeDB->getNodeNum();
    const uint32_t target = pendingTrace.target;
    char scratch[8];
    char reply[200];
    size_t pos = 0;

    // Each leg is relays plus one endpoint, and the SNR list carries one extra entry for it:
    // snr_towards[route_count] is what the target heard, snr_back[route_back_count] what we heard.
    appendf(reply, sizeof(reply), pos, "%s", hopLabel(ourNode, scratch, sizeof(scratch)));
    for (pb_size_t i = 0; i < route.route_count; i++)
        appendHop(reply, sizeof(reply), pos, route.route[i], i < route.snr_towards_count ? route.snr_towards[i] : INT8_MIN);
    appendHop(reply, sizeof(reply), pos, target,
              route.snr_towards_count > route.route_count ? route.snr_towards[route.route_count] : INT8_MIN);

    appendf(reply, sizeof(reply), pos, "\n%s", hopLabel(target, scratch, sizeof(scratch)));
    for (pb_size_t i = 0; i < route.route_back_count; i++)
        appendHop(reply, sizeof(reply), pos, route.route_back[i], i < route.snr_back_count ? route.snr_back[i] : INT8_MIN);
    appendHop(reply, sizeof(reply), pos, ourNode,
              route.snr_back_count > route.route_back_count ? route.snr_back[route.route_back_count] : INT8_MIN);

    pendingTrace.active = false;
    sendTextTo(pendingTrace.broadcast ? NODENUM_BROADCAST : pendingTrace.requester, pendingTrace.channel, reply);
}

int32_t MeshBotModule::runOnce()
{
    if (!pendingTrace.active)
        return INT32_MAX;
    if (Throttle::isWithinTimespanMs(pendingTrace.startedMs, kTraceTimeoutMs))
        return 1000;

    char reply[48];
    snprintf(reply, sizeof(reply), "Trace !%08x: no response", (unsigned)pendingTrace.target);
    pendingTrace.active = false;
    sendTextTo(pendingTrace.broadcast ? NODENUM_BROADCAST : pendingTrace.requester, pendingTrace.channel, reply);
    return INT32_MAX;
}

void MeshBotModule::cmdReqPos(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen)
{
    uint32_t num;
    if (!requireNodeId(args, num, out, outLen))
        return;
    if (sendRequestTo(mp, num, meshtastic_PortNum_POSITION_APP, false, out, outLen))
        snprintf(out, outLen, "Requested pos !%08x", (unsigned)num);
}

void MeshBotModule::cmdReqTel(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen)
{
    uint32_t num;
    if (!requireNodeId(args, num, out, outLen))
        return;
    if (sendRequestTo(mp, num, meshtastic_PortNum_TELEMETRY_APP, false, out, outLen))
        snprintf(out, outLen, "Requested telem !%08x", (unsigned)num);
}

void MeshBotModule::cmdAnnounce(const meshtastic_MeshPacket &, const char *, char *out, size_t outLen)
{
    if (!nodeInfoModule) {
        snprintf(out, outLen, "NodeInfo unavailable");
        return;
    }
    nodeInfoModule->sendOurNodeInfo(NODENUM_BROADCAST, false, 0);
    snprintf(out, outLen, "NodeInfo announced");
}

void MeshBotModule::cmdSharePos(const meshtastic_MeshPacket &, const char *, char *out, size_t outLen)
{
    if (!positionModule) {
        snprintf(out, outLen, "Position unavailable");
        return;
    }
    positionModule->sendOurPosition();
    snprintf(out, outLen, "Position shared");
}

void MeshBotModule::cmdFav(const meshtastic_MeshPacket &, const char *args, char *out, size_t outLen)
{
    uint32_t num;
    if (!requireNodeId(args, num, out, outLen))
        return;
    if (!nodeDB->getMeshNode(num)) {
        snprintf(out, outLen, "Unknown node !%08x", (unsigned)num);
        return;
    }
    nodeDB->set_favorite(true, num);
    snprintf(out, outLen, "Favorited !%08x", (unsigned)num);
}

void MeshBotModule::cmdUnfav(const meshtastic_MeshPacket &, const char *args, char *out, size_t outLen)
{
    uint32_t num;
    if (!requireNodeId(args, num, out, outLen))
        return;
    if (!nodeDB->getMeshNode(num)) {
        snprintf(out, outLen, "Unknown node !%08x", (unsigned)num);
        return;
    }
    nodeDB->set_favorite(false, num);
    snprintf(out, outLen, "Unfavorited !%08x", (unsigned)num);
}

void MeshBotModule::cmdAckPing(const meshtastic_MeshPacket &mp, const char *args, char *out, size_t outLen)
{
    uint32_t num;
    if (!requireNodeId(args, num, out, outLen))
        return;
    // REPLY_APP, not TEXT_MESSAGE_APP: a bare text packet renders as a blank message in chat clients.
    if (sendRequestTo(mp, num, meshtastic_PortNum_REPLY_APP, true, out, outLen))
        snprintf(out, outLen, "Ping sent !%08x", (unsigned)num);
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
