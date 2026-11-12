// Data sets for the T5 screen renders. Canonical = the design mockups' sample identities; messy = the
// worst real-world input each field can hold (limits, scripts the font lacks, extremes, long lists).
#ifdef T5_SCREENS_HOST
#include "Fixtures.h"

#include "BluetoothStatus.h"
#include "GPSStatus.h"
#include "MeshService.h"
#include "MessageStore.h"
#include "NodeDB.h"
#include "PowerStatus.h"
#include "gps/RTC.h"
#include "mesh/Channels.h"
#include "mesh/MeshModule.h"
#include "mesh/MeshRadio.h"
#include "mesh/generated/meshtastic/telemetry.pb.h"

#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

static constexpr NodeNum OUR_NUM = 0xa4f2c1b0;
static constexpr double OUR_LAT = 45.4012, OUR_LON = -122.2290; // Ridge trailhead
static uint32_t nowEpoch = 0;
static uint32_t nextPacketId = 0x1000;

uint32_t fixtureNow()
{
    return nowEpoch;
}

// Portduino keeps the host's clock, so the data is anchored to it rather than to the mockups' 14:32
static void setClock()
{
    nowEpoch = getTime();
}

static void offset(double meters, double bearingDeg, int32_t &latI, int32_t &lonI)
{
    const double R = 6371000.0, b = bearingDeg * M_PI / 180.0, d = meters / R;
    const double la1 = OUR_LAT * M_PI / 180.0, lo1 = OUR_LON * M_PI / 180.0;
    const double la2 = asin(sin(la1) * cos(d) + cos(la1) * sin(d) * cos(b));
    const double lo2 = lo1 + atan2(sin(b) * sin(d) * cos(la1), cos(d) - sin(la1) * sin(la2));
    latI = (int32_t)lround(la2 * 180.0 / M_PI * 1e7);
    lonI = (int32_t)lround(lo2 * 180.0 / M_PI * 1e7);
}

void fixtureNode(const FixtureNode &n)
{
    if (n.longName) {
        meshtastic_User u = meshtastic_User_init_default;
        snprintf(u.id, sizeof(u.id), "!%08x", n.num);
        strncpy(u.long_name, n.longName, sizeof(u.long_name) - 1);
        strncpy(u.short_name, n.shortName ? n.shortName : "", sizeof(u.short_name) - 1);
        u.hw_model = meshtastic_HardwareModel_T_ECHO;
        nodeDB->updateUser(n.num, u, n.channel);
    }
    if (n.meters >= 0) {
        meshtastic_Position p = meshtastic_Position_init_default;
        offset(n.meters, n.bearing, p.latitude_i, p.longitude_i);
        p.has_latitude_i = p.has_longitude_i = true;
        p.time = nowEpoch - n.agoSecs;
        p.precision_bits = n.precisionBits;
        nodeDB->updatePosition(n.num, p);
    }
    if (n.battery >= 0) {
        meshtastic_Telemetry t = meshtastic_Telemetry_init_default;
        t.which_variant = meshtastic_Telemetry_device_metrics_tag;
        t.variant.device_metrics.has_battery_level = true;
        t.variant.device_metrics.battery_level = n.battery;
        t.variant.device_metrics.has_voltage = true;
        t.variant.device_metrics.voltage = n.battery > 100 ? 4.2f : 3.3f + n.battery / 100.0f * 0.9f;
        nodeDB->updateTelemetry(n.num, t);
    }
    meshtastic_NodeInfoLite *node = nodeDB->getOrCreateMeshNode(n.num);
    node->last_heard = nowEpoch - n.agoSecs;
    node->snr = n.snr;
    node->has_hops_away = n.hops >= 0;
    node->hops_away = n.hops >= 0 ? n.hops : 0;
    node->channel = n.channel;
    if (n.favorite)
        nodeDB->set_favorite(true, n.num);
}

// Through MeshModule::callModules, as if from the radio: DMs reach MessageStore via TextMessageModule's
// event, channel text via T5MessageModel, and both count as unread
void fixtureText(NodeNum from, NodeNum to, uint8_t channel, uint32_t agoSecs, const char *text)
{
    meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_default;
    mp.from = from;
    mp.to = to;
    mp.channel = channel;
    mp.id = nextPacketId++;
    mp.rx_time = nowEpoch - agoSecs;
    mp.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    mp.decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    size_t len = std::min(strlen(text), sizeof(mp.decoded.payload.bytes));
    memcpy(mp.decoded.payload.bytes, text, len);
    mp.decoded.payload.size = len;
    MeshModule::callModules(mp, RX_SRC_RADIO);
}

static void baseConfig(const char *ownerLong, const char *ownerShort, bool usbCharging)
{
    myNodeInfo.my_node_num = OUR_NUM;
    config.lora.region = meshtastic_Config_LoRaConfig_RegionCode_US;
    config.lora.use_preset = true;
    config.lora.modem_preset = meshtastic_Config_LoRaConfig_ModemPreset_LONG_FAST;
    config.display.units = meshtastic_Config_DisplayConfig_DisplayUnits_METRIC;
    config.position.gps_mode = meshtastic_Config_PositionConfig_GpsMode_ENABLED;

    meshtastic_User u = meshtastic_User_init_default;
    snprintf(u.id, sizeof(u.id), "!%08x", OUR_NUM);
    strncpy(u.long_name, ownerLong, sizeof(u.long_name) - 1);
    strncpy(u.short_name, ownerShort, sizeof(u.short_name) - 1);
    owner = u;
    nodeDB->updateUser(OUR_NUM, u, 0);

    meshtastic_Position p = meshtastic_Position_init_default;
    p.latitude_i = (int32_t)lround(OUR_LAT * 1e7);
    p.longitude_i = (int32_t)lround(OUR_LON * 1e7);
    p.has_latitude_i = p.has_longitude_i = true;
    p.time = nowEpoch;
    p.sats_in_view = 9;
    localPosition = p;
    nodeDB->updatePosition(OUR_NUM, p, RX_SRC_LOCAL);

    // Nothing on the host feeds these, so they are set the way the radio, GPS, BLE and power code would
    initRegion();
    meshtastic::GPSStatus gps(true, true, false, p);
    gpsStatus->updateStatus(&gps);
    meshtastic::BluetoothStatus bt(meshtastic::BluetoothStatus::ConnectionState::CONNECTED);
    bluetoothStatus->updateStatus(&bt);
    meshtastic::PowerStatus power(meshtastic::OptTrue, usbCharging ? meshtastic::OptTrue : meshtastic::OptFalse,
                                  usbCharging ? meshtastic::OptTrue : meshtastic::OptFalse, usbCharging ? 4190 : 4050,
                                  usbCharging ? 100 : 87);
    powerStatus->updateStatus(&power);
}

static void channels2(const char *ch1Name)
{
    channels.initDefaults();
    meshtastic_Channel &c1 = channels.getByIndex(1);
    c1.role = meshtastic_Channel_Role_SECONDARY;
    c1.has_settings = true;
    strncpy(c1.settings.name, ch1Name, sizeof(c1.settings.name) - 1);
    c1.settings.psk.size = 16;
    memset(c1.settings.psk.bytes, 0x5a, 16);
    channels.onConfigChanged();
}

static void resetData()
{
    nodeDB->resetNodes();
    messageStore.clearAllMessages();
}

void seedCanonical()
{
    setClock();
    resetData();
    baseConfig("Ridge Hiker", "HIKR", false);
    channels2("Ridge Team");

    const FixtureNode nodes[] = {
        {0x7c19ab40, "Kelly Butte Relay", "KB7X", 2100, 42, 6.25f, 0, 3 * 60, 92, true},
        {0x5d21e0a7, "Sandy Ridge", "N7QX", 960, 318, 9.5f, 0, 60, 71, true},
        {0x3b8e0f12, "Ridge Relay", "RDGE", 6400, 95, -3.5f, 3, 4 * 60, 64, true},
        {0x91ac44d2, "Nora\xE2\x80\x99s T-Echo", "NORA", 1800, 210, 4.0f, 0, 2 * 60, 88, true},
        {0x2e7f6c15, "Hilltop Router", "HTOP", 3200, 160, 6.3f, 0, 2 * 60, 101, false},
        {0x6a0d93be, "Cascade Base", "CSCD", 11800, 20, 1.5f, 2, 6 * 60, 55, false},
        {0x48f2b7c3, "Trailhead 4", "TRL4", 2500, 250, 3.0f, 1, 18 * 60, 47, false},
        {0x0b7f7f3a, "Meshtastic 7f3a", "7f3a", -1, 0, -7.0f, 4, 41 * 60, -1, false},
        {0xc0ffee21, "Valley Repeater", "VLLY", 15400, 280, -1.0f, 2, 3600, 99, false},
        {0x77aa1001, "Sam K", "SAMK", -1, 0, -12.0f, -1, 3 * 3600, -1, false},
        {0x12345f00, "Ferry Dock", "FRRY", 22000, 300, -9.0f, 3, 5 * 3600, 30, false},
        {0x4e7f0d01, "Rhododendron Community Emergency Response Relay", "W7PD", 18300, 70, 2.0f, 2, 25 * 60, 77, false},
        {0x9a01c0de, "Weather Station Summit", "WX01", 5200, 30, -4.0f, 3, 3 * 3600 + 120, 100, false},
        {0x3fa91c22, nullptr, nullptr, -1, 0, -2.0f, 1, 9 * 60, -1, false}, // Unknown: shows as 1c22
    };
    for (const FixtureNode &n : nodes)
        fixtureNode(n);

    fixtureText(0x7c19ab40, OUR_NUM, 0, 33 * 60, "Just a weather check from the top if you can");
    fixtureText(0x7c19ab40, OUR_NUM, 0, 3 * 60, "Clear above 1200 m, wind picking up from the west. Heading down by 4.");
    fixtureText(0x5d21e0a7, NODENUM_BROADCAST, 0, 34 * 60, "Road is clear past the gate");
    fixtureText(0x4e7f0d01, NODENUM_BROADCAST, 0, 5 * 3600,
                "Road closure: Hwy 26 eastbound closed at milepost 48 after a rockfall. Expect delays until 18:00.");
    fixtureText(0x3fa91c22, NODENUM_BROADCAST, 0, 12 * 60, "anyone on the north trail?");
    fixtureText(0x91ac44d2, NODENUM_BROADCAST, 1, 8 * 60, "Meet at the lower lot at 3, bring the spare radio");
    fixtureText(0x3b8e0f12, NODENUM_BROADCAST, 1, 4 * 60, "Copy. Relay is up on the ridge.");
}

void seedMessy()
{
    setClock();
    resetData();
    // Owner at the protocol limits: 39-byte long name, 4-byte short name
    baseConfig("Wwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwww", "WWWW", true);
    channels2("Ομάδα Διάσωσης Βουνού"); // Greek, longer than 11 chars, multi-byte

    std::vector<FixtureNode> nodes = {
        {0x10000001, "Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm", "MMMM", 12345000, 135, -20.5f, 7, 0, 0, true},
        {0x10000002, "Αθηνά Ορεινή Ομάδα Διάσωσης Ολύμπου 2", "ΑΘΗΝ", 999, 359, 12.75f, 0, 10, 101, true},
        {0x10000003, "Base \xF0\x9F\x93\xA1 Camp \xF0\x9F\x8F\x94", "\xF0\x9F\x93\xA1", 48, 1, 0.0f, 0, 5, 100, true},
        {0x10000004, "\xE5\xB1\xB1\xE9\xA1\xB6\xE4\xB8\xAD\xE7\xBB\xA7\xE7\xAB\x99", "\xE5\xB1\xB1", 4500, 180, -1.25f, 1,
         3600 * 23, 15, false},
        {0x10000005, "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", "WWWW", 999999, 270, 3.0f, 5, 3600 * 24 * 6, 50, false},
        {0x10000006, nullptr, nullptr, -1, 0, -15.0f, -1, 3600 * 24 * 30, -1, false},
    };
    char names[64][40];
    for (int i = 0; i < 54; i++) {
        snprintf(names[i], sizeof(names[i]), "Overflow Node %02d With A Long Name", i);
        FixtureNode n = {0x20000000u + (uint32_t)i,
                         names[i],
                         nullptr,
                         (int32_t)(100 + i * 731),
                         (float)(i * 37 % 360),
                         (float)(i % 25) - 12,
                         (int8_t)(i % 8),
                         (uint32_t)(i * 97 + 30),
                         (int16_t)(i % 102),
                         false};
        nodes.push_back(n);
    }
    for (const FixtureNode &n : nodes)
        fixtureNode(n);

    const char *nospace = "Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                          "Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char *spaced = "Wide words WWWWW MMMMM WWWWW MMMMM on every line so the wrap has the worst case to handle here, and "
                         "it keeps going until it reaches the frozen two hundred byte composer limit for one message ok.";
    fixtureText(0x10000001, OUR_NUM, 0, 30, nospace);
    fixtureText(0x10000001, OUR_NUM, 0, 20, spaced);
    fixtureText(0x10000002, OUR_NUM, 0, 15, "Καλησπέρα! Ο δρόμος είναι κλειστός μετά το καταφύγιο.");
    fixtureText(0x10000003, OUR_NUM, 0, 10, "\xF0\x9F\x91\x8D\xF0\x9F\x91\x8D on my way \xF0\x9F\x9A\xB6");
    fixtureText(0x10000006, NODENUM_BROADCAST, 0, 40, spaced);
    for (int i = 0; i < 50; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Channel message number %d in a long busy thread", i);
        fixtureText(0x20000000u + (uint32_t)(i % 54), NODENUM_BROADCAST, i % 2, 3600 - i * 60, buf);
    }
    for (int i = 0; i < 30; i++)
        fixtureText(0x20000000u + (uint32_t)i, OUR_NUM, 0, 7200 - i * 120, "DM from an overflow node");
    // The widest tags' DMs arrive last, so they are the newest conversations rather than past the list's cap
    fixtureText(0x10000005, OUR_NUM, 0, 2, "Widest tag there is, and a preview long enough to reach the badge");
    fixtureText(0x10000001, OUR_NUM, 0, 1, "Another wide tag with a preview that runs all the way across the row");
}
#endif
