// Renders the T5 InkHUD screens on the host and saves each frame, for checking against the design mockups.
// Frames go to $T5_SHOT_DIR (default ./t5shots).
#include "TestUtil.h"
#include "configuration.h" // Arduino's extern "C" setup() / loop(), which both branches define
#include <unity.h>

#ifdef T5_SCREENS_HOST

#include "BluetoothStatus.h"
#include "DrawLog.h"
#include "Fixtures.h"
#include "GPSStatus.h"
#include "MeshService.h"
#include "MessageStore.h"
#include "NodeDB.h"
#include "PowerStatus.h"
#include "ReliableRouter.h"
#include "ScreenCapture.h"
#include "T5Applet.h"
#include "T5AppsApplet.h"
#include "T5ComposeTask.h"
#include "T5ConversationApplet.h"
#include "T5MapApplet.h"
#include "T5MenuApplet.h"
#include "T5MenuTask.h"
#include "T5MessageModel.h"
#include "T5MessagesApplet.h"
#include "T5NodeDetailApplet.h"
#include "T5NodesApplet.h"
#include "T5SystemLayer.h"
#include "T5TracePickTask.h"
#include "airtime.h"
#include "graphics/niche/InkHUD/InkHUD.h"
#include "graphics/niche/InkHUD/SystemApplet.h"
#include "mesh/mesh-pb-constants.h"
#include "modules/Modules.h"

#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unity.h>

void setupNicheGraphics();

using namespace NicheGraphics;

static std::string shotDir()
{
    const char *d = getenv("T5_SHOT_DIR");
    std::string dir = d ? d : "t5shots";
    mkdir(dir.c_str(), 0755);
    return dir;
}

static void renderNow()
{
    InkHUD::InkHUD::getInstance()->forceUpdate(Drivers::EInk::UpdateTypes::FULL, true, false);
}

// The boot logo holds the screen for 8 s on the device; the harness never waits that long
static void dismissBootScreens()
{
    if (InkHUD::SystemApplet *logo = InkHUD::InkHUD::getInstance()->getSystemApplet("Logo"))
        logo->sendToBackground();
}

// The new-message banner stays up until dismissed; tap DISMISS, as a user would
static void dismissBanner()
{
    InkHUD::T5SystemLayer *layer = InkHUD::T5SystemLayer::get();
    for (int i = 0; layer && layer->coversContent() && i < 4; i++) {
        // Its taps arm only after the frame that drew it, plus 300 ms with no finger down
        renderNow();
        layer->runOnce();
        testDelay(350);
        const InkHUD::T5SystemLayer::Geometry &g = layer->geometry();
        InkHUD::InkHUD::getInstance()->touchTap(g.dismissLeft + g.dismissW / 2, g.actionTop + g.actionH / 2);
    }
}

// Render the current state and save <name>-<carry|console>.pbm plus its draw log
static void snap(const std::string &name)
{
    dismissBootScreens();
    drawLogStart();
    renderNow();
    drawLogStop();
    std::string base = shotDir() + "/" + name + (t5CurrentMode() == T5Mode::CARRY ? "-carry" : "-console");
    TEST_ASSERT_TRUE(captureSavePbm((base + ".pbm").c_str()));
    TEST_ASSERT_TRUE(drawLogDump((base + ".json").c_str()));
}

static bool logHasText(const char *needle)
{
    for (const DrawRec &r : drawLog())
        if (r.kind == DrawRec::TEXT && r.text.find(needle) != std::string::npos)
            return true;
    return false;
}

void setUp() {}
void tearDown() {}

static const char *dataset = "canonical";

static void show(const char *applet)
{
    int8_t i = InkHUD::T5Applet::indexOf(applet);
    TEST_ASSERT_GREATER_OR_EQUAL_INT8(0, i);
    InkHUD::InkHUD::getInstance()->showApplet(i, Drivers::EInk::UpdateTypes::FULL, true);
}

// One state in both modes, named <state>-<dataset>-<mode>
static void both(const char *state, void (*enter)(), bool keepBanner = false)
{
    for (T5Mode mode : {T5Mode::CARRY, T5Mode::CONSOLE}) {
        t5SetMode(mode);
        enter();
        if (!keepBanner)
            dismissBanner();
        snap(std::string(state) + "-" + dataset);
    }
}

static void test_home_renders_in_both_modes()
{
    for (T5Mode mode : {T5Mode::CARRY, T5Mode::CONSOLE}) {
        t5SetMode(mode);
        show("Home");
        uint32_t before = captureFrameCount();
        snap("smoke-home");
        TEST_ASSERT_GREATER_THAN_UINT32(before, captureFrameCount());
        TEST_ASSERT_GREATER_THAN_UINT32(1000, captureBlackPixels());
        TEST_ASSERT_TRUE(logHasText("HOME"));
    }
}

using Convo = InkHUD::T5MessageModel::Convo;
static constexpr NodeNum KB7X = 0x7c19ab40, UNKNOWN_1C22 = 0x3fa91c22, SAMK = 0x77aa1001, WX01 = 0x9a01c0de;
static constexpr NodeNum MESSY_LONG = 0x10000001, MESSY_GREEK = 0x10000002, OUR_NUM_FIXTURE = 0xa4f2c1b0;
static constexpr NodeNum W7PD = 0x4e7f0d01;

// Test-only access to the system layer's power-check clock: the watcher samples once a minute and needs two
// samples in a row, which the harness should not wait for. Explicit instantiation is exempt from access checks
struct PowerCheckField {
    using type = uint32_t InkHUD::T5SystemLayer::*;
    friend type get(PowerCheckField);
};
template <typename Tag, typename Tag::type M> struct RobT5 {
    friend typename Tag::type get(Tag) { return M; }
};
template struct RobT5<PowerCheckField, &InkHUD::T5SystemLayer::lastPowerCheckMs>;

// One power sample, as if the last one were a minute old
static void powerSample(const meshtastic::PowerStatus &status)
{
    powerStatus->updateStatus(const_cast<meshtastic::PowerStatus *>(&status));
    InkHUD::T5SystemLayer *layer = InkHUD::T5SystemLayer::get();
    layer->*get(PowerCheckField()) = millis() - 61000;
    layer->runOnce();
}

// A radio that takes every packet and says nothing back, so a send stays SENDING until a reply settles it
class QuietRadio : public RadioInterface
{
  public:
    ErrorCode send(meshtastic_MeshPacket *p) override
    {
        packetPool.release(p);
        return ERRNO_OK;
    }
    uint32_t getPacketTime(uint32_t totalPacketLen, bool received = false) override
    {
        (void)totalPacketLen;
        (void)received;
        return 0;
    }
};

// A routing reply to one of our sends, delivered as the radio would
static void routingReply(uint32_t requestId, NodeNum from, meshtastic_Routing_Error error)
{
    static uint32_t nextId = 0x7000;
    meshtastic_Routing r = meshtastic_Routing_init_default;
    r.which_variant = meshtastic_Routing_error_reason_tag;
    r.error_reason = error;
    meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_default;
    mp.id = nextId++;
    mp.from = from;
    mp.to = OUR_NUM_FIXTURE;
    mp.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    mp.decoded.portnum = meshtastic_PortNum_ROUTING_APP;
    mp.decoded.request_id = requestId;
    mp.decoded.payload.size =
        pb_encode_to_bytes(mp.decoded.payload.bytes, sizeof(mp.decoded.payload.bytes), meshtastic_Routing_fields, &r);
    MeshModule::callModules(mp, RX_SRC_RADIO);
}

// Close whatever task layer the previous state left open, one layer at a time like the Home key
static void resetUi()
{
    if (InkHUD::T5TracePickTask::isOpen())
        InkHUD::T5TracePickTask::close();
    // The Home key's path: each press cancels one layer
    for (int i = 0; i < 6 && InkHUD::T5MenuTask::isOpen(); i++)
        InkHUD::T5MenuTask::get()->onExitShort();
    for (int i = 0; i < 6 && InkHUD::T5ComposeTask::taskOpen(); i++)
        InkHUD::T5ComposeTask::get()->onExitShort();
    dismissBanner();
}

// Tap the centre of the first drawn text run equal to (or else containing) label, as a finger would
static bool tapLabel(const char *label)
{
    dismissBootScreens();
    drawLogStart();
    renderNow();
    drawLogStop();
    const DrawRec *hit = nullptr;
    for (const DrawRec &r : drawLog()) {
        if (r.kind != DrawRec::TEXT)
            continue;
        if (r.text == label) {
            hit = &r;
            break;
        }
        if (!hit && r.text.find(label) != std::string::npos)
            hit = &r;
    }
    if (!hit)
        return false;
    InkHUD::InkHUD::getInstance()->touchTap(hit->x + hit->w / 2, hit->y + hit->h / 2);
    return true;
}

static void menuPage(const char *section)
{
    resetUi();
    InkHUD::T5MenuApplet::open();
    tapLabel(section);
}

static void appsTool(const char *tool)
{
    resetUi();
    show("Apps");
    tapLabel(tool);
}

static void renderCoreScreens()
{
    // A message in a script the font has (Greek) must reach the banner decoded, not as raw UTF-8
    both(
        "banner-greek",
        [] {
            show("Home");
            fixtureText(MESSY_GREEK, OUR_NUM_FIXTURE, 0, 0, "Καλησπέρα! Ο δρόμος είναι κλειστός μετά το καταφύγιο.");
        },
        true);
    both("banner-home", [] { show("Home"); }, true);
    both("home", [] { show("Home"); });
    both("nodes", [] { show("Nodes"); });
    both("nodes-p2", [] {
        show("Nodes");
        renderNow(); // A mode change takes effect at the next render; paging needs that mode's rows per page
        InkHUD::T5NodesApplet::pageDown();
    });
    both("nodedetail", [] { InkHUD::T5NodeDetailApplet::open(KB7X); });
    both("nodedetail-unknown", [] { InkHUD::T5NodeDetailApplet::open(UNKNOWN_1C22); });
    both("nodedetail-nopos", [] { InkHUD::T5NodeDetailApplet::open(SAMK); });
    if (std::string(dataset) == "messy") {
        both("nodedetail-long", [] { InkHUD::T5NodeDetailApplet::open(MESSY_LONG); });
        both("nodedetail-greek", [] { InkHUD::T5NodeDetailApplet::open(MESSY_GREEK); });
        both("convo-dm-long", [] { InkHUD::T5MessagesApplet::open(Convo::forPeer(MESSY_LONG)); });
    }

    both("messages", [] { InkHUD::T5MessagesApplet::open(); });
    both("messages-dm", [] { InkHUD::T5MessagesApplet::open(Convo::forPeer(KB7X)); });
    both("convo-dm", [] { InkHUD::T5MessagesApplet::open(Convo::forPeer(KB7X)); });
    both("convo-ch0", [] { InkHUD::T5MessagesApplet::open(Convo::forChannel(0)); });
    both("convo-ch1", [] { InkHUD::T5MessagesApplet::open(Convo::forChannel(1)); });
    both("compose-reply", [] {
        resetUi();
        InkHUD::T5ComposeTask::openReply(Convo::forPeer(KB7X));
    });
    both("compose-picker", [] {
        resetUi();
        InkHUD::T5ComposeTask::openPicker();
    });
    both("compose-fornode", [] {
        resetUi();
        InkHUD::T5ComposeTask::openForNode(WX01);
    });
    both(
        "compose-banner",
        [] {
            resetUi();
            InkHUD::T5ComposeTask::openReply(Convo::forChannel(0));
            fixtureText(SAMK, NODENUM_BROADCAST, 1, 0, "Arriving while you type: the system layer must stay out of the way");
        },
        true);
    resetUi();

    both("map", [] { show("Map"); });
    both("map-focus", [] { InkHUD::T5MapApplet::focusNode(KB7X); });
    if (std::string(dataset) == "canonical") {
        // W7PD from a reduced-precision position north-north-west of us: DIST gives up its precision, BEARING
        // its degrees, and the CENTER cell steps a wide short name down (Panel Fit D4, D7)
        fixtureNode(
            {W7PD, "Rhododendron Community Emergency Response Relay", "W7PD", 18300, 337, 2.0f, 2, 25 * 60, 77, false, 0, 13});
        both("map-precise", [] { InkHUD::T5MapApplet::focusNode(W7PD); });
        both("map-precise-us", [] {
            InkHUD::T5MapApplet::focusNode(W7PD);
            tapLabel("CENTER");
        });
        fixtureNode({W7PD, "Rhododendron Community Emergency Response Relay", "W7PD", 18300, 70, 2.0f, 2, 25 * 60, 77, false});
    }
    {
        // GPS on without lock, from a position we once had: the dashed ring and its LAST FIX label (Map E2)
        meshtastic::GPSStatus noLock(false, true, false, localPosition);
        gpsStatus->updateStatus(&noLock);
        both("map-lastfix", [] { show("Map"); });
        meshtastic::GPSStatus lock(true, true, false, localPosition);
        gpsStatus->updateStatus(&lock);
    }

    both("apps", [] {
        resetUi();
        show("Apps");
    });
    both("apps-trace", [] { appsTool("TRACE ROUTE"); });
    both("apps-trace-pick", [] {
        appsTool("TRACE ROUTE");
        InkHUD::T5TracePickTask::open();
    });
    both("apps-rf", [] { appsTool("RF STATUS"); });
    both("apps-sensors", [] { appsTool("SENSORS"); });
    resetUi();

    both("menu", [] {
        resetUi();
        InkHUD::T5MenuApplet::open();
    });
    for (const char *section : {"RADIO", "CHANNELS", "POSITION", "CONNECTIVITY", "DISPLAY", "POWER", "DEVICE", "ABOUT"}) {
        static const char *current;
        current = section;
        both((std::string("menu-") + section).c_str(), [] { menuPage(current); });
    }
    both("menu-region-picker", [] {
        menuPage("RADIO");
        InkHUD::T5MenuTask::openPicker(InkHUD::T5Act::PICK_REGION, 0);
    });
    both("menu-region-confirm", [] {
        menuPage("RADIO");
        InkHUD::T5MenuTask::openPicker(InkHUD::T5Act::PICK_REGION, 0);
        testDelay(350); // A task layer ignores taps for its first 300 ms (System UI G1)
        tapLabel("EU 868");
    });
    both("menu-channel1", [] {
        menuPage("CHANNELS");
        tapLabel("1 \xB7 "); // The second channel's row: C7, titled with its stored name (D12)
    });
    both("menu-pin-picker", [] {
        menuPage("CONNECTIVITY");
        InkHUD::T5MenuTask::openPicker(InkHUD::T5Act::PICK_PIN_MODE, 0);
    });
    resetUi();

    // System surfaces over Home
    both(
        "lock-asleep",
        [] {
            // Asleep freezes the last image, so Home is drawn awake first and the gate set after it
            T5UiGate::userWake();
            InkHUD::T5SystemLayer::get()->onTimeoutGateClear();
            resetUi();
            show("Home");
            dismissBanner();
            renderNow();
            T5UiGate::enterTimeout();
            InkHUD::T5SystemLayer::get()->onTimeoutGateSet();
        },
        true);
    T5UiGate::userWake();
    InkHUD::T5SystemLayer::get()->onTimeoutGateClear();
    both(
        "touch-off",
        [] {
            show("Nodes");
            setTouchInputEnabled(false, false);
            InkHUD::T5SystemLayer::get()->onTouchPreferenceChanged();
        },
        true);
    setTouchInputEnabled(true, false);
    InkHUD::T5SystemLayer::get()->onTouchPreferenceChanged();
    both(
        "pairing-card",
        [] {
            show("Home");
            meshtastic::BluetoothStatus pairing(std::string("483920"));
            bluetoothStatus->updateStatus(&pairing);
        },
        true);
    {
        meshtastic::BluetoothStatus linked(meshtastic::BluetoothStatus::ConnectionState::CONNECTED);
        bluetoothStatus->updateStatus(&linked);
    }
    both(
        "battery-low",
        [] {
            resetUi();
            show("Apps");
            for (int i = 0; i < 2; i++)
                powerSample(meshtastic::PowerStatus(meshtastic::OptTrue, meshtastic::OptFalse, meshtastic::OptFalse, 3550, 9));
        },
        true);
    both(
        "battery-critical",
        [] {
            for (int i = 0; i < 2; i++)
                powerSample(meshtastic::PowerStatus(meshtastic::OptTrue, meshtastic::OptFalse, meshtastic::OptFalse, 3350, 2));
        },
        true);
    powerSample(meshtastic::PowerStatus(meshtastic::OptTrue, meshtastic::OptTrue, meshtastic::OptTrue, 4190, 100));
    resetUi();

    if (std::string(dataset) == "canonical") {
        // Our own messages in each delivery state the host can produce, settled by routing replies as the
        // radio would send them. Channel sends: SENDING, RELAYED, NOT HEARD. A DM fails at once with KEY ERROR,
        // as the host has no PKI keys; DELIVERED needs those too, so it is not covered. Last, because these
        // change two conversations for every state after them
        InkHUD::T5MessageModel *model = InkHUD::T5MessageModel::get();
        const Convo ch0 = Convo::forChannel(0);
        const char *texts[] = {"Heading back down now", "Anyone near the lower lot?", "On the ridge in 10"};
        uint32_t ids[3] = {};
        const bool txWas = config.lora.tx_enabled;
        config.lora.tx_enabled = true; // Listen-only would have the router refuse every send at once
        router->addInterface(std::unique_ptr<RadioInterface>(new QuietRadio())); // No interface: refused as RADIO OFF
        for (int i = 0; i < 3; i++) {
            model->send(ch0, texts[i]);
            const InkHUD::T5MessageModel::Outgoing *o = model->outgoing(&messageStore.getLiveMessages().back());
            ids[i] = o ? o->packetId : 0;
        }
        routingReply(ids[0], W7PD, meshtastic_Routing_Error_NONE);                      // RELAYED: a neighbour repeated it
        routingReply(ids[1], OUR_NUM_FIXTURE, meshtastic_Routing_Error_MAX_RETRANSMIT); // NOT HEARD
        // ids[2] stays SENDING
        model->send(Convo::forPeer(KB7X), "Rain starting here"); // FAILED, KEY ERROR
        both("convo-ch0-sends", [] { InkHUD::T5MessagesApplet::open(Convo::forChannel(0)); });
        both("convo-dm-sends", [] { InkHUD::T5MessagesApplet::open(Convo::forPeer(KB7X)); });
        config.lora.tx_enabled = txWas;
        router->addInterface(nullptr);
        resetUi();
    }
    resetUi();
}

// The CARRY / CONSOLE cells are sized from their labels; a tap on the drawn CARRY label must still
// land in the CARRY cell, which only holds if hit testing uses the same widths as drawing
static void test_mode_cells_hit_what_they_draw()
{
    t5SetMode(T5Mode::CONSOLE);
    menuPage("DISPLAY");
    drawLogStart();
    renderNow();
    drawLogStop();
    const DrawRec *label = nullptr, *cell = nullptr;
    for (const DrawRec &r : drawLog())
        if (r.kind == DrawRec::TEXT && r.text == "CARRY")
            label = &r;
    TEST_ASSERT_NOT_NULL(label);
    const int16_t cx = label->x + label->w / 2, cy = label->y + label->h / 2;
    for (const DrawRec &r : drawLog())
        if ((r.kind == DrawRec::RECT || r.kind == DrawRec::FILL) && r.x <= cx && cx < r.x + r.w && r.y <= cy && cy < r.y + r.h &&
            r.w < 200)
            cell = &r;
    TEST_ASSERT_NOT_NULL(cell);
    // Just inside the cell's left edge: where a mismatch between drawn and hit-tested widths shows
    InkHUD::InkHUD::getInstance()->touchTap(cell->x + 4, cy);
    TEST_ASSERT_EQUAL(static_cast<int>(T5Mode::CARRY), static_cast<int>(t5CurrentMode()));
    resetUi();
}

// Terminal images ignore all input from then on, so they go last
// PAIRING PIN picks the pairing mode. fixed_pin is the PIN number the app chose, and must survive the change
static void test_pairing_pin_picker_writes_the_mode_not_the_pin()
{
    config.bluetooth.fixed_pin = 482913;
    config.bluetooth.mode = meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN;
    InkHUD::t5Apply(InkHUD::T5Act::PICK_PIN_MODE, 1, 0);
    TEST_ASSERT_EQUAL(meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN, config.bluetooth.mode);
    TEST_ASSERT_EQUAL_UINT32(482913, config.bluetooth.fixed_pin);
    InkHUD::t5Apply(InkHUD::T5Act::PICK_PIN_MODE, 0, 0);
    TEST_ASSERT_EQUAL(meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN, config.bluetooth.mode);
    TEST_ASSERT_EQUAL_UINT32(482913, config.bluetooth.fixed_pin);
}

static void test_terminal_screens()
{
    dataset = "canonical";
    both(
        "terminal-restarting", [] { InkHUD::T5SystemLayer::get()->showTerminal(InkHUD::T5SystemLayer::Terminal::RESTARTING); },
        true);
    both("terminal-off", [] { InkHUD::T5SystemLayer::get()->showTerminal(InkHUD::T5SystemLayer::Terminal::SWITCHED_OFF); }, true);
    both(
        "terminal-empty", [] { InkHUD::T5SystemLayer::get()->showTerminal(InkHUD::T5SystemLayer::Terminal::BATTERY_EMPTY); },
        true);
}

static void test_canonical_screens()
{
    dataset = "canonical";
    seedCanonical();
    renderCoreScreens();
}

static void test_messy_screens()
{
    dataset = "messy";
    seedMessy();
    renderCoreScreens();
}

void setup()
{
    initializeTestEnvironment();
    nodeDB = new NodeDB();
    service = new MeshService();
    router = new ReliableRouter();
    airTime = new AirTime();
    setupModules();
    setupNicheGraphics();
    UNITY_BEGIN();
    RUN_TEST(test_home_renders_in_both_modes);
    RUN_TEST(test_canonical_screens);
    RUN_TEST(test_messy_screens);
    RUN_TEST(test_mode_cells_hit_what_they_draw);
    RUN_TEST(test_pairing_pin_picker_writes_the_mode_not_the_pin);
    RUN_TEST(test_terminal_screens);
    exit(UNITY_END());
}

void loop() {}

#else // Only env:native-t5-screens builds InkHUD and the T5 sources

void setUp() {}
void tearDown() {}

// Elsewhere the suite has nothing to render: it only confirms it is not silently skipping a real build
static void test_screens_render_only_in_their_own_env()
{
#ifdef MESHTASTIC_INCLUDE_INKHUD
    TEST_FAIL_MESSAGE("InkHUD is built here, so T5_SCREENS_HOST should be too");
#endif
    TEST_PASS();
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_screens_render_only_in_their_own_env);
    exit(UNITY_END());
}

void loop() {}

#endif
