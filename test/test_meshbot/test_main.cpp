// Unit tests for MeshBotModule in src/modules/MeshBotModule.cpp - the slash-command text bot's
// command parsing and its per-command replies.
//
// The module answers "/name [args]" text messages arriving as DMs, or as broadcasts on its command
// channel. kCommands is the whole dispatch surface: each entry pairs a name with a handler and a
// privileged flag, and handleReceived reads that flag to decide whether the sender has to appear in
// MESHBOT_ALLOWED_SENDERS and which cooldown applies.
//
// The case that matters is that flag. It is the only thing standing between a stranger's text
// message and our airtime or our node database, so test_actionCommands asserts privileged == true
// for each transmitting or mutating command. Note that both it and test_dumpAllCommands spell their
// command lists out rather than deriving them from kCommands: a command added to the table is not
// covered until it is added here too, and an action command left unprivileged is reachable by
// anyone on the command channel with nothing else in the build to complain.
//
// The rest pins behavior that would fail quietly rather than loudly. A handler that leaves its
// reply buffer empty is dropped by handleReceived and the command simply appears to do nothing, so
// every case requires a non-empty reply. The transmitting handlers run with their subsystems absent
// - service, router and the module globals are null in this harness, as they are on a real device
// before the radio comes up - and have to say so rather than crash. /signal must report RSSI and
// SNR only for a 0-hop request: our radio measured the final leg, so quoting those for a relayed
// request describes a link the requester is not on, which is why the relayed case names the relay.
//
// The suite constructs a NodeDB subclass, whose constructor persists a default prefs set when the
// prefs directory is empty - hence this suite's entry in test/state-manifest.tsv.
#include "NodeDB.h"
#include "TestUtil.h"
#include "gps/RTC.h"
#include "modules/MeshBotModule.h"
#include <unity.h>
#include <vector>

#if !MESHTASTIC_EXCLUDE_MESHBOT

// Subclass to reach the module's protected parse/dispatch internals for testing.
class TestableBot : public MeshBotModule
{
  public:
    using MeshBotModule::cmd8ball;
    using MeshBotModule::cmdAckPing;
    using MeshBotModule::cmdAnnounce;
    using MeshBotModule::cmdEcho;
    using MeshBotModule::cmdFav;
    using MeshBotModule::cmdFlip;
    using MeshBotModule::cmdHelp;
    using MeshBotModule::cmdJoke;
    using MeshBotModule::cmdNode;
    using MeshBotModule::cmdNodes;
    using MeshBotModule::cmdOnline;
    using MeshBotModule::cmdPeerBatt;
    using MeshBotModule::cmdPeerUtil;
    using MeshBotModule::cmdPing;
    using MeshBotModule::cmdPos;
    using MeshBotModule::cmdReqPos;
    using MeshBotModule::cmdReqTel;
    using MeshBotModule::cmdRoll;
    using MeshBotModule::cmdSeen;
    using MeshBotModule::cmdSharePos;
    using MeshBotModule::cmdSignal;
    using MeshBotModule::cmdTrace;
    using MeshBotModule::cmdUnfav;
    using MeshBotModule::cmdUptime;
    using MeshBotModule::cmdVer;
    using MeshBotModule::cmdWhoami;
    using MeshBotModule::Command;
    using MeshBotModule::kCommands;
    using MeshBotModule::parseCommand;
};

// Minimal NodeDB stand-in so node-aware commands (/node, /pos, /nodes, /online, /whoami) can be
// exercised with deterministic data. getMeshNode is virtual; the count helpers read public members.
class MockNodeDB : public NodeDB
{
  public:
    meshtastic_NodeInfoLite node = meshtastic_NodeInfoLite_init_zero;
    std::vector<meshtastic_NodeInfoLite> nodes;
    meshtastic_NodeInfoLite *getMeshNode(NodeNum) override { return &node; }
};

static TestableBot *bot;
static meshtastic_MeshPacket pkt;

void setUp(void)
{
    bot = new TestableBot();
    memset(&pkt, 0, sizeof(pkt));
}

void tearDown(void)
{
    delete bot;
}

static void test_parse_bareCommand()
{
    const char *args = nullptr;
    auto *c = bot->parseCommand("/ping", &args);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("ping", c->name);
    TEST_ASSERT_EQUAL_STRING("", args); // no trailing args
}

static void test_parse_leadingWhitespace()
{
    const char *args = nullptr;
    auto *c = bot->parseCommand("   /help", &args);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("help", c->name);
}

static void test_parse_argsTail()
{
    const char *args = nullptr;
    auto *c = bot->parseCommand("/echo hello world", &args);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("echo", c->name);
    TEST_ASSERT_EQUAL_STRING("hello world", args);
}

static void test_parse_notACommand()
{
    const char *args = nullptr;
    TEST_ASSERT_NULL(bot->parseCommand("just a chat message", &args));
    TEST_ASSERT_NULL(bot->parseCommand("", &args));
    TEST_ASSERT_NULL(bot->parseCommand("/", &args));
}

static void test_parse_unknownCommand()
{
    const char *args = nullptr;
    TEST_ASSERT_NULL(bot->parseCommand("/bogus", &args));
    // A prefix of a real command must not match.
    TEST_ASSERT_NULL(bot->parseCommand("/pin", &args));
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
    TEST_ASSERT_NOT_NULL(strstr(out, "/ping"));
    TEST_ASSERT_NOT_NULL(strstr(out, "/echo"));
    // Public help omits privileged action commands and points at "/help all".
    TEST_ASSERT_NULL(strstr(out, "/trace"));
    TEST_ASSERT_NULL(strstr(out, "/fav"));
    TEST_ASSERT_NOT_NULL(strstr(out, "+/help all"));
}

static void test_cmdHelp_all_listsActions()
{
    char out[200] = {0};
    bot->cmdHelp(pkt, "all", out, sizeof(out));
    TEST_ASSERT_NOT_NULL(strstr(out, "/trace"));
    TEST_ASSERT_NOT_NULL(strstr(out, "/fav"));
    TEST_ASSERT_NOT_NULL(strstr(out, "/announce"));
    // The action listing excludes the public commands.
    TEST_ASSERT_NULL(strstr(out, "/ping"));
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

// /signal reports RSSI/SNR only for a directly-received packet; a relayed one names the relay,
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

    meshtastic_DeviceMetrics metrics = meshtastic_DeviceMetrics_init_zero;
    metrics.has_battery_level = true;
    metrics.battery_level = 77;
    metrics.has_voltage = true;
    metrics.voltage = 3.92f;
    metrics.has_channel_utilization = true;
    metrics.channel_utilization = 4.7f;
    metrics.has_air_util_tx = true;
    metrics.air_util_tx = 1.3f;

    meshtastic_PositionLite position = meshtastic_PositionLite_init_zero;
    position.latitude_i = (int32_t)(37.7749 * 1e7);
    position.longitude_i = (int32_t)(-122.4194 * 1e7);
    position.altitude = 15;

    // v25 keeps metrics and position in satellite maps keyed by node num, not on NodeInfoLite.
    mock->nodeTelemetry[mock->node.num] = metrics;
    mock->nodePositions[mock->node.num] = position;

    mock->node.last_heard = getTime() - 125; // ~2 minutes ago for /seen
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

    // Received packet metadata so /signal and /whoami have something to report.
    memset(&pkt, 0, sizeof(pkt));
    pkt.from = 0x433a1b2c;
    pkt.rx_rssi = -73;
    pkt.rx_snr = 6.25f;
    pkt.hop_start = 3;
    pkt.hop_limit = 1;
    pkt.relay_node = 0x2c;

    printf("\n===== MeshBot command outputs =====\n");
    static const char *const names[] = {"help", "ping", "signal", "nodes", "uptime", "echo", "online", "whoami", "ver",
                                        "node", "pos",  "seen",   "pbatt", "putil",  "roll", "flip",   "8ball",  "joke"};
    for (const char *name : names) {
        char probe[32];
        snprintf(probe, sizeof(probe), "/%s", name);
        const char *args = nullptr;
        const TestableBot::Command *c = bot->parseCommand(probe, &args);
        TEST_ASSERT_NOT_NULL_MESSAGE(c, name);
        const char *callArgs = "";
        if (strcmp(name, "echo") == 0)
            callArgs = "hello mesh";
        else if (strcmp(name, "roll") == 0)
            callArgs = "20";
        char out[200] = {0};
        (bot->*(c->handler))(pkt, callArgs, out, sizeof(out));
        printf("  /%-7s -> %s\n", name, out);
        TEST_ASSERT_TRUE_MESSAGE(out[0] != '\0', name);
    }
    printf("===================================\n");
    fflush(stdout);

    nodeDB = nullptr;
    delete mock;
}

// Exercises the action commands. The transmitting/module-backed ones (trace/reqpos/reqtel/announce/
// sharepos/ackping) run with their subsystems absent (service, router, and the module globals are
// null in the unit-test harness), so they must degrade gracefully rather than crash. The favorite
// commands run against the mock NodeDB and take real effect.
static void test_actionCommands()
{
    auto *mock = new MockNodeDB();
    mock->node.num = 0x433a1b2c;
    mock->nodes.push_back(mock->node);
    mock->meshNodes = &mock->nodes;
    mock->numMeshNodes = (pb_size_t)mock->nodes.size();
    nodeDB = mock;

    memset(&pkt, 0, sizeof(pkt));
    pkt.from = 0x433a1b2c;

    struct ActionCase {
        const char *cmd;
        const char *expect; // exact expected reply
    };
    const ActionCase cases[] = {
        {"/trace !433a1b2c", "Trace unavailable"},     {"/reqpos !433a1b2c", "Radio unavailable"},
        {"/reqtel !433a1b2c", "Radio unavailable"},    {"/announce", "NodeInfo unavailable"},
        {"/sharepos", "Position unavailable"},         {"/fav !433a1b2c", "Favorited !433a1b2c"},
        {"/unfav !433a1b2c", "Unfavorited !433a1b2c"}, {"/ackping !433a1b2c", "Radio unavailable"},
    };

    printf("\n===== MeshBot action commands (subsystems absent) =====\n");
    for (const auto &tc : cases) {
        const char *args = nullptr;
        const TestableBot::Command *c = bot->parseCommand(tc.cmd, &args);
        TEST_ASSERT_NOT_NULL_MESSAGE(c, tc.cmd);
        TEST_ASSERT_TRUE_MESSAGE(c->privileged, tc.cmd); // every action command must be gated
        char out[200] = {0};
        (bot->*(c->handler))(pkt, args, out, sizeof(out));
        printf("  %-18s -> %s\n", tc.cmd, out);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(tc.expect, out, tc.cmd);
    }
    printf("=======================================================\n");
    fflush(stdout);

    // /fav actually flipped the flag on the mock node.
    TEST_ASSERT_FALSE(nodeInfoLiteIsFavorite(&mock->node)); // /unfav ran last, clearing it

    // A missing id is reported rather than silently transmitting.
    const char *args = nullptr;
    const TestableBot::Command *c = bot->parseCommand("/reqpos", &args);
    char out[64] = {0};
    (bot->*(c->handler))(pkt, args, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Need a node id", out);

    nodeDB = nullptr;
    delete mock;
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
    exit(UNITY_END());
}

void loop() {}

#else // MESHTASTIC_EXCLUDE_MESHBOT

void setup() {}
void loop() {}

#endif
