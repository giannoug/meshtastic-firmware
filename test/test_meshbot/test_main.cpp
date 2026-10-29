// Unit tests for MeshBotModule in src/modules/MeshBotModule.cpp - the mention-addressed text bot's
// authorization gates, its argument parsing, and its per-command replies.
//
// The module answers "name [args]" text messages arriving as DMs, and "@!<bot id> name [args]"
// broadcasts on any channel. kCommands is the whole dispatch surface: each entry pairs a name with a handler and a
// privileged flag, and handleReceived reads that flag to pick a cooldown and to decide whether the
// packet has to clear isPrivilegedSenderAuthorized() first.
//
// The `privileged` flag is a routing label, not an authentication boundary: on its own it only says
// which gate a command goes through. The gate itself is isPrivilegedSenderAuthorized(), and it is
// the security boundary this suite exists to defend. It mirrors AdminModule's remote-admin check -
// the packet must have been PKI-decrypted and carry a 32-byte public key listed in
// config.security.admin_key - because the NodeNum in mp.from proves nothing: any node sharing the
// channel can put someone else's id there. The regression guarded is a return to NodeNum-based
// authorization (the removed MESHBOT_ALLOWED_SENDERS list), under which a stranger could spend our
// airtime, aim traffic at a node of their choosing, and edit our node database by typing one text
// message. test_handleReceived_privilegedRequiresAdminKey therefore drives the real
// handleReceived() and asserts on transmits and on the node database, not on a reply buffer: a gate
// that merely declines to compose a reply is not a gate. test_actionCommands still asserts
// privileged == true per transmitting command (notify, every and rules included: a stored rule
// transmits later), so an action command cannot be added outside the gate; note that it and test_dumpAllCommands spell their
// command lists out rather than deriving them from kCommands, so a new command is uncovered until it is added here too.
//
// The second gate is on what a lookup may be aimed at, and it reuses that same admin key rather
// than adding a permission system beside it. seen reads NodeDB, and the lookup commands it once
// shared this gate with used to read any record in it for anyone: a stranger on the channel could ask the bot
// for a third party's position, battery level, channel utilization and last-heard time, learned on
// channels the asker never shared and relayed to them in plaintext. resolveLookupTarget() is now
// the single place that decides, so test_resolveLookupTarget_policy is the policy statement and
// test_handleReceived_bareLookupIsTheSender / _peerLookupNeedsAdminKey / _adminPeerLookupIsAllowed
// drive the real path. Bare means the caller (mp.from, the only identity the packet
// carries) and nothing else - the regression guarded there is the old default of the bot's own
// NodeNum, which answered about the bot while the help text called the argument optional. An
// explicit id is refused ahead of parseNodeId and ahead of every NodeDB read, which is what makes
// the refusal silent about the node named: well-formed, malformed, known and never-heard ids all
// get one identical sentence, so the reply is not an existence oracle.
//
// What a bare lookup cannot do is authenticate the caller: outside a PKI DM, mp.from is a claim
// any node holding the channel PSK can make, so an impersonator can still aim a bare lookup at a
// third party by putting that node's number in `from`. The gate bounds the damage rather than
// removing it - the answer is addressed to the node named, on that node's channel, not to whoever
// asked - and closing it entirely would mean requiring PKI for every lookup, which would take
// seen away from ordinary users on a broadcast channel. Worth revisiting if MeshBot ever
// ships as a production target.
//
// Broadcast eligibility is the other rule. A broadcast is a command only when it opens with the
// bot's own "@!<id>" mention, the plain-text token the apps put on the wire for a mention
// (meshtastic/design#21); test_parse_mentionAddressing pins the grammar, including that a mention
// mid-sentence or of another node is chat. With the mention saying who a message is for, the node's
// own channel list decides where the bot listens, and test_handleReceived_mentionOnAnyChannel pins
// that a mention on a secondary channel is answered there. A DM is accepted on whatever channel
// byte it carries, because PKI DMs all arrive on channel 0 and the privileged commands can arrive
// no other way.
// test_handleReceived_broadcastCannotReachPrivilege closes the loop: a broadcast is never
// PKI-encrypted, so it can reach no privileged command and no peer lookup.
//
// parseNodeId is the other quiet failure: it used strtoul, which accepted "-1", "0x433a1b2c" and
// "deadbeefzzz" and silently resolved them to a node nobody named. test_parseNodeId_table pins the
// exact accepted grammar. requireNodeId adds the target rules every action command shares, so
// test_requireNodeId_rejectsUnsafeTargets covers the amplification cases - broadcast fans one text
// message out into a mesh-wide request storm plus a NAK per node.
//
// The rest pins behavior that would fail quietly rather than loudly. A handler that leaves its
// reply buffer empty is dropped by handleReceived and the command simply appears to do nothing, so
// every case requires a non-empty reply. The transmitting handlers run with their subsystems absent
// - service, router and the module globals are null in those cases, as they are on a real device
// before the radio comes up - and have to say so rather than crash. signal must report RSSI and
// SNR only for a 0-hop request: our radio measured the final leg, so quoting those for a relayed
// request describes a link the requester is not on, which is why the relayed case names the relay.
//
// ackping waits for the ROUTING_APP ack of its own packet and relays what comes back to whoever
// asked. What makes an ack theirs is decoded.request_id, which carries the id of the request packet
// we sent. Correlating on the peer's NodeNum instead also accepted acks of somebody else's packet
// and anything from a node that cared to forge `from`, so test_cmdAckPing_reportsWhatTheAckProves
// feeds an ack that is right in every respect but the id and requires it to be ignored. The report
// then has to distinguish the three things a routing ack can mean: one from the target is
// delivery, one from ourselves is our own router acking a rebroadcast it overheard and proves only
// the first leg, and a nak (MAX_RETRANSMIT, which our router raises when the retries run out) is a
// failure. A command that transmits must not report having done so when nothing left the node,
// which is what test_sendFailureIsNotReportedAsSuccess pins.
//
// test_txBackstop_boundsTheBotNotJustTheSender covers the limit the per-sender cooldowns cannot be:
// they bound one node, and eight nodes clear them independently. Both limbs matter - a floor on the
// gap between the bot's own transmits, and the airtime figures every periodic module consults, taken
// at the polite threshold because a bot answers only because it was asked. The backstop also runs
// ahead of the per-sender window, which stamps a sender's turn as a side effect, so a command it
// drops does not also cost that sender their next one.
//
// MeshBot reads Time::getMillis() rather than millis(), so setUp() drives the virtual clock. It only
// moves forward: the global transmit gap is module state that outlives one case.
//
// The suite constructs a NodeDB subclass, whose constructor persists a default prefs set when the
// prefs directory is empty - hence this suite's entry in test/state-manifest.tsv.
#include "Channels.h"
#include "NodeDB.h"
#include "TestUtil.h"
#include "UptimeClock.h"
#include "airtime.h"
#include "gps/RTC.h"
#include "mesh/MeshService.h"
#include "mesh/RadioInterface.h"
#include "mesh/Router.h"
#include "mesh/generated/meshtastic/telemetry.pb.h"
#include "mesh/mesh-pb-constants.h"
#include "modules/MeshBotModule.h"
#include "support/MockMeshService.h"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <unity.h>
#include <vector>

#if !MESHTASTIC_EXCLUDE_MESHBOT

// Subclass to reach the module's protected parse/dispatch internals for testing.
class TestableBot : public MeshBotModule
{
  public:
    using MeshBotModule::clockDayNoonUtc;
    using MeshBotModule::cmd8ball;
    using MeshBotModule::cmdAckPing;
    using MeshBotModule::cmdEcho;
    using MeshBotModule::cmdEvery;
    using MeshBotModule::cmdFlip;
    using MeshBotModule::cmdHelp;
    using MeshBotModule::cmdJoke;
    using MeshBotModule::cmdMoon;
    using MeshBotModule::cmdNotify;
    using MeshBotModule::cmdPing;
    using MeshBotModule::cmdRoll;
    using MeshBotModule::cmdRules;
    using MeshBotModule::cmdSeen;
    using MeshBotModule::cmdSignal;
    using MeshBotModule::cmdStats;
    using MeshBotModule::cmdSun;
    using MeshBotModule::cmdVer;
    using MeshBotModule::cmdWhoami;
    using MeshBotModule::Command;
    using MeshBotModule::handleReceived;
    using MeshBotModule::isPrivilegedSenderAuthorized;
    using MeshBotModule::kCommands;
    using MeshBotModule::moonAgeDays;
    using MeshBotModule::parseCommand;
    using MeshBotModule::parseDistance;
    using MeshBotModule::parseDuration;
    using MeshBotModule::parseNodeId;
    using MeshBotModule::requireNodeId;
    using MeshBotModule::resolveLookupTarget;
    using MeshBotModule::runOnce;
    using MeshBotModule::secondsUntil;
    using MeshBotModule::sunRiseSet;
    using MeshBotModule::txBackstopAllows;
    using MeshBotModule::wantPacket;
};

// Minimal NodeDB stand-in so node-aware commands (seen, whoami) can be
// exercised with deterministic data. getMeshNode is virtual; the count helpers read public members.
class MockNodeDB : public NodeDB
{
  public:
    meshtastic_NodeInfoLite node = meshtastic_NodeInfoLite_init_zero;
    std::vector<meshtastic_NodeInfoLite> nodes;
    // Populate this and getMeshNode answers per NodeNum, so "it looked up the wrong node" is
    // visible in the reply rather than hidden behind one record that answers for every id. Left
    // empty, every id resolves to `node`, which is what the older cases here expect.
    std::map<NodeNum, meshtastic_NodeInfoLite> byNum;
    meshtastic_NodeInfoLite *getMeshNode(NodeNum n) override
    {
        if (byNum.empty())
            return &node;
        auto it = byNum.find(n);
        return it == byNum.end() ? nullptr : &it->second;
    }
};

// Records what actually left the node, so a denied command can be checked against "nothing was
// transmitted" rather than against an empty reply buffer.
class RecordingRouter : public Router
{
  public:
    ~RecordingRouter()
    {
        delete cryptLock;
        cryptLock = nullptr;
    }

    ErrorCode send(meshtastic_MeshPacket *p) override
    {
        sent.push_back(*p);
        packetPool.release(p);
        return ERRNO_OK;
    }

    std::vector<meshtastic_MeshPacket> sent;
};

// A router whose transmit always fails, for the paths that must not report a request they did not
// send. Not simply a RecordingRouter with no interface: that route NAKs through routingModule,
// which this harness does not stand up.
class FailingRouter : public Router
{
  public:
    ~FailingRouter()
    {
        delete cryptLock;
        cryptLock = nullptr;
    }

    ErrorCode send(meshtastic_MeshPacket *p) override
    {
        attempts++;
        packetPool.release(p);
        return ERRNO_UNKNOWN;
    }

    uint32_t attempts = 0;
};

class NullRadioInterface : public RadioInterface
{
  public:
    ErrorCode send(meshtastic_MeshPacket *p) override
    {
        packetPool.release(p);
        return ERRNO_OK;
    }
    uint32_t getPacketTime(uint32_t, bool = false) override { return 0; }
};

static TestableBot *bot;
static meshtastic_MeshPacket pkt;

void setUp(void)
{
    // MeshBot reads Time::getMillis() for its cooldowns and for the global gap between its own
    // transmits, so drive the virtual clock rather than real time. It only ever moves forward: the
    // global gap is module state that outlives one case, and rewinding would let a stamp from an
    // earlier case sit in this one's future.
    Time::advanceTestMillis(60u * 1000u);
    bot = new TestableBot();
    memset(&pkt, 0, sizeof(pkt));
}

void tearDown(void)
{
    delete bot;
    airTime = nullptr;
}

static void test_cmdPing()
{
    char out[32] = {0};
    bot->cmdPing(pkt, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("pong", out);
}

static void test_cmdEcho()
{
    char out[64] = {0};
    bot->cmdEcho(pkt, "hi there", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("hi there", out);

    bot->cmdEcho(pkt, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("echo what?", out);
}

static void test_cmdHelp_listsCommands()
{
    // Match the real reply buffer size so we see the full, untruncated listing.
    char out[200] = {0};
    bot->cmdHelp(pkt, "", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, " ping"));
    TEST_ASSERT_NOT_NULL(strstr(out, " echo"));
    // Public help omits privileged action commands and points at "help all".
    TEST_ASSERT_NULL(strstr(out, " ackping"));
    TEST_ASSERT_NOT_NULL(strstr(out, "+help all"));
    // The lookup commands take an optional argument with two different meanings, so help has to
    // say which is which - the listing alone reads as if anyone may name any node.
    TEST_ASSERT_NOT_NULL(strstr(out, "bare=you, !id=admin"));
    TEST_ASSERT_TRUE(strlen(out) < sizeof(out) - 1); // still a single untruncated text packet
}

static void test_cmdHelp_all_listsActions()
{
    char out[200] = {0};
    bot->cmdHelp(pkt, "all", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, " ackping"));
    // The action listing excludes the public commands.
    TEST_ASSERT_NULL(strstr(out, " ping"));
}

static void test_cmdVer_prefix()
{
    char out[48] = {0};
    bot->cmdVer(pkt, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_LEN("FW ", out, 3);
}

static void test_cmdFlip()
{
    char out[16] = {0};
    bot->cmdFlip(pkt, "", out, sizeof(out));
    TEST_ASSERT_TRUE(strcmp(out, "heads") == 0 || strcmp(out, "tails") == 0);
}

static void test_cmdRoll_default()
{
    char out[32] = {0};
    bot->cmdRoll(pkt, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_LEN("Rolled ", out, 7);
    TEST_ASSERT_NOT_NULL(strstr(out, "(d6)"));
    int roll = atoi(out + 7);
    TEST_ASSERT_TRUE(roll >= 1 && roll <= 6);
}

static void test_cmdRoll_customSides()
{
    char out[32] = {0};
    bot->cmdRoll(pkt, "20", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "(d20)"));
    int roll = atoi(out + 7);
    TEST_ASSERT_TRUE(roll >= 1 && roll <= 20);
}

static void test_cmd8ball_nonEmpty()
{
    char out[32] = {0};
    bot->cmd8ball(pkt, "", out, sizeof(out));
    TEST_ASSERT_TRUE(strlen(out) > 0);
}

static void test_cmdJoke_nonEmpty()
{
    char out[96] = {0};
    bot->cmdJoke(pkt, "", out, sizeof(out));
    TEST_ASSERT_TRUE(strlen(out) > 0);
}

// signal reports RSSI/SNR only for a directly-received packet; a relayed one names the relay,
// because our radio measured the last leg rather than the sender.
static void test_cmdSignal_directReportsRssi()
{
    auto *mock = new MockNodeDB();
    mock->meshNodes = &mock->nodes;
    mock->numMeshNodes = 0;
    nodeDB = mock;

    memset(&pkt, 0, sizeof(pkt));
    pkt.rx_rssi = -73;
    pkt.rx_snr = 6.25f;
    pkt.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    pkt.decoded.has_bitfield = true; // proves the sender populates hop_start, so 0 means direct

    char out[64] = {0};
    bot->cmdSignal(pkt, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Direct | RSSI -73 | SNR 6.2", out);

    nodeDB = nullptr;
    delete mock;
}

static void test_cmdSignal_relayedNamesRelay()
{
    auto *mock = new MockNodeDB();
    mock->node.num = 0x433a1b2c;
    nodeInfoLiteSetBit(&mock->node, NODEINFO_BITFIELD_HAS_USER_MASK, true);
    strncpy(mock->node.short_name, "ALFA", sizeof(mock->node.short_name) - 1);
    mock->nodes.push_back(mock->node);
    mock->meshNodes = &mock->nodes;
    mock->numMeshNodes = (pb_size_t)mock->nodes.size();
    nodeDB = mock;

    memset(&pkt, 0, sizeof(pkt));
    pkt.rx_rssi = -73;
    pkt.rx_snr = 6.25f;
    pkt.hop_start = 3;
    pkt.hop_limit = 1;
    pkt.relay_node = 0x2c; // low byte of 0x433a1b2c

    char out[64] = {0};
    bot->cmdSignal(pkt, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2 hops | via ALFA !433a1b2c", out);

    // An unmatched byte falls back to the raw relay id rather than guessing.
    pkt.relay_node = 0x99;
    memset(out, 0, sizeof(out));
    bot->cmdSignal(pkt, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2 hops | via relay 0x99", out);

    // Pre-2.3.0 senders leave relay_node unset.
    pkt.relay_node = NO_RELAY_NODE;
    memset(out, 0, sizeof(out));
    bot->cmdSignal(pkt, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("2 hops | relay unknown", out);

    nodeDB = nullptr;
    delete mock;
}

static void test_cmdSignal_unknownHopsIsNotNegative()
{
    auto *mock = new MockNodeDB();
    mock->meshNodes = &mock->nodes;
    mock->numMeshNodes = 0;
    nodeDB = mock;

    memset(&pkt, 0, sizeof(pkt));
    pkt.hop_start = 1;
    pkt.hop_limit = 3; // malformed: TTL can only decrease

    char out[64] = {0};
    bot->cmdSignal(pkt, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_LEN("? hops", out, 6);

    nodeDB = nullptr;
    delete mock;
}

// Drives every command in the dispatch table against a rich, deterministic environment and prints
// the reply each one produces, so the whole bot surface is visible in one place.
static void test_dumpAllCommands()
{
    auto *mock = new MockNodeDB();
    mock->node.num = 0x433a1b2c;
    nodeInfoLiteSetBit(&mock->node, NODEINFO_BITFIELD_HAS_USER_MASK, true);
    strncpy(mock->node.long_name, "AlphaNode", sizeof(mock->node.long_name) - 1);
    strncpy(mock->node.short_name, "ALFA", sizeof(mock->node.short_name) - 1);
    mock->node.has_hops_away = true;
    mock->node.hops_away = 2;
    mock->node.snr = 6.5f;

    mock->node.last_heard = getTime() - 125; // ~2 minutes ago for seen
    for (int i = 0; i < 3; i++) {
        meshtastic_NodeInfoLite n = mock->node;
        n.last_heard = getTime();
        mock->nodes.push_back(n);
    }
    mock->meshNodes = &mock->nodes;
    mock->numMeshNodes = (pb_size_t)mock->nodes.size();
    nodeDB = mock;

    // Set this after constructing MockNodeDB: its constructor runs loadFromDisk() and would
    // otherwise clobber our demo identity values.
    strncpy(owner.long_name, "MeshBot", sizeof(owner.long_name) - 1);
    myNodeInfo.my_node_num = 0x11223344;

    // Received packet metadata so signal and whoami have something to report.
    memset(&pkt, 0, sizeof(pkt));
    pkt.from = 0x433a1b2c;
    pkt.rx_rssi = -73;
    pkt.rx_snr = 6.25f;
    pkt.hop_start = 3;
    pkt.hop_limit = 1;
    pkt.relay_node = 0x2c;

    printf("\n===== MeshBot command outputs =====\n");
    static const char *const names[] = {"help", "ping", "signal", "echo", "whoami", "ver",   "seen", "stats",
                                        "sun",  "moon", "vote",   "roll", "flip",   "8ball", "joke"};
    for (const char *name : names) {
        char probe[32];
        snprintf(probe, sizeof(probe), "%s", name);
        const char *args = nullptr;
        const TestableBot::Command *c = bot->parseCommand(probe, true, &args);
        TEST_ASSERT_NOT_NULL_MESSAGE(c, name);
        const char *callArgs = "";
        if (strcmp(name, "echo") == 0)
            callArgs = "hello mesh";
        else if (strcmp(name, "roll") == 0)
            callArgs = "20";
        char out[200] = {0};
        (bot->*(c->handler))(pkt, callArgs, out, sizeof(out));
        printf("  %-7s -> %s\n", name, out);
        TEST_ASSERT_TRUE_MESSAGE(out[0] != '\0', name);
    }
    printf("===================================\n");
    fflush(stdout);

    nodeDB = nullptr;
    delete mock;
}

// Exercises the action commands with their subsystems absent (service and router are null in the
// unit-test harness), so they must degrade gracefully rather than crash. notify and rules transmit
// nothing up front, so their cases only pin the gate and the usage line.
static void test_actionCommands()
{
    auto *mock = new MockNodeDB();
    mock->node.num = 0x433a1b2c;
    mock->nodes.push_back(mock->node);
    mock->meshNodes = &mock->nodes;
    mock->numMeshNodes = (pb_size_t)mock->nodes.size();
    nodeDB = mock;
    myNodeInfo.my_node_num = 0x11223344; // requireNodeId refuses our own NodeNum as a target

    memset(&pkt, 0, sizeof(pkt));
    pkt.from = 0x433a1b2c;

    struct ActionCase {
        const char *cmd;
        const char *expect; // exact expected reply
    };
    const ActionCase cases[] = {
        {"ackping !433a1b2c", "Radio unavailable"},
        {"notify me", "notify me|@!id|#N when @!id|me <cond> [: note] | in 2h | at 18:00"},
        {"every ping", "notify me|@!id|#N when @!id|me <cond> [: note] | in 2h | at 18:00"},
        {"rules", "no rules"},
        {"poll 0 Q?", "poll <1-1440 min> question | a | b"},
    };

    printf("\n===== MeshBot action commands (subsystems absent) =====\n");
    for (const auto &tc : cases) {
        const char *args = nullptr;
        const TestableBot::Command *c = bot->parseCommand(tc.cmd, true, &args);
        TEST_ASSERT_NOT_NULL_MESSAGE(c, tc.cmd);
        TEST_ASSERT_TRUE_MESSAGE(c->privileged, tc.cmd); // every action command must be gated
        char out[200] = {0};
        (bot->*(c->handler))(pkt, args, out, sizeof(out));
        printf("  %-18s -> %s\n", tc.cmd, out);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(tc.expect, out, tc.cmd);
    }
    printf("=======================================================\n");
    fflush(stdout);

    // A missing id is reported rather than silently transmitting.
    const char *args = nullptr;
    const TestableBot::Command *c = bot->parseCommand("ackping", true, &args);
    char out[64] = {0};
    (bot->*(c->handler))(pkt, args, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Need a node id", out);

    nodeDB = nullptr;
    delete mock;
}

namespace
{
constexpr NodeNum kBotNode = 0x11223344;
constexpr NodeNum kPeerNode = 0x433a1b2c;

// Brings up the globals the real gates read: a node database, the channel table, and a
// service/router pair whose transmissions are recorded. RecordingRouter::send() replaces the whole
// of Router::send(), so the packets it captures are still decoded and their text can be read.
struct BotHarness {
    MockNodeDB *db = nullptr;
    MockMeshService *svc = nullptr;
    RecordingRouter *rtr = nullptr;

    explicit BotHarness(bool withRadio = true)
    {
        config = meshtastic_LocalConfig_init_zero;
        db = new MockNodeDB();
        nodeDB = db;
        myNodeInfo.my_node_num = kBotNode;
        db->node.num = kPeerNode;
        db->nodes.push_back(db->node);
        db->meshNodes = &db->nodes;
        db->numMeshNodes = (pb_size_t)db->nodes.size();

        channels.initDefaults();
        // A private 128-bit PSK, so a "precise" position_precision is not clamped to the
        // publicly-decryptable ceiling and the precision under test is the one configured.
        meshtastic_Channel &ch = channels.getByIndex(0);
        memset(ch.settings.psk.bytes, 0xAB, 16);
        ch.settings.psk.size = 16;
        channels.onConfigChanged();

        svc = new MockMeshService();
        service = svc;
        if (withRadio) {
            rtr = new RecordingRouter();
            rtr->addInterface(std::unique_ptr<RadioInterface>(new NullRadioInterface()));
            router = rtr;
        }
    }

    ~BotHarness()
    {
        while (auto *q = svc->getQueueStatusForPhone())
            svc->releaseQueueStatusToPool(q);
        while (auto *t = svc->getForPhone())
            svc->releaseToPool(t);
        delete rtr;
        rtr = nullptr;
        router = nullptr;
        delete svc;
        svc = nullptr;
        service = nullptr;
        delete db;
        db = nullptr;
        nodeDB = nullptr;
    }

    void setChannelPrecision(uint32_t precision)
    {
        meshtastic_Channel &ch = channels.getByIndex(0);
        ch.settings.has_module_settings = true;
        ch.settings.module_settings.position_precision = precision;
        channels.onConfigChanged();
    }
};

meshtastic_MeshPacket makeTextDm(NodeNum from, const char *text)
{
    meshtastic_MeshPacket p = meshtastic_MeshPacket_init_zero;
    p.from = from;
    p.to = kBotNode;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p.decoded.payload.size = (pb_size_t)strlen(text);
    memcpy(p.decoded.payload.bytes, text, p.decoded.payload.size);
    return p;
}

meshtastic_MeshPacket makeTextBroadcast(NodeNum from, const char *text, uint8_t channel)
{
    meshtastic_MeshPacket p = makeTextDm(from, text);
    p.to = NODENUM_BROADCAST;
    p.channel = channel;
    return p;
}

// A ROUTING_APP ack/nak addressed to us, as the firmware builds one: the id of the packet it
// acknowledges travels in decoded.request_id, and error_reason NONE is the ack.
meshtastic_MeshPacket makeRoutingAck(NodeNum from, uint32_t requestId, meshtastic_Routing_Error err)
{
    meshtastic_Routing r = meshtastic_Routing_init_default;
    r.which_variant = meshtastic_Routing_error_reason_tag;
    r.error_reason = err;

    meshtastic_MeshPacket p = meshtastic_MeshPacket_init_zero;
    p.from = from;
    p.to = kBotNode;
    p.relay_node = NO_RELAY_NODE;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = meshtastic_PortNum_ROUTING_APP;
    p.decoded.request_id = requestId;
    p.decoded.payload.size =
        (pb_size_t)pb_encode_to_bytes(p.decoded.payload.bytes, sizeof(p.decoded.payload.bytes), &meshtastic_Routing_msg, &r);
    return p;
}

void attachPkiKey(meshtastic_MeshPacket &p, uint8_t fill, pb_size_t size = 32)
{
    p.pki_encrypted = true;
    memset(p.public_key.bytes, fill, sizeof(p.public_key.bytes));
    p.public_key.size = size;
}

void configureAdminKeys()
{
    config.security.admin_key_count = 2;
    config.security.admin_key[0].size = 32;
    memset(config.security.admin_key[0].bytes, 0xAA, 32);
    config.security.admin_key[1].size = 32;
    memset(config.security.admin_key[1].bytes, 0xBB, 32);
}

} // namespace

// The security boundary itself. Each case uses a different sender so the per-sender cooldown cannot
// be what denied it, and every denial is asserted as "nothing transmitted, nothing mutated".
static void test_handleReceived_privilegedRequiresAdminKey()
{
    BotHarness h;
    configureAdminKeys();

    // (1) A plain packet claiming the admin's own NodeNum - exactly the forgery the removed
    // MESHBOT_ALLOWED_SENDERS list could not tell from the real thing.
    auto p = makeTextDm(0x851c9f24, "ackping !433a1b2c");
    bot->handleReceived(p);
    TEST_ASSERT_EQUAL_UINT(0, h.rtr->sent.size());

    // (2) PKI-authenticated, but the key is nobody's admin key.
    p = makeTextDm(0x00000002, "ackping !433a1b2c");
    attachPkiKey(p, 0xCC);
    bot->handleReceived(p);
    TEST_ASSERT_EQUAL_UINT(0, h.rtr->sent.size());

    // (3) A 31-byte key whose bytes match admin_key[0]. AdminModule memcmps 32 bytes without
    // consulting public_key.size, so the length test has to be explicit here.
    p = makeTextDm(0x00000003, "ackping !433a1b2c");
    attachPkiKey(p, 0xAA, 31);
    bot->handleReceived(p);
    TEST_ASSERT_EQUAL_UINT(0, h.rtr->sent.size());

    // (4) PKI with the second configured admin key: the command runs, transmits and answers.
    p = makeTextDm(0x00000004, "ackping !433a1b2c");
    attachPkiKey(p, 0xBB);
    bot->handleReceived(p);
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());
    TEST_ASSERT_EQUAL(meshtastic_PortNum_REPLY_APP, h.rtr->sent[0].decoded.portnum);
    TEST_ASSERT_EQUAL_STRING("Pinging !433a1b2c", (const char *)h.rtr->sent[1].decoded.payload.bytes);

    // (5) Public commands stay open to an unauthenticated stranger. Past the global transmit gap
    // first: case (4) just transmitted, and the backstop is not what this case is about.
    Time::advanceTestMillis(5000);
    p = makeTextDm(0x00000005, "ping");
    bot->handleReceived(p);
    TEST_ASSERT_EQUAL_UINT(3, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_STRING("pong", (const char *)h.rtr->sent[2].decoded.payload.bytes);
}

// The gate on its own, including the fail-closed cases a device ships with.
static void test_isPrivilegedSenderAuthorized()
{
    config = meshtastic_LocalConfig_init_zero;

    auto plain = makeTextDm(kPeerNode, "ackping !1");
    TEST_ASSERT_FALSE(TestableBot::isPrivilegedSenderAuthorized(plain)); // no keys configured at all

    configureAdminKeys();
    TEST_ASSERT_FALSE(TestableBot::isPrivilegedSenderAuthorized(plain)); // not PKI-encrypted

    auto pki = makeTextDm(kPeerNode, "ackping !1");
    attachPkiKey(pki, 0xAA);
    TEST_ASSERT_TRUE(TestableBot::isPrivilegedSenderAuthorized(pki));

    attachPkiKey(pki, 0xBB);
    TEST_ASSERT_TRUE(TestableBot::isPrivilegedSenderAuthorized(pki));

    attachPkiKey(pki, 0xCC);
    TEST_ASSERT_FALSE(TestableBot::isPrivilegedSenderAuthorized(pki));

    attachPkiKey(pki, 0xAA, 31);
    TEST_ASSERT_FALSE(TestableBot::isPrivilegedSenderAuthorized(pki));

    attachPkiKey(pki, 0xAA, 0);
    TEST_ASSERT_FALSE(TestableBot::isPrivilegedSenderAuthorized(pki));

    // A configured slot shorter than 32 bytes must never match, however the packet's key is padded.
    config = meshtastic_LocalConfig_init_zero;
    config.security.admin_key_count = 1;
    config.security.admin_key[0].size = 4;
    memset(config.security.admin_key[0].bytes, 0x00, 32);
    attachPkiKey(pki, 0x00);
    TEST_ASSERT_FALSE(TestableBot::isPrivilegedSenderAuthorized(pki));
}

// The accepted grammar is "[ws][!]1-8 hex digits[ws]" and nothing else. strtoul took a sign, an 0x
// prefix, a 9-digit value and arbitrary trailing junk, and resolved all of them to some node.
static void test_parseNodeId_table()
{
    struct Case {
        const char *in;
        bool ok;
        uint32_t want;
    };
    const Case cases[] = {
        {"433a1b2c", true, 0x433a1b2c},
        {"!433a1b2c", true, 0x433a1b2c},
        {"@!433a1b2c", true, 0x433a1b2c}, // a mention token, as the apps put it on the wire
        {"@433a1b2c", true, 0x433a1b2c},
        {" deadbeef ", true, 0xdeadbeef},
        {"!00000001", true, 0x00000001},
        {"1", true, 0x1},
        {"FFFFFFFF", true, 0xffffffff},
        {"\t!AbCdEf12\n", true, 0xabcdef12},

        {"-1", false, 0},
        {"+5", false, 0},
        {"1ffffffff", false, 0},
        {"deadbeefzzz", false, 0},
        {"!433a1b2c garbage", false, 0},
        {"0x433a1b2c", false, 0},
        {"0xZZ", false, 0},
        {"!!1234", false, 0},
        {"!", false, 0},
        {"", false, 0},
        {"   ", false, 0},
        {"1234 5678", false, 0},
        {"12 34", false, 0},
        {"!-1", false, 0},
        {"@@!433a1b2c", false, 0},
        {"@ !433a1b2c", false, 0},
    };

    for (const auto &tc : cases) {
        uint32_t got = 0xa5a5a5a5;
        const bool ok = TestableBot::parseNodeId(tc.in, got);
        TEST_ASSERT_EQUAL_MESSAGE(tc.ok, ok, tc.in);
        if (tc.ok)
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(tc.want, got, tc.in);
        else
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(0xa5a5a5a5, got, tc.in); // a rejected id must not be written
    }
    uint32_t got = 0;
    TEST_ASSERT_FALSE(TestableBot::parseNodeId(nullptr, got));
}

static void test_parse_bareCommand()
{
    BotHarness h;
    const char *args = nullptr;
    auto *c = bot->parseCommand("ping", true, &args);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("ping", c->name);
    TEST_ASSERT_EQUAL_STRING("", args); // no trailing args
}

static void test_parse_leadingWhitespace()
{
    BotHarness h;
    const char *args = nullptr;
    auto *c = bot->parseCommand("   help", true, &args);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("help", c->name);
}

static void test_parse_argsTail()
{
    BotHarness h;
    const char *args = nullptr;
    auto *c = bot->parseCommand("echo hello world", true, &args);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("echo", c->name);
    TEST_ASSERT_EQUAL_STRING("hello world", args);
}

static void test_parse_notACommand()
{
    BotHarness h;
    const char *args = nullptr;
    TEST_ASSERT_NULL(bot->parseCommand("just a chat message", true, &args));
    TEST_ASSERT_NULL(bot->parseCommand("", true, &args));
}

static void test_parse_unknownCommand()
{
    BotHarness h;
    const char *args = nullptr;
    TEST_ASSERT_NULL(bot->parseCommand("bogus", true, &args));
    // A prefix of a real command must not match.
    TEST_ASSERT_NULL(bot->parseCommand("pin", true, &args));
}

// The bot is addressed the way the apps address a node: a "@!<8 hex>" mention token, which is plain
// text on the wire (meshtastic/design#21). On a channel the message has to open with the bot's own
// mention, so ordinary chat that happens to mention the bot mid-sentence never runs a command. In a
// DM the bot is plainly the one being addressed, so the mention is optional there. kBotNode is
// 0x11223344, so its mention is "@!11223344".
static void test_parse_mentionAddressing()
{
    BotHarness h;
    const char *args = nullptr;
    // Channel: mention required, then the command word in any case.
    TEST_ASSERT_EQUAL_STRING("ping", bot->parseCommand("@!11223344 ping", false, &args)->name);
    TEST_ASSERT_EQUAL_STRING("ping", bot->parseCommand("  @!11223344 PING", false, &args)->name);
    TEST_ASSERT_EQUAL_STRING("echo", bot->parseCommand("@!11223344 echo hi there", false, &args)->name);
    TEST_ASSERT_EQUAL_STRING("hi there", args);
    TEST_ASSERT_NOT_NULL(bot->parseCommand("@!11223344  ping", false, &args));   // Android's trailing space
    TEST_ASSERT_NULL(bot->parseCommand("ping", false, &args));                   // no mention on a channel
    TEST_ASSERT_NULL(bot->parseCommand("thanks @!11223344 ping", false, &args)); // mid-sentence
    TEST_ASSERT_NULL(bot->parseCommand("@!55667788 ping", false, &args));        // someone else
    TEST_ASSERT_NULL(bot->parseCommand("@!11223344ping", false, &args));         // no separator
    TEST_ASSERT_NULL(bot->parseCommand("@!11223344", false, &args));             // no command at all
    TEST_ASSERT_NULL(bot->parseCommand("/ping", false, &args));
    // DM: mention optional.
    TEST_ASSERT_EQUAL_STRING("ping", bot->parseCommand("ping", true, &args)->name);
    TEST_ASSERT_EQUAL_STRING("ping", bot->parseCommand("@!11223344 ping", true, &args)->name);
    TEST_ASSERT_NULL(bot->parseCommand("/ping", true, &args));
    TEST_ASSERT_NULL(bot->parseCommand("@!55667788 ping", true, &args));

    // Hex case: apps emit lowercase, Android can emit uppercase. Needs an id with letters in it.
    myNodeInfo.my_node_num = 0xa1b2c3d4;
    TEST_ASSERT_NOT_NULL(bot->parseCommand("@!a1b2c3d4 ping", false, &args));
    TEST_ASSERT_NOT_NULL(bot->parseCommand("@!A1B2C3D4 ping", false, &args));
    myNodeInfo.my_node_num = kBotNode;
}

// requireNodeId is where every action command's target rules live, so one check covers
// ackping. Broadcast is the one that matters: a single text
// message would otherwise fan out into a mesh-wide request and a NAK from every node that hears it.
static void test_requireNodeId_rejectsUnsafeTargets()
{
    BotHarness h(false);

    struct Case {
        const char *args;
        const char *expect; // "" means accepted
    };
    const Case cases[] = {
        {"", "Need a node id"},     {"nonsense", "Bad node id"}, {"!ffffffff", "Bad target"},
        {"ffffffff", "Bad target"}, {"0", "Bad target"},         {"!00000000", "Bad target"},
        {"11223344", "Bad target"}, {"!11223344", "Bad target"}, // ourselves
        {"433a1b2c", ""},           {"!433a1b2c", ""},
    };

    for (const auto &tc : cases) {
        uint32_t num = 0;
        char out[64] = {0};
        const bool ok = TestableBot::requireNodeId(tc.args, num, out, sizeof(out));
        if (tc.expect[0] == '\0') {
            TEST_ASSERT_TRUE_MESSAGE(ok, tc.args);
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(kPeerNode, num, tc.args);
        } else {
            TEST_ASSERT_FALSE_MESSAGE(ok, tc.args);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(tc.expect, out, tc.args);
        }
    }

    // NODENUM_BROADCAST_NO_LORA is a broadcast too, whatever it is spelled as.
    uint32_t num = 0;
    char out[64] = {0};
    char arg[16];
    snprintf(arg, sizeof(arg), "%08x", (unsigned)NODENUM_BROADCAST_NO_LORA);
    TEST_ASSERT_FALSE(TestableBot::requireNodeId(arg, num, out, sizeof(out)));

    // The read-only lookups keep their own parsing, so asking about ourselves still works.
    char reply[64] = {0};
    bot->cmdSeen(pkt, "!11223344", reply, sizeof(reply));
    TEST_ASSERT_TRUE(strcmp(reply, "Bad target") != 0);
}

// ackping, end to end. The ping is a REPLY_APP packet with want_ack and no want_response - the
// routing ack is the answer, and want_response would add a NO_RESPONSE nak from any target with no
// reply module, which reads as a failed ping and is not one. The report then says what the ack it
// got actually proves, which is three different things.
static void test_cmdAckPing_reportsWhatTheAckProves()
{
    BotHarness h;
    memset(&pkt, 0, sizeof(pkt));
    pkt.from = kPeerNode;
    pkt.to = kBotNode;
    pkt.id = 0x55;
    char out[64] = {0};

    bot->cmdAckPing(pkt, "!433a1b2c", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Pinging !433a1b2c", out);
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.size());
    const meshtastic_MeshPacket ping = h.rtr->sent[0];
    TEST_ASSERT_EQUAL(meshtastic_PortNum_REPLY_APP, ping.decoded.portnum);
    TEST_ASSERT_TRUE(ping.want_ack);
    TEST_ASSERT_FALSE(ping.decoded.want_response);
    TEST_ASSERT_TRUE(ping.id != 0);

    // An ack for some other packet of ours, from the right node, to us, on the right portnum. Only
    // request_id tells it from the one we are waiting for, which is why the id is tracked at all.
    auto stray = makeRoutingAck(kPeerNode, ping.id ^ 1u, meshtastic_Routing_Error_NONE);
    bot->handleReceived(stray);
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.size());

    // The real thing, from the target itself: delivery confirmed, with the round trip and the hops
    // the ack came back over.
    Time::advanceTestMillis(1200);
    auto ack = makeRoutingAck(kPeerNode, ping.id, meshtastic_Routing_Error_NONE);
    ack.hop_start = 3;
    ack.hop_limit = 1;
    bot->handleReceived(ack);
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_STRING("Ack !433a1b2c: ok 1200ms | 2hops", (const char *)h.rtr->sent[1].decoded.payload.bytes);
    // Threaded to the admin's command like every other answer, and a text, not a reaction.
    TEST_ASSERT_EQUAL_HEX32(0x55, h.rtr->sent.back().decoded.reply_id);
    TEST_ASSERT_EQUAL_UINT(0, h.rtr->sent.back().decoded.emoji);

    // An implicit ack: our own router acked our packet because it heard a neighbour rebroadcast it,
    // so it comes from us, not from the target. That proves the first leg left the node and nothing
    // about the far end, so the report must not call it delivered.
    bot->cmdAckPing(pkt, "!433a1b2c", out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(3, h.rtr->sent.size());
    auto implicit = makeRoutingAck(kBotNode, h.rtr->sent[2].id, meshtastic_Routing_Error_NONE);
    bot->handleReceived(implicit);
    TEST_ASSERT_EQUAL_UINT(4, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_STRING_LEN("Ack !433a1b2c: relayed", (const char *)h.rtr->sent[3].decoded.payload.bytes, 22);

    // A nak. Our own router raises MAX_RETRANSMIT once the retries are spent, so this is also how
    // "never arrived" arrives.
    bot->cmdAckPing(pkt, "!433a1b2c", out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(5, h.rtr->sent.size());
    auto nak = makeRoutingAck(kBotNode, h.rtr->sent[4].id, meshtastic_Routing_Error_MAX_RETRANSMIT);
    bot->handleReceived(nak);
    TEST_ASSERT_EQUAL_UINT(6, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_STRING("Ack !433a1b2c: no ack, err 5", (const char *)h.rtr->sent[5].decoded.payload.bytes);

    // A nak from the target itself is a different fact: ReliableRouter raises NO_CHANNEL from inside
    // its isToUs() branch, so the packet did arrive and only the decrypt failed. Reporting that as
    // "no ack" would hide a node that is plainly reachable.
    bot->cmdAckPing(pkt, "!433a1b2c", out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(7, h.rtr->sent.size());
    auto refused = makeRoutingAck(kPeerNode, h.rtr->sent[6].id, meshtastic_Routing_Error_NO_CHANNEL);
    bot->handleReceived(refused);
    TEST_ASSERT_EQUAL_UINT(8, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_STRING("Ack !433a1b2c: reached, err 6", (const char *)h.rtr->sent[7].decoded.payload.bytes);
}

// A reply that arrives after its own request has been given up on must not be credited to the next
// one, and the two waiting commands must not run at the same time.
static void test_pendingSlotIsExclusiveAndNotSatisfiedByStaleReplies()
{
    BotHarness h;
    memset(&pkt, 0, sizeof(pkt));
    pkt.from = kPeerNode;
    pkt.to = kBotNode;
    char out[64] = {0};

    // Time out a first ping, which clears the slot and reports.
    bot->cmdAckPing(pkt, "!433a1b2c", out, sizeof(out));
    const uint32_t firstPing = h.rtr->sent[0].id;
    Time::advanceTestMillis(31u * 1000u);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());

    // A second ping takes the slot.
    bot->cmdAckPing(pkt, "!433a1b2c", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Pinging !433a1b2c", out);
    TEST_ASSERT_EQUAL_UINT(3, h.rtr->sent.size());

    // The abandoned ping's ack, arriving now. Its id is not this slot's id, so it is ignored.
    auto late = makeRoutingAck(kPeerNode, firstPing, meshtastic_Routing_Error_NONE);
    bot->handleReceived(late);
    TEST_ASSERT_EQUAL_UINT(3, h.rtr->sent.size());

    // The second ping's own ack still lands, so the slot was never corrupted by the stale reply.
    auto ack = makeRoutingAck(kPeerNode, h.rtr->sent[2].id, meshtastic_Routing_Error_NONE);
    bot->handleReceived(ack);
    TEST_ASSERT_EQUAL_UINT(4, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_STRING_LEN("Ack !433a1b2c: ok", (const char *)h.rtr->sent[3].decoded.payload.bytes, 17);
}

// Nothing came back. The slot has to clear itself and say so, or ackping answers only when it
// succeeds and look broken when they don't.
static void test_pendingRequestTimesOut()
{
    BotHarness h;
    memset(&pkt, 0, sizeof(pkt));
    pkt.from = kPeerNode;
    pkt.to = kBotNode;
    pkt.id = 0x55;
    char out[64] = {0};

    bot->cmdAckPing(pkt, "!433a1b2c", out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.size());

    // A second request while one is in flight is refused rather than queued: one radio, one voice.
    char busy[64] = {0};
    bot->cmdAckPing(pkt, "!433a1b2c", busy, sizeof(busy));
    TEST_ASSERT_EQUAL_STRING("Busy, try again", busy);
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.size());

    TEST_ASSERT_EQUAL_INT(1000, bot->runOnce()); // still waiting
    Time::advanceTestMillis(31u * 1000u);
    TEST_ASSERT_EQUAL_INT(INT32_MAX, bot->runOnce());
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_STRING("Ack !433a1b2c: no response", (const char *)h.rtr->sent[1].decoded.payload.bytes);
    TEST_ASSERT_EQUAL_HEX32(0x55, h.rtr->sent.back().decoded.reply_id);
    TEST_ASSERT_EQUAL_UINT(0, h.rtr->sent.back().decoded.emoji);

    // And the slot is free again.
    bot->cmdAckPing(pkt, "!433a1b2c", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Pinging !433a1b2c", out);
}

// The backstop. The per-sender cooldowns bound one node; nothing bounded what the bot put on the
// air across senders, and a bot answers only because it was asked, so it is the traffic that yields
// first. Two limbs: a floor on the gap between our own transmits, and the airtime figures every
// periodic module consults - asked politely, which is the stricter of the two thresholds.
static void test_txBackstop_boundsTheBotNotJustTheSender()
{
    BotHarness h;

    auto first = makeTextDm(0x00000011, "ping");
    bot->handleReceived(first);
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.size());

    // A different sender, so its own cooldown is not what denies this.
    auto second = makeTextDm(0x00000012, "ping");
    Time::advanceTestMillis(500);
    bot->handleReceived(second);
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.size());

    // The backstop runs ahead of the per-sender window, which stamps the sender's turn as a side
    // effect: a command it dropped must not also have cost that sender their next one. 2000ms later
    // the global gap has passed, but less than the 3000ms DM cooldown has - so this only goes out if
    // the attempt above left the sender's window untouched.
    Time::advanceTestMillis(2000);
    bot->handleReceived(second);
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_STRING("pong", (const char *)h.rtr->sent[1].decoded.payload.bytes);

    // The airtime limb, and that it is the polite threshold: 33% of the window is under the 40%
    // ceiling the mesh-critical modules hold to and over the 25% a bot should.
    AirTime busy;
    airTime = &busy;
    busy.logAirtime(RX_LOG, 20000);
    TEST_ASSERT_FALSE(TestableBot::txBackstopAllows());

    Time::advanceTestMillis(5000);
    auto third = makeTextDm(0x00000013, "ping");
    bot->handleReceived(third);
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());

    airTime = nullptr;
    TEST_ASSERT_TRUE(TestableBot::txBackstopAllows()); // the gap alone no longer denies
}

// A send that never reached the router must not be reported as a request that went out. The router
// here has no interface, which is what a device looks like before the radio is up.
static void test_sendFailureIsNotReportedAsSuccess()
{
    BotHarness h(false);
    FailingRouter deaf;
    deaf.addInterface(std::unique_ptr<RadioInterface>(new NullRadioInterface()));
    router = &deaf;

    memset(&pkt, 0, sizeof(pkt));
    pkt.from = kPeerNode;
    pkt.to = kBotNode;

    char out[64] = {0};
    // ackping gets no slot out of a failed send, so the next one is not told the bot is busy.
    bot->cmdAckPing(pkt, "!433a1b2c", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Send failed", out);
    TEST_ASSERT_EQUAL_UINT(1, deaf.attempts); // it really tried, rather than refusing early
    bot->cmdAckPing(pkt, "!433a1b2c", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Send failed", out);

    router = nullptr;
}

// ---------------------------------------------------------------------------------------------
// Lookup access policy: public callers may ask about themselves, peer lookups need the admin key.
// ---------------------------------------------------------------------------------------------

namespace
{
constexpr NodeNum kThirdNode = 0x55667788;

// Three distinguishable records, so "answered about the wrong node" fails loudly rather than
// looking like a pass because one mock record answers for every id.
void seedThreeNodes(MockNodeDB *db)
{
    const struct {
        NodeNum num;
        const char *name;
    } rows[] = {{kBotNode, "BotSelf"}, {kPeerNode, "PeerWho"}, {kThirdNode, "ThirdParty"}};
    for (const auto &r : rows) {
        meshtastic_NodeInfoLite n = meshtastic_NodeInfoLite_init_zero;
        n.num = r.num;
        nodeInfoLiteSetBit(&n, NODEINFO_BITFIELD_HAS_USER_MASK, true);
        strncpy(n.long_name, r.name, sizeof(n.long_name) - 1);
        strncpy(n.short_name, r.name, sizeof(n.short_name) - 1);
        n.last_heard = getTime() - 60;
        db->byNum[r.num] = n;
    }
}

// Text of the single packet the bot sent for a command, or "" when it sent nothing.
const char *lastReply(RecordingRouter *rtr)
{
    return rtr->sent.empty() ? "" : (const char *)rtr->sent.back().decoded.payload.bytes;
}
} // namespace

// The resolver is the one place the lookup policy lives, so the table is the policy. The ordering
// matters as much as the verdicts: an unauthorized explicit lookup is refused before parseNodeId
// and before any NodeDB read, so the refusal cannot be read as an answer about the node named.
static void test_resolveLookupTarget_policy()
{
    BotHarness h;
    configureAdminKeys();
    char out[64];
    uint32_t num;

    // No argument resolves to the sender, whoever they are - including the bot's own NodeNum, which
    // is a legitimate thing to ask about and is only refused as the *target of an action*.
    auto dm = makeTextDm(kPeerNode, "seen");
    num = 0;
    out[0] = '\0';
    TEST_ASSERT_TRUE(TestableBot::resolveLookupTarget(dm, "", num, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX32(kPeerNode, num);
    TEST_ASSERT_EQUAL_STRING("", out); // nothing written on success

    // A nullptr args tail is the same as an empty one; handlers are reached both ways.
    num = 0;
    TEST_ASSERT_TRUE(TestableBot::resolveLookupTarget(dm, nullptr, num, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX32(kPeerNode, num);

    // A sender we cannot name: 0 is a locally-injected packet, and the broadcast address in `from`
    // is a forgery. Neither identifies a node whose own record we could report.
    auto local = makeTextDm(0, "seen");
    TEST_ASSERT_FALSE(TestableBot::resolveLookupTarget(local, "", num, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("No sender to look up", out);
    auto forged = makeTextDm(NODENUM_BROADCAST, "seen");
    TEST_ASSERT_FALSE(TestableBot::resolveLookupTarget(forged, "", num, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("No sender to look up", out);

    // An explicit id from an unauthenticated caller is refused, and refused identically whether the
    // id is well-formed, unknown to us, reserved, or not an id at all - so the reply is no oracle.
    const char *probes[] = {"!433a1b2c", "!55667788", "!deadbeef", "!0", "!ffffffff", "zzz", "!1234567890"};
    char refusal[64] = {0};
    for (const char *probe : probes) {
        out[0] = '\0';
        TEST_ASSERT_FALSE_MESSAGE(TestableBot::resolveLookupTarget(dm, probe, num, out, sizeof(out)), probe);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("Peer lookup is admin only", out, probe);
        if (refusal[0] == '\0')
            strncpy(refusal, out, sizeof(refusal) - 1);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(refusal, out, probe);
    }

    // The same requester, now PKI-authenticated with a configured admin key, may name a peer.
    auto admin = makeTextDm(kPeerNode, "seen !55667788");
    attachPkiKey(admin, 0xAA);
    TEST_ASSERT_TRUE(TestableBot::resolveLookupTarget(admin, "!55667788", num, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX32(kThirdNode, num);
    // Including the bot itself: reading our own record is a read, not an action.
    TEST_ASSERT_TRUE(TestableBot::resolveLookupTarget(admin, "!11223344", num, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX32(kBotNode, num);

    // Past the gate, the admin's id still has to be a usable one.
    TEST_ASSERT_FALSE(TestableBot::resolveLookupTarget(admin, "0x1234", num, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Bad node id", out);
    TEST_ASSERT_FALSE(TestableBot::resolveLookupTarget(admin, "!0", num, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Bad target", out);
    TEST_ASSERT_FALSE(TestableBot::resolveLookupTarget(admin, "!ffffffff", num, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Bad target", out);

    // A PKI DM whose key is not an admin key is an ordinary public caller.
    auto notAdmin = makeTextDm(kPeerNode, "seen !55667788");
    attachPkiKey(notAdmin, 0xCC);
    TEST_ASSERT_FALSE(TestableBot::resolveLookupTarget(notAdmin, "!55667788", num, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Peer lookup is admin only", out);
}

// Through the real handleReceived(), because that is the only path a packet takes on a device: a
// bare lookup reports on the sender and on nobody else. The regression guarded is the previous
// default, which resolved a bare lookup to the bot's own NodeNum - the bot answered about itself
// while the help text said the argument was optional.
static void test_handleReceived_bareLookupIsTheSender()
{
    BotHarness h;
    seedThreeNodes(h.db);

    // Distinguishable data per node, so each reply names which record it read.
    h.db->byNum[kBotNode].last_heard = getTime() - 86400;
    h.db->byNum[kThirdNode].last_heard = getTime() - 2 * 86400;

    const struct {
        const char *cmd;
        const char *expect;
    } cases[] = {{"seen", "m ago"}};
    for (const auto &tc : cases) {
        Time::advanceTestMillis(20000); // clear the global tx gap and this sender's cooldown
        auto p = makeTextDm(kPeerNode, tc.cmd);
        bot->handleReceived(p);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(lastReply(h.rtr), tc.expect), tc.cmd);
        TEST_ASSERT_NULL_MESSAGE(strstr(lastReply(h.rtr), "BotSelf"), tc.cmd);
        TEST_ASSERT_NULL_MESSAGE(strstr(lastReply(h.rtr), "ThirdParty"), tc.cmd);
        TEST_ASSERT_NULL_MESSAGE(strstr(lastReply(h.rtr), "d "), tc.cmd); // the others are days old
    }

    // A sender who is not in our NodeDB at all gets an answer about themselves and no fallback to
    // some other record. The id in the reply is their own, which they already knew.
    Time::advanceTestMillis(20000);
    auto stranger = makeTextDm(0x0a0b0c0d, "seen");
    bot->handleReceived(stranger);
    TEST_ASSERT_EQUAL_STRING("Unknown node !0a0b0c0d", lastReply(h.rtr));
}

// Every lookup command refuses an explicit id from a public caller, and refuses it the same way for
// a node we hold and a node we have never heard of. This is the privacy boundary of the whole
// group: before it, any node on the channel could ask the bot for a third party's position,
// battery, utilization and last-heard time, learned on channels the caller never shared.
static void test_handleReceived_peerLookupNeedsAdminKey()
{
    BotHarness h;
    configureAdminKeys();
    seedThreeNodes(h.db);

    const char *const lookups[] = {"seen"};
    NodeNum sender = 0x00010000;
    for (const char *name : lookups) {
        char held[64], unheld[64];
        for (int unknown = 0; unknown < 2; unknown++) {
            Time::advanceTestMillis(20000);
            char cmd[48];
            snprintf(cmd, sizeof(cmd), "%s %s", name, unknown ? "!0d0e0f00" : "!55667788");
            auto p = makeTextDm(++sender, cmd);
            bot->handleReceived(p);
            TEST_ASSERT_EQUAL_STRING_MESSAGE("Peer lookup is admin only", lastReply(h.rtr), cmd);
            strncpy(unknown ? unheld : held, lastReply(h.rtr), sizeof(held) - 1);
        }
        // Identical refusals, so the reply never says whether that record exists.
        TEST_ASSERT_EQUAL_STRING_MESSAGE(held, unheld, name);
    }

    // The refused commands disclosed nothing about the third party at any point.
    for (const auto &sent : h.rtr->sent) {
        const char *text = (const char *)sent.decoded.payload.bytes;
        TEST_ASSERT_NULL(strstr(text, "ThirdParty"));
        TEST_ASSERT_NULL(strstr(text, "Seen"));
    }
    // And armed nothing: a denial is not a request, so no reply slot is waiting on one.
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, bot->runOnce());
}

// The other half of the boundary: an admin may do exactly what the public caller may not.
static void test_handleReceived_adminPeerLookupIsAllowed()
{
    BotHarness h;
    configureAdminKeys();
    seedThreeNodes(h.db);
    h.db->byNum[kThirdNode].last_heard = getTime() - 2 * 86400; // tell its record from the caller's

    auto p = makeTextDm(kPeerNode, "seen !55667788");
    attachPkiKey(p, 0xAA);
    bot->handleReceived(p);
    TEST_ASSERT_EQUAL_STRING("Seen 2d 0h ago", lastReply(h.rtr));

    Time::advanceTestMillis(20000);
    auto q = makeTextDm(kPeerNode, "seen !55667788");
    attachPkiKey(q, 0xBB);
    bot->handleReceived(q);
    TEST_ASSERT_EQUAL_STRING("Seen 2d 0h ago", lastReply(h.rtr));
}

// A broadcast is never PKI-encrypted (Router::perhapsDecode only attempts PKI for a packet addressed
// to us alone), so no broadcast can clear the admin gate - not a peer lookup, and not a privileged
// command either, however the sender dresses the packet up.
static void test_handleReceived_broadcastCannotReachPrivilege()
{
    BotHarness h;
    configureAdminKeys();
    seedThreeNodes(h.db);

    // An explicit peer lookup broadcast with our mention: accepted as a command,
    // refused as a peer lookup.
    auto p = makeTextBroadcast(kPeerNode, "@!11223344 seen !55667788", 0);
    bot->handleReceived(p);
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_STRING("Peer lookup is admin only", lastReply(h.rtr));

    // A privileged command broadcast on the same channel is dropped without a word and without
    // transmitting - the same gate, reached by a packet that can never clear it.
    Time::advanceTestMillis(20000);
    auto q = makeTextBroadcast(kPeerNode, "@!11223344 ackping !55667788", 0);
    bot->handleReceived(q);
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.size());
}

// A mention says who the message is for, so the node's own channel list decides where the bot
// listens: a mention on any channel this node has is answered on that channel. A broadcast that
// does not open with the mention is chat, not a command.
static void test_handleReceived_mentionOnAnyChannel()
{
    BotHarness h;
    meshtastic_Channel &sec = channels.getByIndex(1);
    sec.has_settings = true;
    sec.role = meshtastic_Channel_Role_SECONDARY;
    channels.onConfigChanged();

    auto onSecondary = makeTextBroadcast(kPeerNode, "@!11223344 ping", 1);
    bot->handleReceived(onSecondary);
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent[0].channel);
    TEST_ASSERT_EQUAL_STRING("pong", lastReply(h.rtr));

    Time::advanceTestMillis(20000);
    auto unaddressed = makeTextBroadcast(0x00000021, "ping", 0);
    bot->handleReceived(unaddressed);
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.size());

    // A DM is accepted whatever channel byte it carries: PKI DMs arrive on channel 0, and a
    // channel-encrypted DM on a secondary is still a DM.
    Time::advanceTestMillis(20000);
    auto dm = makeTextDm(kPeerNode, "ping");
    dm.channel = 1;
    bot->handleReceived(dm);
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_STRING("pong", lastReply(h.rtr));

    sec.has_settings = false; // fixupChannel() disables and clears it; initDefaults() would not
    channels.onConfigChanged();
}

// Every answer is threaded to the question it answers: decoded.reply_id names the command packet,
// so the apps show the bot's text as a reply to it rather than as a loose line in the chat.
static void test_replies_areThreadedToTheCommand()
{
    BotHarness h;
    auto p = makeTextDm(kPeerNode, "ping");
    p.id = 0x1234;
    bot->handleReceived(p);
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_HEX32(0x1234, h.rtr->sent[0].decoded.reply_id);
    TEST_ASSERT_EQUAL_UINT(0, h.rtr->sent[0].decoded.emoji);
}

// stats is read from counters the firmware already keeps, and says nothing about a counter whose
// source does not exist on this node rather than printing a zero that reads as a measurement. It
// stays public because it is an aggregate: no part of it names a node.
static void test_cmdStats_reportsWhatExists()
{
    BotHarness h;
    seedThreeNodes(h.db);
    char out[200] = {0};
    bot->cmdStats(pkt, "", out, sizeof(out)); // no RadioLib radio, no airTime in the test harness
    TEST_ASSERT_EQUAL_STRING_LEN("Up ", out, 3);
    TEST_ASSERT_NOT_NULL(strstr(out, "| Nodes "));
    TEST_ASSERT_NULL(strstr(out, "RX"));
    TEST_ASSERT_NULL(strstr(out, "ChUtil"));
    TEST_ASSERT_NULL(strstr(out, "PeerWho"));
    TEST_ASSERT_NULL(strstr(out, "!"));

    AirTime air;
    airTime = &air;
    memset(out, 0, sizeof(out));
    bot->cmdStats(pkt, "", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "ChUtil "));
    TEST_ASSERT_NOT_NULL(strstr(out, "AirTx "));
    airTime = nullptr;
}

// Sunrise and sunset from the NOAA sunrise equation, checked against published times. The day it
// reports is the caller's local solar day, picked from their longitude rather than the UTC date:
// at 01:00 in Sydney the UTC date is still yesterday, and reading the day from it would report a
// sunrise that has already been and gone.
static void test_sunRiseSet_knownDays()
{
    uint32_t r = 0, s = 0;
    // London, 2024-06-20 12:00 UTC: sunrise 03:43, sunset 20:21 UTC (04:43 / 21:21 BST).
    TEST_ASSERT_EQUAL_INT(0, TestableBot::sunRiseSet(51.5074, -0.1278, 1718884800, r, s));
    TEST_ASSERT_UINT32_WITHIN(180, 1718854976, r);
    TEST_ASSERT_UINT32_WITHIN(180, 1718914876, s);
    // Sydney, 2024-06-20 12:00 UTC = 22:00 local: that local day's 07:00 rise and 16:53 set.
    TEST_ASSERT_EQUAL_INT(0, TestableBot::sunRiseSet(-33.8688, 151.2093, 1718884800, r, s));
    TEST_ASSERT_UINT32_WITHIN(180, 1718830786, r);
    TEST_ASSERT_UINT32_WITHIN(180, 1718866414, s);
    // Sydney, 2024-06-20 15:00 UTC = 01:00 local on the 21st: the 21st's sunrise, not the 20th's.
    TEST_ASSERT_EQUAL_INT(0, TestableBot::sunRiseSet(-33.8688, 151.2093, 1718884800 + 3 * 3600, r, s));
    TEST_ASSERT_UINT32_WITHIN(180, 1718917199, r);
    TEST_ASSERT_UINT32_WITHIN(180, 1718952826, s);
    // Tromso: midnight sun at midsummer, polar night at midwinter.
    TEST_ASSERT_EQUAL_INT(1, TestableBot::sunRiseSet(69.6492, 18.9553, 1718971200, r, s));
    TEST_ASSERT_EQUAL_INT(-1, TestableBot::sunRiseSet(69.6492, 18.9553, 1734782400, r, s));
}

// sun reports the clock day, the one the caller's calendar shows. Picking it from the longitude
// instead gave Madrid at 00:30 on the 21st the 20th's times, since Spain's clock runs ahead of its sun.
static void test_clockDayNoonUtc_isTheCalendarDay()
{
    // Sydney (+10h), 2024-06-20 15:00 UTC = 01:00 on the 21st -> noon on the 21st = 02:00 UTC.
    TEST_ASSERT_EQUAL_UINT32(1718935200, TestableBot::clockDayNoonUtc(1718895600, 36000));
    // Madrid summer (+2h), 2024-06-20 22:30 UTC = 00:30 on the 21st. The solar day by longitude
    // (-3.7) is still the 20th; the clock day is the 21st -> noon = 10:00 UTC on the 21st.
    TEST_ASSERT_EQUAL_UINT32(1718964000, TestableBot::clockDayNoonUtc(1718922600, 7200));
    // UTC, mid-afternoon: same day.
    TEST_ASSERT_EQUAL_UINT32(1718884800, TestableBot::clockDayNoonUtc(1718895600, 0));
}

// Moon age from the mean synodic month against a known new moon. The mean cycle drifts from the
// true one by under a day, so the checks allow that.
static void test_moonAgeDays_knownPhases()
{
    // New moon 2024-01-11 11:57 UTC, full moon 2024-01-25 17:54 UTC.
    const float newAge = TestableBot::moonAgeDays(1704974220);
    TEST_ASSERT_TRUE(newAge < 1.0f || newAge > 28.53f);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 14.77f, TestableBot::moonAgeDays(1706205240));
}

// sun reports for the caller's position, falling back to the bot's, and says so plainly when it
// has neither a position nor a clock it can trust rather than computing from 0,0 or from uptime.
static void test_cmdSun_needsPositionAndTime()
{
    BotHarness h;
    memset(&pkt, 0, sizeof(pkt));
    pkt.from = kPeerNode;
    char out[200] = {0};
    bot->cmdSun(pkt, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("no position", out);

    meshtastic_PositionLite london = meshtastic_PositionLite_init_zero;
    london.latitude_i = 515074000;
    london.longitude_i = -1278000;
    h.db->nodePositions[kBotNode] = london; // the bot's own position is the fallback
    memset(out, 0, sizeof(out));
    bot->cmdSun(pkt, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_LEN("Sunrise ", out, 8);
    TEST_ASSERT_NOT_NULL(strstr(out, " Sunset "));
    memset(out, 0, sizeof(out));
    bot->cmdMoon(pkt, "", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "% "));

    // At full moon the line is just the phase: "full in 0d" said nothing, and a little past full the
    // countdown rounded up to 30d, longer than a whole cycle. The RTC refuses a time before the build
    // date, so this takes the next full moon by the same mean month moonAgeDays counts in.
    constexpr double kSynodicSecs = 29.530588853 * 86400;
    const double cycles = floor((time(NULL) - 947182440) / kSynodicSecs) + 1.5;
    struct timeval full = {(time_t)(947182440 + cycles * kSynodicSecs), 0};
    setBootRelativeTimeForUnitTest(100);
    TEST_ASSERT_EQUAL_INT(RTCSetResultSuccess, perhapsSetRTC(RTCQualityNTP, &full));
    memset(out, 0, sizeof(out));
    bot->cmdMoon(pkt, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING_LEN("Full moon ", out, 10);
    TEST_ASSERT_NULL(strstr(out, "full in"));

    // No trusted clock: initializeTestEnvironment() set the RTC, so drop it and put it back after.
    setBootRelativeTimeForUnitTest(100);
    memset(out, 0, sizeof(out));
    bot->cmdSun(pkt, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("no valid time", out);
    memset(out, 0, sizeof(out));
    bot->cmdMoon(pkt, "", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("no valid time", out);
    struct timeval tv = {time(NULL), 0};
    perhapsSetRTC(RTCQualityNTP, &tv);
}

// notify keeps a rule in RAM and alerts someone when a node does what the rule watches for. It is an
// admin command because it makes the bot transmit on someone else's behalf later.
namespace
{
constexpr const char *kQueuedReaction = "\xE2\x9C\x85"; // the ✅ a stored rule is acknowledged with
constexpr uint32_t kTxGapMs = 2000;                     // MESHBOT_MIN_TX_GAP_MS's default

// Any packet the node sends, not just text: hearing telemetry from a node is hearing the node.
meshtastic_MeshPacket makeTelemetryFrom(NodeNum from)
{
    meshtastic_MeshPacket p = meshtastic_MeshPacket_init_zero;
    p.from = from;
    p.to = NODENUM_BROADCAST;
    p.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    p.decoded.portnum = meshtastic_PortNum_TELEMETRY_APP;
    return p;
}

void sendAdmin(NodeNum from, const char *text, uint32_t id = 0)
{
    auto cmd = makeTextDm(from, text);
    cmd.id = id;
    attachPkiKey(cmd, 0xAA);
    bot->handleReceived(cmd);
}
} // namespace

// poll is an admin command, so it always arrives as a PKI DM; the poll itself is announced, and its
// result posted, on the primary channel. That announcement is the opener's only confirmation: a
// second transmit straight after would be held back by the bot's minimum gap anyway. Votes are
// acknowledged with a reaction on the vote, so a busy poll does not flood the chat with replies.
namespace
{
constexpr const char *kVoteReaction = "\xF0\x9F\x91\x8D"; // 👍

void sendAdminPoll(NodeNum from, const char *text)
{
    auto cmd = makeTextDm(from, text);
    attachPkiKey(cmd, 0xAA);
    bot->handleReceived(cmd);
}
} // namespace

static void test_poll_openVoteClose()
{
    BotHarness h;
    configureAdminKeys();
    sendAdminPoll(0x00000041, "poll 5 Lunch? | pizza | tacos");
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_UINT(NODENUM_BROADCAST, h.rtr->sent[0].to);
    TEST_ASSERT_EQUAL_UINT(channels.getPrimaryIndex(), h.rtr->sent[0].channel);
    TEST_ASSERT_EQUAL_STRING("POLL: Lunch? 1)pizza 2)tacos | reply @!11223344 vote N (5m)", lastReply(h.rtr));
    const int32_t wake = bot->runOnce(); // sleeps until the close
    TEST_ASSERT_TRUE(wake <= 5 * 60 * 1000 && wake > 4 * 60 * 1000);

    Time::advanceTestMillis(3000);
    auto v = makeTextBroadcast(0x00000042, "@!11223344 vote 2", 0);
    v.id = 0x99;
    bot->handleReceived(v);
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent[1].decoded.emoji);
    TEST_ASSERT_EQUAL_HEX32(0x99, h.rtr->sent[1].decoded.reply_id);
    TEST_ASSERT_EQUAL_STRING(kVoteReaction, lastReply(h.rtr));

    Time::advanceTestMillis(3000);
    auto look = makeTextDm(0x00000043, "vote");
    bot->handleReceived(look);
    TEST_ASSERT_EQUAL_STRING("Lunch? | pizza:0 tacos:1", lastReply(h.rtr));

    Time::advanceTestMillis(20000);
    auto revote = makeTextBroadcast(0x00000042, "@!11223344 vote 1", 0); // replaces, not adds
    bot->handleReceived(revote);
    TEST_ASSERT_EQUAL_STRING(kVoteReaction, lastReply(h.rtr));

    Time::advanceTestMillis(5u * 60u * 1000u);
    TEST_ASSERT_EQUAL_INT(INT32_MAX, bot->runOnce());
    TEST_ASSERT_EQUAL_UINT(NODENUM_BROADCAST, h.rtr->sent.back().to);
    TEST_ASSERT_EQUAL_STRING("RESULT: Lunch? | pizza:1 tacos:0", lastReply(h.rtr));
}

// Between the deadline and the runOnce tick that announces the result, the poll is closed for
// voting: a vote cast then must not count toward a result already decided.
static void test_vote_afterDeadlineBeforeCloseIsRefused()
{
    BotHarness h;
    configureAdminKeys();
    sendAdminPoll(0x00000051, "poll 1 Coffee?");
    TEST_ASSERT_EQUAL_STRING("POLL: Coffee? 1)yes 2)no | reply @!11223344 vote N (1m)", lastReply(h.rtr));

    Time::advanceTestMillis(61u * 1000u);
    auto v = makeTextDm(0x00000052, "vote 1");
    bot->handleReceived(v);
    TEST_ASSERT_EQUAL_STRING("no poll", lastReply(h.rtr));
    TEST_ASSERT_EQUAL_UINT(0, h.rtr->sent.back().decoded.emoji);

    Time::advanceTestMillis(3000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_STRING("RESULT: Coffee? | yes:0 no:0", lastReply(h.rtr));
}

static void test_poll_gateAndLimits()
{
    BotHarness h;
    configureAdminKeys();

    auto stranger = makeTextDm(0x00000061, "poll 5 Free pizza?");
    bot->handleReceived(stranger);
    TEST_ASSERT_EQUAL_UINT(0, h.rtr->sent.size());

    const struct {
        const char *cmd;
        const char *expect;
    } bad[] = {
        {"poll 0 Q?", "poll <1-1440 min> question | a | b"},
        {"poll 1441 Q?", "poll <1-1440 min> question | a | b"},
        {"poll 5", "poll <1-1440 min> question | a | b"},
        {"poll five Q?", "poll <1-1440 min> question | a | b"},
        {"poll 5 Q? | a | | b", "poll <1-1440 min> question | a | b"},
        {"poll 5 Q? | a | b | c | d | e", "max 4 options"},
    };
    NodeNum admin = 0x70;
    for (const auto &tc : bad) {
        Time::advanceTestMillis(3000);
        sendAdminPoll(admin++, tc.cmd);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(tc.expect, lastReply(h.rtr), tc.cmd);
    }

    Time::advanceTestMillis(3000);
    sendAdminPoll(admin++, "poll 5 Q?");
    TEST_ASSERT_EQUAL_STRING_LEN("POLL: Q?", lastReply(h.rtr), 8);
    Time::advanceTestMillis(3000);
    sendAdminPoll(admin++, "poll 5 R?");
    TEST_ASSERT_EQUAL_STRING("poll already open", lastReply(h.rtr));

    // 32 voters fit; the 33rd is told rather than silently dropped. A bad choice is answered too.
    Time::advanceTestMillis(3000);
    auto badChoice = makeTextDm(0x00000100, "vote 3");
    bot->handleReceived(badChoice);
    TEST_ASSERT_EQUAL_STRING("vote 1-2", lastReply(h.rtr));
    for (NodeNum voter = 0x200; voter < 0x221; voter++) {
        Time::advanceTestMillis(2500);
        auto v = makeTextDm(voter, "vote 1");
        bot->handleReceived(v);
    }
    TEST_ASSERT_EQUAL_STRING("poll full", lastReply(h.rtr));
}

// Text that does not fit is refused, not cut: a poll announced without its tail (or ending in half
// a UTF-8 character) is not the poll the admin wrote.
static void test_poll_refusesTextTooLongToKeep()
{
    BotHarness h;
    configureAdminKeys();
    char cmd[240];
    snprintf(cmd, sizeof(cmd), "poll 5 %080d", 0); // an 80-character question
    sendAdminPoll(0x00000038, cmd);
    TEST_ASSERT_EQUAL_STRING("too long", lastReply(h.rtr));

    Time::advanceTestMillis(3000);
    sendAdminPoll(0x00000039, "poll 5 Q? | a | an option far too long to keep");
    TEST_ASSERT_EQUAL_STRING("too long", lastReply(h.rtr));

    // Every part fits on its own, but the announcement built from them does not.
    Time::advanceTestMillis(3000);
    char q[200];
    snprintf(q, sizeof(q), "poll 5 %079d | %023d | %023d | %023d | %023d", 0, 1, 2, 3, 4);
    const size_t before = h.rtr->sent.size();
    sendAdminPoll(0x0000003a, q);
    TEST_ASSERT_EQUAL_STRING("too long", lastReply(h.rtr));
    TEST_ASSERT_EQUAL_UINT(before + 1, h.rtr->sent.size()); // the refusal only, no broadcast
}

// notify is the admin's rule engine: "when <node> <condition>, tell <who>", or a plain reminder.
// These cases cover the grammar and the table; the rules fire in the cases after them.
static void test_notify_parseHelpers()
{
    uint32_t v;
    TEST_ASSERT_TRUE(TestableBot::parseDuration("90m", v));
    TEST_ASSERT_EQUAL_UINT32(5400, v);
    TEST_ASSERT_TRUE(TestableBot::parseDuration("2h", v));
    TEST_ASSERT_EQUAL_UINT32(7200, v);
    TEST_ASSERT_TRUE(TestableBot::parseDuration("1d", v));
    TEST_ASSERT_EQUAL_UINT32(86400, v);
    for (const char *bad : {"", "0m", "5", "5s", "-5m", "5mm", "m"})
        TEST_ASSERT_FALSE_MESSAGE(TestableBot::parseDuration(bad, v), bad);
    TEST_ASSERT_TRUE(TestableBot::parseDistance("500m", v));
    TEST_ASSERT_EQUAL_UINT32(500, v);
    TEST_ASSERT_TRUE(TestableBot::parseDistance("2km", v));
    TEST_ASSERT_EQUAL_UINT32(2000, v);
    TEST_ASSERT_FALSE(TestableBot::parseDistance("2k", v));
    TEST_ASSERT_EQUAL_UINT32(60, TestableBot::secondsUntil(17 * 3600 + 59 * 60, 18 * 60));
    TEST_ASSERT_EQUAL_UINT32(86400, TestableBot::secondsUntil(18 * 3600, 18 * 60)); // now: next time is tomorrow
}

static void test_notify_createsAcksAndRefuses()
{
    BotHarness h;
    configureAdminKeys();

    // A stranger gets nothing, and the bot watches nothing on their say-so.
    auto s = makeTextDm(0x00000033, "notify me when @!433a1b2c heard");
    bot->handleReceived(s);
    TEST_ASSERT_EQUAL_UINT(0, h.rtr->sent.size());
    auto tel = makeTelemetryFrom(kPeerNode);
    TEST_ASSERT_FALSE(bot->wantPacket(&tel));

    meshtastic_DeviceMetrics m = meshtastic_DeviceMetrics_init_zero;
    m.has_battery_level = true;
    m.battery_level = 80;
    h.db->nodeTelemetry[kPeerNode] = m;
    sendAdmin(0x00000031, "notify me when @!433a1b2c battery < 20%: swap panel", 0x77);
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.back().decoded.emoji);
    TEST_ASSERT_EQUAL_STRING(kQueuedReaction, lastReply(h.rtr));
    TEST_ASSERT_EQUAL_HEX32(0x77, h.rtr->sent.back().decoded.reply_id);
    TEST_ASSERT_TRUE(bot->wantPacket(&tel));

    static const char *const kUsage = "notify me|@!id|#N when @!id|me <cond> [: note] | in 2h | at 18:00";
    char tooLong[240];
    snprintf(tooLong, sizeof(tooLong), "notify me when @!433a1b2c heard: %0160d", 0);
    const struct {
        const char *cmd;
        const char *expect;
    } bad[] = {
        {"notify #9 when @!433a1b2c heard", "Bad channel"},
        {"notify #1 when @!433a1b2c heard", "Bad channel"}, // exists but disabled
        {"notify me when @!11223344 heard", "Bad target"},  // the bot itself
        {"every notify me in 10m: x", "min 15m"},
        {"every notify me when @!433a1b2c silent 10m", "min 15m"},
        {"notify @!433a1b2c when @!433a1b2c heard", kUsage}, // the tell form needs a note
        {tooLong, "too long"},
        {"notify me when @!433a1b2c sideways", kUsage},
        {"notify me when @!433a1b2c battery < 0%", kUsage},
        {"notify me in 2x", kUsage},
        {"notify me", kUsage},
        {"every ping", kUsage},
    };
    NodeNum admin = 0x80;
    for (const auto &tc : bad) {
        Time::advanceTestMillis(3000);
        sendAdmin(admin++, tc.cmd);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(tc.expect, lastReply(h.rtr), tc.cmd);
    }
}

static void test_rules_listPageAndDelete()
{
    BotHarness h;
    configureAdminKeys();
    meshtastic_NodeInfoLite peer = h.db->node;
    nodeInfoLiteSetBit(&peer, NODEINFO_BITFIELD_HAS_USER_MASK, true);
    strncpy(peer.short_name, "PEER", sizeof(peer.short_name) - 1);
    nodeInfoLiteSetBit(&peer, NODEINFO_BITFIELD_HAS_SNR_MASK, true);
    peer.has_hops_away = true;
    h.db->byNum[kPeerNode] = peer; // the only node known, so the admin 0x31 labels as !0031
    meshtastic_DeviceMetrics m = meshtastic_DeviceMetrics_init_zero;
    m.has_battery_level = true;
    m.battery_level = 80;
    h.db->nodeTelemetry[kPeerNode] = m;
    meshtastic_PositionLite here = meshtastic_PositionLite_init_zero;
    here.latitude_i = 515074000;
    here.longitude_i = -1278000;
    h.db->nodePositions[kPeerNode] = here;

    // All but the first repeat, so the listing runs past one packet.
    const char *const make[] = {
        "notify me when @!433a1b2c heard",
        "every notify me when @!433a1b2c silent 90m",
        "every notify me when @!433a1b2c battery < 20%",
        "every notify me when @!433a1b2c snr < -10",
        "every notify me when @!433a1b2c hops > 2",
        "every notify me when @!433a1b2c moved 500m",
        "every notify me when @!433a1b2c near @!433a1b2c 300m",
        "every notify me in 2h",
    };
    for (const char *cmd : make) {
        Time::advanceTestMillis(20000);
        sendAdmin(0x00000031, cmd);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(kQueuedReaction, lastReply(h.rtr), cmd);
    }
    Time::advanceTestMillis(20000);
    sendAdmin(0x00000031, "notify me in 2h");
    TEST_ASSERT_EQUAL_STRING("rules full", lastReply(h.rtr));

    Time::advanceTestMillis(20000);
    sendAdmin(0x00000031, "rules");
    const std::string page1 = lastReply(h.rtr);
    TEST_ASSERT_EQUAL_STRING_LEN("1 heard PEER>!0031 | 2 every silent90m PEER>!0031 | 3 every bat<20 PEER>!0031", page1.c_str(),
                                 77);
    TEST_ASSERT_NOT_NULL(strstr(page1.c_str(), "4 every snr<-10 PEER>!0031 | 5 every hops>2 PEER>!0031"));
    TEST_ASSERT_NOT_NULL(strstr(page1.c_str(), "6 every moved500m PEER>!0031"));
    TEST_ASSERT_TRUE(page1.size() < 200);
    const char *more = strstr(page1.c_str(), " +");
    TEST_ASSERT_NOT_NULL(more);
    unsigned left = 0, next = 0;
    TEST_ASSERT_EQUAL_INT(2, sscanf(more, " +%u: rules %u", &left, &next));
    TEST_ASSERT_EQUAL_UINT(9 - next, left);

    Time::advanceTestMillis(20000);
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "rules %u", next);
    sendAdmin(0x00000031, cmd);
    char start[4];
    snprintf(start, sizeof(start), "%u ", next);
    TEST_ASSERT_EQUAL_STRING_LEN(start, lastReply(h.rtr), strlen(start));
    TEST_ASSERT_NULL(strstr(lastReply(h.rtr), "+"));
    TEST_ASSERT_NOT_NULL(strstr(lastReply(h.rtr), "8 every in2h>!0031"));
    TEST_ASSERT_NOT_NULL(strstr(lastReply(h.rtr), "near300m PEER,PEER>!0031"));

    Time::advanceTestMillis(20000);
    sendAdmin(0x00000031, "rules del 2");
    TEST_ASSERT_EQUAL_STRING(kQueuedReaction, lastReply(h.rtr));
    Time::advanceTestMillis(20000);
    sendAdmin(0x00000031, "rules del 2");
    TEST_ASSERT_EQUAL_STRING("no rule 2", lastReply(h.rtr));
    Time::advanceTestMillis(20000);
    sendAdmin(0x00000031, "rules");
    TEST_ASSERT_NULL(strstr(lastReply(h.rtr), "2 every"));

    // Once no rule watches the peer, the bot stops taking its packets.
    auto tel = makeTelemetryFrom(kPeerNode);
    TEST_ASSERT_TRUE(bot->wantPacket(&tel));
    for (const char *del : {"rules del 1", "rules del 3", "rules del 4", "rules del 5", "rules del 6", "rules del 7"}) {
        Time::advanceTestMillis(20000);
        sendAdmin(0x00000031, del);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(kQueuedReaction, lastReply(h.rtr), del);
    }
    TEST_ASSERT_FALSE(bot->wantPacket(&tel));

    // The note separator is the first ':' not between two digits, so a clock time survives both in
    // the rule and in its note.
    Time::advanceTestMillis(20000);
    sendAdmin(0x00000031, "notify me at 18:00: dinner at 19:30");
    TEST_ASSERT_EQUAL_STRING(kQueuedReaction, lastReply(h.rtr));
    Time::advanceTestMillis(20000);
    sendAdmin(0x00000031, "rules");
    TEST_ASSERT_EQUAL_STRING("1 at18:00>!0031 | 8 every in2h>!0031", lastReply(h.rtr));

    Time::advanceTestMillis(20000);
    sendAdmin(0x00000031, "rules del 9");
    TEST_ASSERT_EQUAL_STRING("no rule 9", lastReply(h.rtr));
    Time::advanceTestMillis(20000);
    sendAdmin(0x00000031, "rules 9");
    TEST_ASSERT_EQUAL_STRING("no rules", lastReply(h.rtr));
    Time::advanceTestMillis(20000);
    sendAdmin(0x00000031, "rules 4294967296"); // wraps to 0 as an int on a 64-bit host
    TEST_ASSERT_EQUAL_STRING("no rules", lastReply(h.rtr));
}

// The tell replacement: "notify @!x when @!x heard: msg" reaches x the next time the bot hears it,
// in the same words tell used. The alert is the rule's only copy once sent, so it asks for an ack.
static void test_notify_heardDeliversLikeTell()
{
    BotHarness h;
    configureAdminKeys();
    sendAdmin(0x00000031, "notify @!433a1b2c when @!433a1b2c heard: lunch at noon", 0x77);
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_UINT(0x00000031, h.rtr->sent[0].to);
    TEST_ASSERT_FALSE(h.rtr->sent[0].want_ack); // the reaction is an ordinary answer

    Time::advanceTestMillis(5000);
    auto tel = makeTelemetryFrom(kPeerNode);
    TEST_ASSERT_TRUE(bot->wantPacket(&tel));
    bot->handleReceived(tel);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_UINT(kPeerNode, h.rtr->sent[1].to);
    TEST_ASSERT_TRUE(h.rtr->sent[1].want_ack);
    TEST_ASSERT_EQUAL_UINT(0, h.rtr->sent[1].decoded.reply_id);
    TEST_ASSERT_EQUAL_UINT(0, h.rtr->sent[1].decoded.emoji);
    TEST_ASSERT_EQUAL_STRING("From !00000031: lunch at noon", lastReply(h.rtr));

    // One-shot: gone once sent, and the bot stops asking to see that node's traffic.
    Time::advanceTestMillis(5000);
    TEST_ASSERT_FALSE(bot->wantPacket(&tel));
    bot->handleReceived(tel);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());
}

// The node's own command is answered first. The alert then loses to the minimum gap between the
// bot's transmits and is held for a later sweep, never dropped.
static void test_notify_alertWaitsForTheTxGap()
{
    BotHarness h;
    configureAdminKeys();
    sendAdmin(0x00000032, "notify @!433a1b2c when @!433a1b2c heard: see you there");
    TEST_ASSERT_EQUAL_UINT(1, h.rtr->sent.size());

    Time::advanceTestMillis(5000);
    auto ping = makeTextDm(kPeerNode, "ping");
    bot->handleReceived(ping);
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_STRING("pong", lastReply(h.rtr));

    TEST_ASSERT_TRUE(bot->runOnce() <= (int32_t)kTxGapMs);
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());
    Time::advanceTestMillis(kTxGapMs);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(3, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_STRING("From !00000032: see you there", lastReply(h.rtr));
}

static void test_notify_expiresAfterAWeek()
{
    BotHarness h;
    configureAdminKeys();
    sendAdmin(0x00000033, "notify me when @!433a1b2c heard");
    const int32_t wake = bot->runOnce(); // the TTL is the only deadline
    TEST_ASSERT_TRUE(wake > 0 && wake <= (int32_t)(7u * 24 * 3600 * 1000));

    const size_t sentBefore = h.rtr->sent.size();
    Time::advanceTestMillis(7u * 24 * 3600 * 1000 + 1000);
    TEST_ASSERT_EQUAL_INT(INT32_MAX, bot->runOnce());
    auto tel = makeTelemetryFrom(kPeerNode);
    TEST_ASSERT_FALSE(bot->wantPacket(&tel));
    bot->handleReceived(tel);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(sentBefore, h.rtr->sent.size());
}

// To the admin who set it the alert is just the facts; to anyone else, a channel included, it says
// who set it. A channel alert is a plain broadcast: an ack there would come from every node.
static void test_notify_heardToOwnerAndChannel()
{
    BotHarness h;
    configureAdminKeys();
    sendAdmin(0x00000031, "notify me when @!433a1b2c heard");
    Time::advanceTestMillis(kTxGapMs);
    sendAdmin(0x00000034, "notify #0 when @!433a1b2c heard: hi");
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());

    Time::advanceTestMillis(5000);
    auto tel = makeTelemetryFrom(kPeerNode);
    bot->handleReceived(tel);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(3, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_UINT(0x00000031, h.rtr->sent[2].to);
    TEST_ASSERT_TRUE(h.rtr->sent[2].want_ack);
    TEST_ASSERT_EQUAL_STRING("!433a1b2c heard", lastReply(h.rtr));

    Time::advanceTestMillis(kTxGapMs);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(4, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_UINT(NODENUM_BROADCAST, h.rtr->sent[3].to);
    TEST_ASSERT_EQUAL_UINT(0, h.rtr->sent[3].channel);
    TEST_ASSERT_FALSE(h.rtr->sent[3].want_ack);
    TEST_ASSERT_EQUAL_STRING("From !00000034: !433a1b2c heard: hi", lastReply(h.rtr));
}

// every heard on a node that beacons every 5 minutes alerts every 15, not on every packet.
static void test_notify_everyHeardFloor()
{
    BotHarness h;
    configureAdminKeys();
    sendAdmin(0x00000031, "every notify me when @!433a1b2c heard");
    auto tel = makeTelemetryFrom(kPeerNode);

    Time::advanceTestMillis(5000);
    bot->handleReceived(tel);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_STRING("!433a1b2c heard", lastReply(h.rtr));

    Time::advanceTestMillis(5u * 60 * 1000);
    bot->handleReceived(tel);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(2, h.rtr->sent.size());

    Time::advanceTestMillis(10u * 60 * 1000);
    bot->handleReceived(tel);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(3, h.rtr->sent.size());
    TEST_ASSERT_TRUE(bot->wantPacket(&tel)); // still watching
}

static void test_notify_inAndAt()
{
    BotHarness h;
    configureAdminKeys();
    sendAdmin(0x00000031, "notify me in 2h: move the car");
    int32_t wake = bot->runOnce();
    TEST_ASSERT_TRUE(wake <= 7200000 && wake > 7190000);
    Time::advanceTestMillis(2u * 3600 * 1000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_STRING("reminder: move the car", lastReply(h.rtr));
    TEST_ASSERT_EQUAL_INT(INT32_MAX, bot->runOnce()); // gone

    // at runs on the local clock. The RTC refuses a time before the build date, so take noon UTC
    // tomorrow (TZ is unset here, so local is UTC).
    struct timeval noon = {(time(NULL) / 86400 + 1) * 86400 + 12 * 3600, 0};
    Time::advanceTestMillis(kTxGapMs);
    setBootRelativeTimeForUnitTest(100);
    TEST_ASSERT_EQUAL_INT(RTCSetResultSuccess, perhapsSetRTC(RTCQualityNTP, &noon));
    sendAdmin(0x00000032, "notify me at 12:30: tea");
    TEST_ASSERT_EQUAL_STRING(kQueuedReaction, lastReply(h.rtr));
    Time::advanceTestMillis(30u * 60 * 1000 - 1000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_STRING(kQueuedReaction, lastReply(h.rtr)); // not yet
    Time::advanceTestMillis(2000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_STRING("reminder: tea", lastReply(h.rtr));

    Time::advanceTestMillis(kTxGapMs);
    setBootRelativeTimeForUnitTest(100);
    sendAdmin(0x00000033, "notify me at 12:30: tea");
    TEST_ASSERT_EQUAL_STRING("no clock", lastReply(h.rtr));
    struct timeval tv = {time(NULL), 0};
    perhapsSetRTC(RTCQualityNTP, &tv);
}

// Condition rules read NodeDB, which the bot learns from what nodes already broadcast; a packet
// from the watched node wakes the sweep that evaluates them.
namespace
{
void reportBattery(BotHarness &h, uint32_t level)
{
    meshtastic_DeviceMetrics m = meshtastic_DeviceMetrics_init_zero;
    m.has_battery_level = true;
    m.battery_level = level;
    h.db->nodeTelemetry[kPeerNode] = m;
    Time::advanceTestMillis(kTxGapMs);
    auto tel = makeTelemetryFrom(kPeerNode);
    bot->handleReceived(tel);
    bot->runOnce();
}
} // namespace

// Fires on the way down through the threshold. A repeating copy re-arms only once the level is back
// 5% above it, and never within 15 minutes of its last alert, so a battery hovering at the line
// does not alert on every report.
static void test_notify_batteryCrossingAndMargin()
{
    BotHarness h;
    configureAdminKeys();
    sendAdmin(0x00000031, "notify me when @!433a1b2c battery < 20%: swap panel");
    TEST_ASSERT_EQUAL_STRING("ok, but no battery data from !433a1b2c yet", lastReply(h.rtr));

    size_t sent = h.rtr->sent.size();
    reportBattery(h, 25);
    TEST_ASSERT_EQUAL_UINT(sent, h.rtr->sent.size());
    reportBattery(h, 19);
    TEST_ASSERT_EQUAL_UINT(0x00000031, h.rtr->sent.back().to);
    TEST_ASSERT_EQUAL_STRING("!433a1b2c battery 19%: swap panel", lastReply(h.rtr));
    auto tel = makeTelemetryFrom(kPeerNode);
    TEST_ASSERT_FALSE(bot->wantPacket(&tel)); // one-shot, gone

    Time::advanceTestMillis(kTxGapMs);
    sendAdmin(0x00000032, "every notify me when @!433a1b2c battery < 20%");
    Time::advanceTestMillis(kTxGapMs);
    bot->runOnce();
    TEST_ASSERT_EQUAL_STRING("!433a1b2c battery 19%", lastReply(h.rtr)); // already below
    sent = h.rtr->sent.size();
    for (uint32_t level : {18, 23, 25, 19}) // 25 re-arms, but 19 comes inside the 15 minutes
        reportBattery(h, level);
    TEST_ASSERT_EQUAL_UINT(sent, h.rtr->sent.size());
    Time::advanceTestMillis(15u * 60 * 1000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(sent + 1, h.rtr->sent.size());
    TEST_ASSERT_EQUAL_STRING("!433a1b2c battery 19%", lastReply(h.rtr));

    // 101 is "on external power", not a level below anything.
    Time::advanceTestMillis(kTxGapMs);
    sendAdmin(0x00000033, "notify me when @!433a1b2c battery < 100%");
    reportBattery(h, 101);
    Time::advanceTestMillis(16u * 60 * 1000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(sent + 2, h.rtr->sent.size()); // the ✅ only
}

static void test_notify_alreadyTrueFiresAtOnce()
{
    BotHarness h;
    configureAdminKeys();
    meshtastic_DeviceMetrics m = meshtastic_DeviceMetrics_init_zero;
    m.has_battery_level = true;
    m.battery_level = 15;
    h.db->nodeTelemetry[kPeerNode] = m;
    sendAdmin(0x00000031, "notify me when @!433a1b2c battery < 20%");
    TEST_ASSERT_EQUAL_STRING(kQueuedReaction, lastReply(h.rtr));
    Time::advanceTestMillis(kTxGapMs);
    bot->runOnce();
    TEST_ASSERT_EQUAL_STRING("!433a1b2c battery 15%", lastReply(h.rtr));
}

static void test_notify_silent()
{
    BotHarness h;
    configureAdminKeys();
    h.db->node.last_heard = getTime(); // heard just now
    sendAdmin(0x00000031, "notify #0 when @!433a1b2c silent 60m");
    const int32_t wake = bot->runOnce();
    TEST_ASSERT_TRUE(wake <= 3600000 && wake > 3590000);
    Time::advanceTestMillis(60u * 60 * 1000 + 1000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(NODENUM_BROADCAST, h.rtr->sent.back().to);
    TEST_ASSERT_EQUAL_STRING("From !00000031: !433a1b2c silent 60m", lastReply(h.rtr));

    // A repeating copy alerts once per silence: it re-arms when the node is heard again.
    Time::advanceTestMillis(kTxGapMs);
    sendAdmin(0x00000032, "every notify me when @!433a1b2c silent 60m");
    Time::advanceTestMillis(kTxGapMs);
    bot->runOnce(); // NodeDB last heard it over an hour ago
    TEST_ASSERT_EQUAL_STRING("!433a1b2c silent 60m", lastReply(h.rtr));
    const size_t sent = h.rtr->sent.size();
    Time::advanceTestMillis(60u * 60 * 1000 + 1000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(sent, h.rtr->sent.size());
    auto tel = makeTelemetryFrom(kPeerNode);
    bot->handleReceived(tel);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(sent, h.rtr->sent.size());
    Time::advanceTestMillis(60u * 60 * 1000 + 1000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(sent + 1, h.rtr->sent.size());

    // "me" as the watched node is the admin who set the rule.
    Time::advanceTestMillis(kTxGapMs);
    sendAdmin(0x00000031, "notify #0 when me silent 60m");
    auto fromAdmin = makeTelemetryFrom(0x00000031);
    TEST_ASSERT_TRUE(bot->wantPacket(&fromAdmin));
    Time::advanceTestMillis(20000);
    sendAdmin(0x00000031, "rules");
    TEST_ASSERT_NOT_NULL(strstr(lastReply(h.rtr), "silent60m !0031>#0"));
}

static void test_notify_snrAndHops()
{
    BotHarness h;
    configureAdminKeys();
    nodeInfoLiteSetBit(&h.db->node, NODEINFO_BITFIELD_HAS_SNR_MASK, true);
    h.db->node.snr = -10;
    h.db->node.has_hops_away = true;
    h.db->node.hops_away = 2;
    sendAdmin(0x00000031, "notify me when @!433a1b2c snr < -10");
    Time::advanceTestMillis(kTxGapMs);
    sendAdmin(0x00000032, "notify me when @!433a1b2c hops > 2");
    const size_t sent = h.rtr->sent.size();
    Time::advanceTestMillis(kTxGapMs);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(sent, h.rtr->sent.size()); // -10 is not below -10, 2 is not above 2

    h.db->node.snr = -12;
    h.db->node.hops_away = 3;
    auto tel = makeTelemetryFrom(kPeerNode);
    bot->handleReceived(tel);
    bot->runOnce();
    TEST_ASSERT_EQUAL_STRING("!433a1b2c SNR -12.0", lastReply(h.rtr));
    Time::advanceTestMillis(kTxGapMs);
    bot->runOnce();
    TEST_ASSERT_EQUAL_STRING("!433a1b2c 3 hops", lastReply(h.rtr));
    TEST_ASSERT_EQUAL_UINT(0x00000032, h.rtr->sent.back().to);

    // A node known only from NodeInfo or MQTT has no SNR or hop count: 0 there is not a reading, so
    // the rule waits for one rather than alerting "SNR 0.0", and the admin is told.
    nodeInfoLiteSetBit(&h.db->node, NODEINFO_BITFIELD_HAS_SNR_MASK, false);
    h.db->node.snr = 0;
    h.db->node.has_hops_away = false;
    Time::advanceTestMillis(kTxGapMs);
    sendAdmin(0x00000034, "notify me when @!433a1b2c snr < 5");
    TEST_ASSERT_EQUAL_STRING("ok, but never heard !433a1b2c", lastReply(h.rtr));
    Time::advanceTestMillis(kTxGapMs);
    sendAdmin(0x00000035, "notify me when @!433a1b2c hops > 0");
    TEST_ASSERT_EQUAL_STRING("ok, but never heard !433a1b2c", lastReply(h.rtr));
    const size_t quiet = h.rtr->sent.size();
    bot->handleReceived(tel);
    Time::advanceTestMillis(kTxGapMs);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(quiet, h.rtr->sent.size());

    // A node the bot has never heard has neither, which the admin is told when setting the rule.
    h.db->byNum[0x0000beef] = h.db->node;
    Time::advanceTestMillis(kTxGapMs);
    sendAdmin(0x00000033, "notify me when @!433a1b2c snr < 0");
    TEST_ASSERT_EQUAL_STRING("ok, but never heard !433a1b2c", lastReply(h.rtr));
}

// Movement rules compare positions NodeDB already holds. moved is measured from where the node was
// when the rule was made, or from the first position the bot sees if it had none then.
namespace
{
void place(BotHarness &h, NodeNum n, double lat, double lon)
{
    meshtastic_PositionLite p = meshtastic_PositionLite_init_zero;
    p.latitude_i = (int32_t)lround(lat * 1e7);
    p.longitude_i = (int32_t)lround(lon * 1e7);
    h.db->nodePositions[n] = p;
}

void hear(NodeNum n)
{
    Time::advanceTestMillis(kTxGapMs);
    auto tel = makeTelemetryFrom(n);
    bot->handleReceived(tel);
    bot->runOnce();
}
} // namespace

static void test_notify_movedFromItsSpot()
{
    BotHarness h;
    configureAdminKeys();
    place(h, kPeerNode, 51.5074, -0.1278);
    sendAdmin(0x00000031, "notify me when @!433a1b2c moved 500m");
    TEST_ASSERT_EQUAL_STRING(kQueuedReaction, lastReply(h.rtr));
    const size_t sent = h.rtr->sent.size();
    place(h, kPeerNode, 51.5104, -0.1278); // ~334 m north
    hear(kPeerNode);
    TEST_ASSERT_EQUAL_UINT(sent, h.rtr->sent.size());
    place(h, kPeerNode, 51.5134, -0.1278); // ~667 m
    hear(kPeerNode);
    TEST_ASSERT_EQUAL_STRING_LEN("!433a1b2c moved 6", lastReply(h.rtr), 17);
    TEST_ASSERT_EQUAL_STRING("m", lastReply(h.rtr) + strlen(lastReply(h.rtr)) - 1);

    // No position yet: accepted, and the first position seen becomes the spot.
    h.db->nodePositions.erase(kPeerNode);
    Time::advanceTestMillis(kTxGapMs);
    sendAdmin(0x00000032, "notify me when @!433a1b2c moved 500m");
    TEST_ASSERT_EQUAL_STRING("ok, but no position from !433a1b2c yet", lastReply(h.rtr));
    const size_t before = h.rtr->sent.size();
    place(h, kPeerNode, 51.5104, -0.1278);
    hear(kPeerNode);
    place(h, kPeerNode, 51.5134, -0.1278); // ~334 m from that first fix
    hear(kPeerNode);
    TEST_ASSERT_EQUAL_UINT(before, h.rtr->sent.size());
    place(h, kPeerNode, 51.5164, -0.1278);
    hear(kPeerNode);
    TEST_ASSERT_EQUAL_STRING_LEN("!433a1b2c moved 6", lastReply(h.rtr), 17);
}

static void test_notify_nearAnotherNode()
{
    BotHarness h;
    configureAdminKeys();
    constexpr NodeNum kOther = 0x0000beef;
    place(h, kPeerNode, 51.5074, -0.1278);
    place(h, kOther, 51.5164, -0.1278); // ~1 km apart
    sendAdmin(0x00000031, "notify me when @!433a1b2c near @!0000beef 300m");
    TEST_ASSERT_EQUAL_STRING(kQueuedReaction, lastReply(h.rtr));
    auto fromOther = makeTelemetryFrom(kOther);
    TEST_ASSERT_TRUE(bot->wantPacket(&fromOther)); // both nodes of a near rule are watched
    size_t sent = h.rtr->sent.size();
    hear(kPeerNode);
    TEST_ASSERT_EQUAL_UINT(sent, h.rtr->sent.size());
    place(h, kOther, 51.5092, -0.1278); // ~200 m
    hear(kOther);
    TEST_ASSERT_EQUAL_STRING_LEN("!433a1b2c near !0000beef (2", lastReply(h.rtr), 27);

    // A repeating copy re-arms only once they are 1.5x the distance apart, and 15 minutes on.
    Time::advanceTestMillis(kTxGapMs);
    sendAdmin(0x00000032, "every notify me when @!433a1b2c near @!0000beef 300m");
    hear(kOther);
    TEST_ASSERT_EQUAL_STRING_LEN("!433a1b2c near !0000beef (2", lastReply(h.rtr), 27);
    sent = h.rtr->sent.size();
    const double steps[] = {51.5110, 51.5092, 51.5119, 51.5092}; // 400 m, 200 m, 500 m (re-arms), 200 m
    for (double lat : steps) {
        place(h, kOther, lat, -0.1278);
        hear(kOther);
    }
    TEST_ASSERT_EQUAL_UINT(sent, h.rtr->sent.size());
    Time::advanceTestMillis(15u * 60 * 1000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(sent + 1, h.rtr->sent.size());

    Time::advanceTestMillis(kTxGapMs);
    h.db->nodePositions.erase(kOther);
    sendAdmin(0x00000033, "notify me when @!433a1b2c near @!0000beef 300m");
    TEST_ASSERT_EQUAL_STRING("ok, but no position from !0000beef yet", lastReply(h.rtr));
}

// The longest reminder is as long as a one-shot rule lives; it must fire rather than expire.
static void test_notify_weekLongReminderFires()
{
    BotHarness h;
    configureAdminKeys();
    sendAdmin(0x00000031, "notify me in 7d: renew");
    Time::advanceTestMillis(7u * 24 * 3600 * 1000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_STRING("reminder: renew", lastReply(h.rtr));
}

// A week of silence is as long as a one-shot rule lives; it must fire rather than expire.
static void test_notify_weekLongSilenceFires()
{
    BotHarness h;
    configureAdminKeys();
    h.db->node.last_heard = getTime();
    sendAdmin(0x00000031, "notify me when @!433a1b2c silent 7d");
    Time::advanceTestMillis(7u * 24 * 3600 * 1000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_STRING("!433a1b2c silent 7d", lastReply(h.rtr));
}

// every at follows the local clock, so a DST change or a clock correction moves it with the clock
// instead of leaving it an hour out until the next reboot.
static void test_notify_everyAtFollowsTheClock()
{
    BotHarness h;
    configureAdminKeys();
    struct timeval noon = {(time(NULL) / 86400 + 1) * 86400 + 12 * 3600, 0};
    setBootRelativeTimeForUnitTest(100);
    TEST_ASSERT_EQUAL_INT(RTCSetResultSuccess, perhapsSetRTC(RTCQualityNTP, &noon));
    sendAdmin(0x00000031, "every notify me at 12:30: tea");
    Time::advanceTestMillis(30u * 60 * 1000 + 1000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_STRING("reminder: tea", lastReply(h.rtr));

    // The clock moves an hour ahead: the next 12:30 is 23 hours after the one after this.
    struct timeval ahead = {(time_t)getValidTime(RTCQualityDevice) + 3600, 0};
    setBootRelativeTimeForUnitTest(100);
    TEST_ASSERT_EQUAL_INT(RTCSetResultSuccess, perhapsSetRTC(RTCQualityNTP, &ahead));
    Time::advanceTestMillis(24u * 3600 * 1000);
    bot->runOnce();
    size_t sent = h.rtr->sent.size();
    TEST_ASSERT_EQUAL_UINT(3, sent);
    Time::advanceTestMillis(23u * 3600 * 1000 - 10000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(sent, h.rtr->sent.size());
    Time::advanceTestMillis(20000);
    bot->runOnce();
    TEST_ASSERT_EQUAL_UINT(sent + 1, h.rtr->sent.size());

    struct timeval tv = {time(NULL), 0};
    setBootRelativeTimeForUnitTest(100);
    perhapsSetRTC(RTCQualityNTP, &tv);
}

// "battery < 100%" is "off mains power": 101, back on power, has to re-arm it even though it is
// not 5% above the threshold.
static void test_notify_everyBatteryNearFullReArms()
{
    BotHarness h;
    configureAdminKeys();
    sendAdmin(0x00000031, "every notify me when @!433a1b2c battery < 100%");
    reportBattery(h, 90);
    TEST_ASSERT_EQUAL_STRING("!433a1b2c battery 90%", lastReply(h.rtr));
    const size_t sent = h.rtr->sent.size();
    reportBattery(h, 101);
    Time::advanceTestMillis(15u * 60 * 1000);
    reportBattery(h, 90);
    TEST_ASSERT_EQUAL_UINT(sent + 1, h.rtr->sent.size());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_parse_bareCommand);
    RUN_TEST(test_parse_leadingWhitespace);
    RUN_TEST(test_parse_argsTail);
    RUN_TEST(test_parse_notACommand);
    RUN_TEST(test_parse_unknownCommand);
    RUN_TEST(test_cmdPing);
    RUN_TEST(test_cmdEcho);
    RUN_TEST(test_cmdHelp_listsCommands);
    RUN_TEST(test_cmdHelp_all_listsActions);
    RUN_TEST(test_cmdVer_prefix);
    RUN_TEST(test_cmdFlip);
    RUN_TEST(test_cmdRoll_default);
    RUN_TEST(test_cmdRoll_customSides);
    RUN_TEST(test_cmd8ball_nonEmpty);
    RUN_TEST(test_cmdJoke_nonEmpty);
    RUN_TEST(test_cmdSignal_directReportsRssi);
    RUN_TEST(test_cmdSignal_relayedNamesRelay);
    RUN_TEST(test_cmdSignal_unknownHopsIsNotNegative);
    RUN_TEST(test_dumpAllCommands);
    RUN_TEST(test_actionCommands);
    RUN_TEST(test_isPrivilegedSenderAuthorized);
    RUN_TEST(test_handleReceived_privilegedRequiresAdminKey);
    RUN_TEST(test_parseNodeId_table);
    RUN_TEST(test_parse_mentionAddressing);
    RUN_TEST(test_requireNodeId_rejectsUnsafeTargets);
    RUN_TEST(test_resolveLookupTarget_policy);
    RUN_TEST(test_handleReceived_bareLookupIsTheSender);
    RUN_TEST(test_handleReceived_peerLookupNeedsAdminKey);
    RUN_TEST(test_handleReceived_adminPeerLookupIsAllowed);
    RUN_TEST(test_handleReceived_broadcastCannotReachPrivilege);
    RUN_TEST(test_handleReceived_mentionOnAnyChannel);
    RUN_TEST(test_replies_areThreadedToTheCommand);
    RUN_TEST(test_cmdStats_reportsWhatExists);
    RUN_TEST(test_sunRiseSet_knownDays);
    RUN_TEST(test_clockDayNoonUtc_isTheCalendarDay);
    RUN_TEST(test_moonAgeDays_knownPhases);
    RUN_TEST(test_cmdSun_needsPositionAndTime);
    RUN_TEST(test_poll_openVoteClose);
    RUN_TEST(test_vote_afterDeadlineBeforeCloseIsRefused);
    RUN_TEST(test_poll_gateAndLimits);
    RUN_TEST(test_poll_refusesTextTooLongToKeep);
    RUN_TEST(test_notify_parseHelpers);
    RUN_TEST(test_notify_createsAcksAndRefuses);
    RUN_TEST(test_rules_listPageAndDelete);
    RUN_TEST(test_notify_heardDeliversLikeTell);
    RUN_TEST(test_notify_alertWaitsForTheTxGap);
    RUN_TEST(test_notify_expiresAfterAWeek);
    RUN_TEST(test_notify_heardToOwnerAndChannel);
    RUN_TEST(test_notify_everyHeardFloor);
    RUN_TEST(test_notify_inAndAt);
    RUN_TEST(test_notify_weekLongReminderFires);
    RUN_TEST(test_notify_weekLongSilenceFires);
    RUN_TEST(test_notify_everyAtFollowsTheClock);
    RUN_TEST(test_notify_everyBatteryNearFullReArms);
    RUN_TEST(test_notify_batteryCrossingAndMargin);
    RUN_TEST(test_notify_alreadyTrueFiresAtOnce);
    RUN_TEST(test_notify_silent);
    RUN_TEST(test_notify_snrAndHops);
    RUN_TEST(test_notify_movedFromItsSpot);
    RUN_TEST(test_notify_nearAnotherNode);
    RUN_TEST(test_cmdAckPing_reportsWhatTheAckProves);
    RUN_TEST(test_pendingRequestTimesOut);
    RUN_TEST(test_pendingSlotIsExclusiveAndNotSatisfiedByStaleReplies);
    RUN_TEST(test_txBackstop_boundsTheBotNotJustTheSender);
    RUN_TEST(test_sendFailureIsNotReportedAsSuccess);
    exit(UNITY_END());
}

void loop() {}

#else // MESHTASTIC_EXCLUDE_MESHBOT

void setup() {}
void loop() {}

#endif
