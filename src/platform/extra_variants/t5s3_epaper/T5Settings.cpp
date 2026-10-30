#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

#include "./T5Settings.h"
#include "./T5Applet.h"
#include "./T5MessageModel.h"
#include "./T5Text.h"

#include "BluetoothStatus.h"
#include "Default.h"
#include "DisplayFormatters.h"
#include "MeshService.h"
#include "MessageStore.h"
#include "NodeDB.h"
#include "PowerStatus.h"
#include "UptimeClock.h"
#include "airtime.h"
#include "graphics/niche/InkHUD/InkHUD.h"
#include "main.h"
#include "mesh/Channels.h"
#include "mesh/MeshRadio.h"
#include "mesh/RadioInterface.h"
#include "mesh/RadioLibInterface.h"
#include <algorithm>
#include <cmath>

#if !MESHTASTIC_EXCLUDE_MQTT
#include "mqtt/MQTT.h"
#endif

#ifdef T5_DEVTOOLS
#include <esp_system.h>

namespace
{
// esp_reset_reason() in words, as the frozen page shows it ("POWER ON", Menu C15 / K4)
const char *resetReasonText(int reason)
{
    switch (reason) {
    case 1:
        return "POWER ON";
    case 2:
        return "RESET PIN";
    case 3:
        return "SOFTWARE";
    case 4:
        return "PANIC";
    case 5:
    case 6:
    case 7:
        return "WATCHDOG";
    case 8:
        return "DEEP SLEEP";
    case 9:
        return "BROWNOUT";
    default:
        return "UNKNOWN";
    }
}
} // namespace
#endif

#if defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI)
#include <SD.h>
#endif

using namespace NicheGraphics;
using std::to_string;

namespace
{

const char *const SEP = InkHUD::T5Applet::SEP;
const char *const DASH = InkHUD::T5Applet::DASH;
const char *const ELLIPSIS = InkHUD::T5Applet::ELLIPSIS;

// ---------------------------------------------------------------- lifted apply helpers
//
// The shared menu's apply* helpers are file-static in MenuApplet.cpp, so the native menu cannot call
// them and must not grow a second way to write the same field. These are those helpers, unchanged in
// behaviour, lifted to where the native rows can reach them (Menu Settings 00 last row, 14).

// Returns false when the write did not reach flash: the only signal the frozen NOT SAVED state has
bool saveAndReload(uint32_t changes = SEGMENT_CONFIG)
{
    const bool saved = nodeDB->saveToDisk(changes);
    service->reloadConfig(changes);
    return saved;
}

void scheduleRestart()
{
    InkHUD::InkHUD::getInstance()->notifyApplyingChanges();
    rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
}

void applyLoRaRegion(meshtastic_Config_LoRaConfig_RegionCode region)
{
    if (config.lora.region == region)
        return;

    config.lora.region = region;

    auto changes = SEGMENT_CONFIG;

#if !(MESHTASTIC_EXCLUDE_PKI_KEYGEN || MESHTASTIC_EXCLUDE_PKI)
    // Minting the key moves our node num with it, and the reboot below only re-derives after the save
    if (nodeDB->ensurePkiIdentity())
        changes |= SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE;
#endif

    config.lora.tx_enabled = true; // Setting a region always re-enables transmit

    initRegion();

    if (myRegion && getEffectiveDutyCycle() < 100)
        config.lora.ignore_mqtt = true;

#if !MESHTASTIC_EXCLUDE_MQTT
    if (myRegion && MQTT::applyRegionRootTopic(myRegion->name))
        changes |= SEGMENT_MODULECONFIG;
#endif

    InkHUD::InkHUD::getInstance()->notifyApplyingChanges();
    service->reloadConfig(changes);
    rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
}

void applyDeviceRole(meshtastic_Config_DeviceConfig_Role role)
{
    if (config.device.role == role)
        return;
    config.device.role = role;
    nodeDB->saveToDisk(SEGMENT_CONFIG);
    service->reloadConfig(SEGMENT_CONFIG);
    scheduleRestart();
}

void applyLoRaPreset(meshtastic_Config_LoRaConfig_ModemPreset preset)
{
    if (config.lora.modem_preset == preset)
        return;
    config.lora.use_preset = true;
    config.lora.modem_preset = preset;
    nodeDB->saveToDisk(SEGMENT_CONFIG);
    service->reloadConfig(SEGMENT_CONFIG);
    scheduleRestart();
}

// ---------------------------------------------------------------- option tables

struct Timezone {
    const char *label;
    const char *tzdef;
};

// The eighteen zones the shared menu offers, in its order, as a table rather than eighteen actions
const Timezone TIMEZONES[] = {
    {"US HAWAII", "HST10"},
    {"US ALASKA", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US PACIFIC", "PST8PDT,M3.2.0,M11.1.0"},
    {"US ARIZONA", "MST7"},
    {"US MOUNTAIN", "MST7MDT,M3.2.0,M11.1.0"},
    {"US CENTRAL", "CST6CDT,M3.2.0,M11.1.0"},
    {"US EASTERN", "EST5EDT,M3.2.0,M11.1.0"},
    {"BR BRASILIA", "BRT3"},
    {"UTC", "UTC0"},
    {"EU WESTERN", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"EU CENTRAL", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"EU EASTERN", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"ASIA KOLKATA", "IST-5:30"},
    {"ASIA HONG KONG", "HKT-8"},
    {"AU AWST", "AWST-8"},
    {"AU ACST", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"AU AEST", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"PACIFIC NZ", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
constexpr uint8_t TIMEZONE_COUNT = sizeof(TIMEZONES) / sizeof(TIMEZONES[0]);

struct SecondsOption {
    uint32_t seconds;
    const char *label;
};

// The seven existing display-timeout options. Zero is the one that genuinely disables the timeout:
// the T5 timeout owner arms no deadline for it rather than waiting out a very large number
const SecondsOption SLEEP_OPTIONS[] = {
    {0, "NEVER"}, {30, "30 SECS"}, {60, "1 MIN"}, {5 * 60, "5 MIN"}, {15 * 60, "15 MIN"}, {30 * 60, "30 MIN"}, {60 * 60, "1 HR"},
};
constexpr uint8_t SLEEP_COUNT = sizeof(SLEEP_OPTIONS) / sizeof(SLEEP_OPTIONS[0]);

const SecondsOption BROADCAST_OPTIONS[] = {
    {60, "1 MIN"},        {90, "90 SECS"},      {5 * 60, "5 MIN"},    {15 * 60, "15 MIN"},
    {60 * 60, "1 HR"},    {2 * 3600, "2 HR"},   {3 * 3600, "3 HR"},   {4 * 3600, "4 HR"},
    {5 * 3600, "5 HR"},   {6 * 3600, "6 HR"},   {12 * 3600, "12 HR"}, {18 * 3600, "18 HR"},
    {24 * 3600, "24 HR"}, {36 * 3600, "36 HR"}, {48 * 3600, "48 HR"}, {72 * 3600, "72 HR"},
};
constexpr uint8_t BROADCAST_COUNT = sizeof(BROADCAST_OPTIONS) / sizeof(BROADCAST_OPTIONS[0]);

struct PrecisionOption {
    uint32_t bits;
    const char *label;
};

// The existing precision list: how coarse a position this channel shares
const PrecisionOption PRECISION_OPTIONS[] = {
    {10, "23 KM"}, {11, "12 KM"}, {12, "5.8 KM"}, {13, "2.9 KM"},  {14, "1.5 KM"},
    {15, "700 M"}, {16, "350 M"}, {17, "200 M"},  {32, "PRECISE"},
};
constexpr uint8_t PRECISION_COUNT = sizeof(PRECISION_OPTIONS) / sizeof(PRECISION_OPTIONS[0]);

constexpr uint32_t DEFAULT_PRECISION_BITS = 13; // What the shared menu turns sharing on with

const char *labelFor(const SecondsOption *opts, uint8_t count, uint32_t value)
{
    for (uint8_t i = 0; i < count; i++) {
        if (opts[i].seconds == value)
            return opts[i].label;
    }
    return "CUSTOM";
}

const char *timezoneLabel()
{
    for (const Timezone &t : TIMEZONES) {
        if (strcmp(config.device.tzdef, t.tzdef) == 0)
            return t.label;
    }
    return config.device.tzdef[0] ? "CUSTOM" : "NOT SET";
}

const char *precisionLabel(uint32_t bits)
{
    for (const PrecisionOption &o : PRECISION_OPTIONS) {
        if (o.bits == bits)
            return o.label;
    }
    return bits == 0 ? "OFF" : "CUSTOM";
}

// ---------------------------------------------------------------- capability gates
//
// Every unavailable row names the thing that unblocks it, and each gate is a source flag rather than
// a zero reading (Menu Settings 11, 24)

bool regionSet()
{
    return config.lora.region != meshtastic_Config_LoRaConfig_RegionCode_UNSET;
}

bool gpsFitted()
{
#if !MESHTASTIC_EXCLUDE_GPS && HAS_GPS
    return config.position.gps_mode != meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT;
#else
    return false;
#endif
}

bool mqttOn()
{
#if !MESHTASTIC_EXCLUDE_MQTT
    return moduleConfig.mqtt.enabled;
#else
    return false;
#endif
}

bool wifiConfigured()
{
#if HAS_WIFI && !defined(MESHTASTIC_EXCLUDE_WIFI)
    return config.network.wifi_ssid[0] != '\0';
#else
    return false;
#endif
}

bool sdCardPresent()
{
#if defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI)
    return SD.cardType() != CARD_NONE;
#else
    return false;
#endif
}

bool phoneConnected()
{
    return bluetoothStatus && bluetoothStatus->getConnectionState() == meshtastic::BluetoothStatus::ConnectionState::CONNECTED;
}

// Where "me" is: the live local fix first, then the one NodeDB kept from the last one. The same read
// the native map does, so the two can never disagree
bool ownPosition(double &lat, double &lon)
{
    if (localPosition.latitude_i != 0 || localPosition.longitude_i != 0) {
        lat = localPosition.latitude_i * 1e-7;
        lon = localPosition.longitude_i * 1e-7;
        return true;
    }
    meshtastic_PositionLite stored;
    if (nodeDB->copyNodePosition(nodeDB->getNodeNum(), stored) && (stored.latitude_i != 0 || stored.longitude_i != 0)) {
        lat = stored.latitude_i * 1e-7;
        lon = stored.longitude_i * 1e-7;
        return true;
    }
    return false;
}

// A fixed position can only be turned on from a position we actually have
bool havePosition()
{
    double lat = 0, lon = 0;
    return ownPosition(lat, lon);
}

// Only these two roles are offered. Any other current role is shown as-is and never changed (C18)
bool roleIsOffered()
{
    return config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT ||
           config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE;
}

bool channelSharesPosition(const meshtastic_Channel &ch)
{
    return ch.settings.has_module_settings && ch.settings.module_settings.position_precision > 0;
}

// ---------------------------------------------------------------- value formatting

std::string regionName(meshtastic_Config_LoRaConfig_RegionCode code)
{
    if (code == meshtastic_Config_LoRaConfig_RegionCode_UNSET)
        return "NOT SET";
    const RegionInfo *r = getRegion(code);
    if (!r || !r->name)
        return "NOT SET";
    // The firmware names use underscores; the frozen labels use a space
    std::string out = r->name;
    for (char &c : out)
        if (c == '_')
            c = ' ';
    return out;
}

// "low—high MHz", always a range (Panel Fit D18). Each edge is rounded to 0.1 MHz in whole kHz, halves up,
// and loses a trailing ".0"; if both edges would then read the same, both get two decimals
std::string bandEdge(float mhz, bool twoDecimals)
{
    const uint32_t khz = (uint32_t)lroundf(mhz * 1000.0f);
    char buf[16];
    if (twoDecimals) {
        const uint32_t hundredths = (khz + 5) / 10;
        snprintf(buf, sizeof(buf), "%u.%02u", (unsigned)(hundredths / 100), (unsigned)(hundredths % 100));
    } else {
        const uint32_t tenths = (khz + 50) / 100;
        if (tenths % 10)
            snprintf(buf, sizeof(buf), "%u.%u", (unsigned)(tenths / 10), (unsigned)(tenths % 10));
        else
            snprintf(buf, sizeof(buf), "%u", (unsigned)(tenths / 10));
    }
    return buf;
}

std::string regionBand(meshtastic_Config_LoRaConfig_RegionCode code)
{
    const RegionInfo *r = getRegion(code);
    if (code == meshtastic_Config_LoRaConfig_RegionCode_UNSET || !r || r->freqStart <= 0)
        return "RADIO SILENT";
    std::string low = bandEdge(r->freqStart, false), high = bandEdge(r->freqEnd, false);
    if (low == high) {
        low = bandEdge(r->freqStart, true);
        high = bandEdge(r->freqEnd, true);
    }
    return low + DASH + high + " MHz";
}

std::string presetName()
{
    return DisplayFormatters::getModemPresetDisplayName(config.lora.modem_preset, false, config.lora.use_preset);
}

std::string percentText(float pct, uint8_t decimals)
{
    char buf[16];
    snprintf(buf, sizeof(buf), decimals ? "%.1f %%" : "%.0f %%", pct);
    return buf;
}

std::string channelUseText()
{
    return airTime ? percentText(airTime->channelUtilizationPercent(), 0) : DASH;
}

std::string airtimeText()
{
    return airTime ? percentText(airTime->utilizationTXPercent(), 1) : DASH;
}

std::string frequencyText()
{
    if (!regionSet() || !RadioLibInterface::instance)
        return DASH;
    char buf[24];
    snprintf(buf, sizeof(buf), "%.3f MHz", RadioLibInterface::instance->getFreq());
    return buf;
}

std::string coordinatesText()
{
    double lat = 0, lon = 0;
    if (!ownPosition(lat, lon))
        return DASH;
    char buf[40];
    snprintf(buf, sizeof(buf), "%.4f, %.4f", lat, lon);
    return InkHUD::t5Parse(buf);
}

// The Map vocabulary: where "me" comes from, not whether a receiver is fitted (C8)
const char *positionSource()
{
    if (config.position.fixed_position)
        return "FIXED";
    if (gpsFitted() && config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED)
        return "GPS";
    if (havePosition())
        return "PHONE";
    return "NONE";
}

std::string gpsReceiverText()
{
    if (!gpsFitted())
        return "NOT DETECTED";
    if (config.position.gps_mode != meshtastic_Config_PositionConfig_GpsMode_ENABLED)
        return "OFF";
    if (localPosition.sats_in_view)
        return std::string("FIX ") + SEP + " " + to_string(localPosition.sats_in_view) + " SATS";
    return havePosition() ? "FIX" : "SEARCHING";
}

std::string uptimeText()
{
    const uint32_t secs = millis() / 1000;
    const uint32_t days = secs / 86400, hours = (secs % 86400) / 3600;
    char buf[24];
    if (days)
        snprintf(buf, sizeof(buf), "%u D %02u H", (unsigned)days, (unsigned)hours);
    else
        snprintf(buf, sizeof(buf), "%u H %02u M", (unsigned)hours, (unsigned)((secs % 3600) / 60));
    return buf;
}

// Roles in capitals, as every Menu frame shows them ("CLIENT", Menu C1, C10)
std::string roleText(meshtastic_Config_DeviceConfig_Role role)
{
    std::string text = DisplayFormatters::getDeviceRole(role);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return toupper(ch); });
    return text;
}

// The root hint's one-unit form (Menu C1): "3 d", "4 h", "12 min"
std::string uptimeShortText()
{
    const uint32_t secs = millis() / 1000;
    if (secs >= 86400)
        return to_string(secs / 86400) + " d";
    if (secs >= 3600)
        return to_string(secs / 3600) + " h";
    return to_string(secs / 60) + " min";
}

std::string sdCardText()
{
#if defined(HAS_SDCARD) && !defined(SDCARD_USE_SOFT_SPI)
    if (!sdCardPresent())
        return "NONE";
    const uint64_t total = SD.totalBytes(), used = SD.usedBytes();
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f GB FREE", (total > used ? total - used : 0) / (1024.0 * 1024.0 * 1024.0));
    return buf;
#else
    return "NOT ON THIS BOARD";
#endif
}

std::string radioChipText()
{
    if (!RadioLibInterface::instance)
        return "NOT RESPONDING";
#if defined(USE_SX1262)
    return "SX1262";
#else
    return "LORA";
#endif
}

// The class only: an e-paper panel keeps its last image after power-off, so a key drawn here would
// stay on the glass (Menu Settings 02)
std::string channelKeyClass(const meshtastic_Channel &ch)
{
    const size_t bytes = ch.settings.psk.size;
    if (bytes == 0)
        return "NONE";
    if (bytes == 1)
        return ch.settings.psk.bytes[0] == 0 ? "NONE" : "DEFAULT";
    return std::string("CUSTOM ") + SEP + " " + to_string(bytes * 8) + "-BIT";
}

// A blank primary channel name means the preset name, as it does on every other T5 screen
std::string channelTitle(uint8_t index)
{
    const char *name = channels.getName(index);
    if (name && *name)
        return InkHUD::t5Parse(name);
    return presetName();
}

std::string powerSourceText()
{
    if (!powerStatus || !powerStatus->isInitialized())
        return DASH;
    if (!powerStatus->getHasUSB())
        return "BATTERY";
    return powerStatus->getIsCharging() ? std::string("USB ") + SEP + " CHARGING" : "USB";
}

// ---------------------------------------------------------------- row builders

using InkHUD::T5Act;
using InkHUD::T5Row;
using InkHUD::T5RowKind;

T5Row section(const char *label)
{
    T5Row r;
    r.kind = T5RowKind::SECTION;
    r.label = label;
    return r;
}

T5Row info(const std::string &label, const std::string &value)
{
    T5Row r;
    r.kind = T5RowKind::INFO;
    r.label = label;
    r.value = value;
    return r;
}

T5Row note(const char *text)
{
    T5Row r;
    r.kind = T5RowKind::NOTE;
    r.label = text;
    return r;
}

T5Row nav(const std::string &label, const std::string &value, T5Act act, const char *tag = nullptr, const char *hint = nullptr)
{
    T5Row r;
    r.kind = T5RowKind::NAV;
    r.label = label;
    r.value = value;
    r.action = act;
    r.tag = tag;
    if (hint)
        r.hint = hint;
    return r;
}

T5Row state(const char *label, bool on, T5Act act, const char *tag = nullptr, const char *hint = nullptr)
{
    T5Row r;
    r.kind = T5RowKind::STATE;
    r.label = label;
    r.on = on;
    r.action = act;
    r.arg = on ? 0 : 1; // A tap writes the other state
    r.tag = tag;
    if (hint)
        r.hint = hint;
    return r;
}

T5Row choice(const char *label, std::vector<std::string> opts, uint8_t sel, T5Act act, const char *hint = nullptr)
{
    T5Row r;
    r.kind = T5RowKind::CHOICE;
    r.label = label;
    r.options = std::move(opts);
    r.selected = sel;
    r.action = act;
    if (hint)
        r.hint = hint;
    return r;
}

InkHUD::T5Row facts(std::vector<std::pair<std::string, std::string>> cells)
{
    InkHUD::T5Row r;
    r.kind = InkHUD::T5RowKind::FACTS;
    r.facts = std::move(cells);
    return r;
}

T5Row action(const std::string &label, T5Act act, int32_t arg = 0)
{
    T5Row r;
    r.kind = T5RowKind::ACTION;
    r.label = label;
    r.action = act;
    r.arg = arg;
    return r;
}

// The frozen unavailable treatment: the row stays, the control is replaced by the reason in a dotted
// box, and the tap is ignored. Hiding it would make the app instructions unfindable (Menu Settings 11)
void block(T5Row &r, const std::string &reason, const char *hint = nullptr)
{
    r.available = false;
    r.reason = reason;
    r.action = T5Act::NONE;
    r.tag = nullptr;
    r.hint = hint ? hint : "";
}

} // namespace

// ================================================================ categories

static const InkHUD::T5Page CATEGORY_PAGES[InkHUD::T5_CATEGORY_COUNT] = {
    InkHUD::T5Page::RADIO,  InkHUD::T5Page::CHANNELS, InkHUD::T5Page::POSITION, InkHUD::T5Page::CONNECTIVITY,
    InkHUD::T5Page::SCREEN, InkHUD::T5Page::POWER,    InkHUD::T5Page::DEVICE,   InkHUD::T5Page::ABOUT,
};

static const char *const CATEGORY_NAMES[InkHUD::T5_CATEGORY_COUNT] = {
    "RADIO", "CHANNELS", "POSITION", "CONNECTIVITY", "DISPLAY", "POWER", "DEVICE", "ABOUT",
};

static const InkHUD::T5Act CATEGORY_ACTIONS[InkHUD::T5_CATEGORY_COUNT] = {
    T5Act::OPEN_RADIO,   T5Act::OPEN_CHANNELS, T5Act::OPEN_POSITION, T5Act::OPEN_CONNECTIVITY,
    T5Act::OPEN_DISPLAY, T5Act::OPEN_POWER,    T5Act::OPEN_DEVICE,   T5Act::OPEN_ABOUT,
};

InkHUD::T5Page InkHUD::t5CategoryPage(uint8_t index)
{
    return CATEGORY_PAGES[index < T5_CATEGORY_COUNT ? index : 0];
}

const char *InkHUD::t5CategoryName(uint8_t index)
{
    return CATEGORY_NAMES[index < T5_CATEGORY_COUNT ? index : 0];
}

// The root is also a status summary: every second line is live state (Menu Settings C1)
std::string InkHUD::t5CategorySummary(uint8_t index, bool shortForm)
{
    switch (t5CategoryPage(index)) {
    case T5Page::RADIO: {
        if (!regionSet())
            return "REGION NOT SET";
        const std::string head = T5Applet::join(regionName(config.lora.region), presetName());
        return shortForm ? head : T5Applet::join(head, config.lora.tx_enabled ? "TRANSMIT ON" : "LISTEN ONLY");
    }
    case T5Page::CHANNELS: {
        const uint8_t count = channels.getNumChannels();
        if (shortForm)
            return to_string(count) + " ACTIVE";
        return to_string(count) + " active " + SEP + " primary " + channelTitle(channels.getPrimaryIndex());
    }
    case T5Page::POSITION: {
        if (!gpsFitted())
            return shortForm ? std::string("NO GPS") : std::string("No receiver, source ") + positionSource();
        if (config.position.gps_mode != meshtastic_Config_PositionConfig_GpsMode_ENABLED)
            return shortForm ? "GPS OFF" : "GPS off";
        if (shortForm)
            return std::string("GPS ") + SEP + " " + gpsReceiverText();
        // Root hint (Menu C1): "GPS FIX · 9 sats", "GPS searching"
        if (localPosition.sats_in_view)
            return std::string("GPS FIX ") + SEP + " " + to_string(localPosition.sats_in_view) + " sats";
        return havePosition() ? "GPS FIX" : "GPS searching";
    }
    case T5Page::CONNECTIVITY: {
        if (!config.bluetooth.enabled)
            return shortForm ? "BT OFF" : "Bluetooth off";
        if (shortForm)
            return phoneConnected() ? std::string("BT ") + SEP + " PHONE ON" : std::string("BT ") + SEP + " NO PHONE";
        return std::string("Bluetooth on ") + SEP + (phoneConnected() ? " phone connected" : " no phone");
    }
    case T5Page::SCREEN: {
        const bool console = t5CurrentMode() == T5Mode::CONSOLE;
        if (shortForm)
            return std::string(console ? "CONSOLE " : "CARRY ") + SEP + (t5BacklightIsUserEnabled() ? " LIGHT ON" : " LIGHT OFF");
        return std::string(console ? "Console" : "Carry") + " " + SEP + " " +
               (t5BacklightIsUserEnabled() ? "light on" : "light off") + " " + SEP + " " +
               (t5TouchPreferenceEnabled() ? "touch on" : "touch off");
    }
    case T5Page::POWER: {
        // Root hint (Menu C1): "87 % · 4.05 V · charging"; the Console tile keeps the short status form
        if (!shortForm && powerStatus->isInitialized() && powerStatus->getHasBattery()) {
            char volts[12];
            snprintf(volts, sizeof(volts), "%.2f V", powerStatus->getBatteryVoltageMv() / 1000.0);
            std::string hint = T5Applet::join(to_string(powerStatus->getBatteryChargePercent()) + " %", volts);
            if (powerStatus->getHasUSB())
                hint = T5Applet::join(hint, powerStatus->getIsCharging() ? "charging" : "on USB");
            return hint;
        }
        const std::string head = T5Applet::powerLabel(!shortForm, !shortForm);
        return head.empty() ? std::string("BATTERY ") + DASH : head;
    }
    case T5Page::DEVICE:
        return T5Applet::join(t5Parse(shortForm ? owner.short_name : owner.long_name), roleText(config.device.role));
    case T5Page::ABOUT:
        return shortForm ? "UP " + uptimeText() : "Firmware, hardware, uptime " + uptimeShortText();
    default:
        return "";
    }
}

bool InkHUD::t5PageExists(T5Page page)
{
#ifdef T5_DEVTOOLS
    (void)page;
    return true;
#else
    return page != T5Page::DEVTOOLS;
#endif
}

// ================================================================ headers

std::string InkHUD::t5PageTitle(T5Page page, uint8_t channelIndex)
{
    switch (page) {
    case T5Page::ROOT:
        return "MENU";
    case T5Page::CHANNEL:
        return channelTitle(channelIndex);
    case T5Page::DEVTOOLS:
        return "DEVELOPER TOOLS";
    default:
        for (uint8_t i = 0; i < T5_CATEGORY_COUNT; i++) {
            if (CATEGORY_PAGES[i] == page)
                return CATEGORY_NAMES[i];
        }
        return "MENU";
    }
}

std::string InkHUD::t5PageContext(T5Page page, uint8_t channelIndex)
{
    switch (page) {
    case T5Page::ROOT:
        return T5Applet::join(T5Applet::join(t5Parse(owner.short_name), t5NodeId(nodeDB->getNodeNum())),
                              regionName(config.lora.region)); // NOT SET when unset (Panel Fit D16)
    case T5Page::RADIO:
        return "CHANGES CAN CUT YOU OFF THE MESH";
    case T5Page::CHANNELS:
        return "KEYS STAY IN THE PHONE APP";
    case T5Page::CHANNEL:
        return "CHANNEL " + to_string(channelIndex) + " " + SEP + " " +
               (channelIndex == channels.getPrimaryIndex() ? "PRIMARY" : "SECONDARY");
    case T5Page::POSITION:
        return "SOURCE: " + std::string(positionSource()) + " " + SEP + " USED FOR MAP AND DISTANCES";
    case T5Page::CONNECTIVITY:
        return config.bluetooth.enabled ? (phoneConnected() ? "PHONE CONNECTED OVER BLUETOOTH" : "NO PHONE CONNECTED")
                                        : "BLUETOOTH OFF";
    case T5Page::SCREEN: {
        std::string s = t5CurrentMode() == T5Mode::CONSOLE ? "CONSOLE" : "CARRY";
        s += std::string(" ") + SEP + (t5BacklightIsUserEnabled() ? " LIGHT ON " : " LIGHT OFF ") + SEP;
        s += t5TouchPreferenceEnabled() ? " TOUCH ON" : " TOUCH OFF";
        return s;
    }
    case T5Page::POWER: {
        const std::string p = T5Applet::powerLabel(true, true);
        return p.empty() ? "BATTERY NOT INITIALISED" : p;
    }
    case T5Page::DEVICE:
        return T5Applet::join(t5Parse(owner.short_name), roleText(config.device.role));
    case T5Page::ABOUT:
        return std::string("T5 E-PAPER S3 PRO ") + SEP + " UP " + uptimeText();
    case T5Page::DEVTOOLS:
        return "DEVELOPER BUILD";
    }
    return "";
}

const char *InkHUD::t5PageBack(T5Page page)
{
    switch (page) {
    case T5Page::ROOT:
        return nullptr; // A top-level destination has no Back
    case T5Page::CHANNEL:
        return "CHANNELS";
    case T5Page::DEVTOOLS:
        return "ABOUT";
    default:
        return "MENU";
    }
}

InkHUD::T5Page InkHUD::t5PageParent(T5Page page)
{
    switch (page) {
    case T5Page::CHANNEL:
        return T5Page::CHANNELS;
    case T5Page::DEVTOOLS:
        return T5Page::ABOUT;
    default:
        return T5Page::ROOT;
    }
}

// ================================================================ pages

std::vector<InkHUD::T5Row> InkHUD::t5BuildPage(T5Page page, uint8_t channelIndex, bool console)
{
    std::vector<T5Row> rows;

    switch (page) {

    // ---- C1 / C16 -----------------------------------------------------------------------
    case T5Page::ROOT: {
        if (!regionSet()) {
            // The one banner the root carries. Banner and RADIO row both open the picker (C16)
            T5Row b;
            b.kind = T5RowKind::BANNER;
            b.label = "REGION NOT SET";
            b.hint = "The radio stays silent until you choose one. Tap to choose.";
            b.action = T5Act::PICK_REGION;
            rows.push_back(b);
        }
        for (uint8_t i = 0; i < T5_CATEGORY_COUNT; i++) {
            T5Row r;
            r.kind = T5RowKind::CATEGORY;
            r.label = t5CategoryName(i);
            r.summary = t5CategorySummary(i, false);
            r.action = CATEGORY_ACTIONS[i];
            r.arg = i;
            rows.push_back(r);
        }
        break;
    }

    // ---- C2 / K2 ------------------------------------------------------------------------
    case T5Page::RADIO: {
        rows.push_back(section("MESH"));
        rows.push_back(nav("REGION", regionName(config.lora.region), T5Act::PICK_REGION, "RESTARTS"));

        T5Row preset = nav("MODEM PRESET", presetName(), T5Act::PICK_PRESET, "RESTARTS");
        if (!regionSet())
            block(preset, "SET REGION FIRST");
        rows.push_back(preset);

        rows.push_back(
            nav("HOP LIMIT", to_string(Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit)), T5Act::PICK_HOP));

        T5Row tx = state("TRANSMIT", config.lora.tx_enabled, T5Act::SET_TRANSMIT, nullptr, "Off = listen only. Nothing is sent.");
        if (!regionSet())
            block(tx, "SET REGION FIRST");
        rows.push_back(tx);

        rows.push_back(section("LIVE"));
        rows.push_back(info("CHANNEL USE", channelUseText()));
        rows.push_back(info("MY AIRTIME", airtimeText()));
        rows.push_back(info("FREQUENCY", frequencyText()));

        rows.push_back(section("IN THE PHONE APP"));
        rows.push_back(note("Frequency slot, override frequency, TX power, bandwidth and spreading factor."));
        break;
    }

    // ---- C6 -----------------------------------------------------------------------------
    case T5Page::CHANNELS: {
        rows.push_back(section("ACTIVE"));
        const uint8_t count = channels.getNumChannels();
        for (uint8_t i = 0; i < count; i++) {
            T5Row r = nav(to_string(i) + " " + SEP + " " + channelTitle(i),
                          i == channels.getPrimaryIndex() ? "PRIMARY" : "SECONDARY", T5Act::OPEN_CHANNEL);
            r.arg = i;
            r.smallValue = true;
            rows.push_back(r);
        }
        if (count < 8) {
            rows.push_back(section("EMPTY"));
            rows.push_back(info(count + 1 == 8 ? "SLOT 7" : "SLOTS " + to_string(count) + DASH + "7", "UNUSED"));
        }
        rows.push_back(section("IN THE PHONE APP"));
        rows.push_back(note("Add, remove or rename channels and change keys in the Meshtastic app, or share them by QR "
                            "code from the phone. Keys are never shown on this device."));
        break;
    }

    // ---- C7 -----------------------------------------------------------------------------
    case T5Page::CHANNEL: {
        const meshtastic_Channel &ch = channels.getByIndex(channelIndex);

        rows.push_back(section("SECURITY"));
        T5Row key;
        key.kind = T5RowKind::NAV;
        key.label = "KEY";
        block(key, channelKeyClass(ch), "Set in the Meshtastic app. Never shown here.");
        rows.push_back(key);

        rows.push_back(section("SHARING"));
        rows.push_back(state("SHARE MY POSITION", channelSharesPosition(ch), T5Act::SET_SHARE_POSITION));

        T5Row precision =
            nav("PRECISION", precisionLabel(ch.settings.has_module_settings ? ch.settings.module_settings.position_precision : 0),
                T5Act::PICK_PRECISION);
        if (!channelSharesPosition(ch))
            block(precision, "SHARING OFF");
        rows.push_back(precision);

        rows.push_back(section("MQTT"));
        T5Row up = state("UPLINK", ch.settings.uplink_enabled, T5Act::SET_UPLINK);
        T5Row down = state("DOWNLINK", ch.settings.downlink_enabled, T5Act::SET_DOWNLINK);
        if (!mqttOn()) {
            block(up, "MQTT OFF");
            block(down, "MQTT OFF", "Turn MQTT on in the Meshtastic app first.");
        }
        rows.push_back(up);
        rows.push_back(down);
        break;
    }

    // ---- C8 -----------------------------------------------------------------------------
    case T5Page::POSITION: {
        rows.push_back(section("GPS HARDWARE"));
        rows.push_back(info("RECEIVER", gpsReceiverText()));

        T5Row gps = state("GPS", config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_ENABLED, T5Act::SET_GPS);
        if (!gpsFitted())
            block(gps, "NO RECEIVER", "Some H752-01 boards ship without a GPS module.");
        rows.push_back(gps);

        rows.push_back(section("MY POSITION"));
        rows.push_back(info("SOURCE", positionSource()));
        rows.push_back(info("COORDINATES", coordinatesText()));

        T5Row fixed = state("FIXED POSITION", config.position.fixed_position, T5Act::SET_FIXED_POSITION, nullptr,
                            "Stays at this point. Turning on stores the current position.");
        if (!config.position.fixed_position && !havePosition())
            block(fixed, "NO POSITION YET");
        rows.push_back(fixed);

        rows.push_back(section("SHARING"));
        rows.push_back(nav(
            "BROADCAST EVERY",
            labelFor(BROADCAST_OPTIONS, BROADCAST_COUNT,
                     Default::getConfiguredOrDefault(config.position.position_broadcast_secs, default_broadcast_interval_secs)),
            T5Act::PICK_BROADCAST));

        T5Row smart = state("SMART POSITION", config.position.position_broadcast_smart_enabled, T5Act::SET_SMART_POSITION);
        if (config.position.fixed_position)
            block(smart, "FIXED IS ON");
        rows.push_back(smart);
        break;
    }

    // ---- C9 -----------------------------------------------------------------------------
    case T5Page::CONNECTIVITY: {
        rows.push_back(section("BLUETOOTH"));
        rows.push_back(state("BLUETOOTH", config.bluetooth.enabled, T5Act::SET_BLUETOOTH, "RESTARTS"));
        rows.push_back(info("PHONE", !config.bluetooth.enabled ? "BLUETOOTH OFF"
                                     : phoneConnected()        ? "CONNECTED"
                                                               : "NOT CONNECTED"));
        // The pairing mode, not fixed_pin: that field is the PIN number itself
        const auto pinMode = config.bluetooth.mode;
        rows.push_back(nav("PAIRING PIN",
                           pinMode == meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN ? "FIXED"
                           : pinMode == meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN  ? "NO PIN"
                                                                                              : "RANDOM",
                           T5Act::PICK_PIN_MODE, nullptr, "A fixed PIN is chosen in the app."));

        rows.push_back(section("WI-FI"));
        T5Row wifi = state("WI-FI", config.network.wifi_enabled, T5Act::SET_WIFI, "RESTARTS");
#if !HAS_WIFI || defined(MESHTASTIC_EXCLUDE_WIFI)
        block(wifi, "NOT IN THIS BUILD");
#else
        if (!wifiConfigured())
            block(wifi, "NOT SET UP", "Add a network in the Meshtastic app first.");
#endif
        rows.push_back(wifi);

        rows.push_back(section("MQTT"));
        rows.push_back(info("MQTT", mqttOn() ? "ON" : "OFF"));
        rows.push_back(note("Network name, password, MQTT server and topics are set in the Meshtastic app."));
        break;
    }

    // ---- C5 / K1 ------------------------------------------------------------------------
    case T5Page::SCREEN: {
        rows.push_back(section("MODE"));
        rows.push_back(choice("MODE", {"CARRY", "CONSOLE"}, t5CurrentMode() == T5Mode::CONSOLE ? 1 : 0, T5Act::SET_MODE,
                              "Console is landscape, for desk use."));

        rows.push_back(section("LIGHT & TOUCH"));
        rows.push_back(state("FRONT LIGHT", t5BacklightIsUserEnabled(), T5Act::SET_FRONT_LIGHT));
        rows.push_back(nav("SLEEP AFTER", labelFor(SLEEP_OPTIONS, SLEEP_COUNT, config.display.screen_on_secs), T5Act::PICK_SLEEP,
                           nullptr, "Light and touch turn off. Side button wakes."));

        // OFF is offered because the side-button short press calls toggleTouchInputEnabled() while
        // touch is off, which is the recovery path the confirmation promises (decision 7)
        rows.push_back(state("TOUCH", t5TouchPreferenceEnabled(), T5Act::SET_TOUCH, nullptr,
                             "Asks first. The side button turns it back on."));

        rows.push_back(section("UNITS & TIME"));
        rows.push_back(choice("UNITS", {"METRIC", "IMPERIAL"},
                              config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL ? 1 : 0,
                              T5Act::SET_UNITS));
        rows.push_back(choice("CLOCK", {"24 H", "12 H"}, config.display.use_12h_clock ? 1 : 0, T5Act::SET_CLOCK));

        rows.push_back(section("E-PAPER"));
        rows.push_back(action("CLEAN SCREEN", T5Act::CLEAN_SCREEN));
        break;
    }

    // ---- C11 ----------------------------------------------------------------------------
    case T5Page::POWER: {
        const bool ready = powerStatus && powerStatus->isInitialized() && powerStatus->getHasBattery();
        rows.push_back(section("BATTERY"));
        rows.push_back(info("LEVEL", ready ? to_string(powerStatus->getBatteryChargePercent()) + " %" : DASH));
        if (ready) {
            char volts[12];
            snprintf(volts, sizeof(volts), "%.2f V", powerStatus->getBatteryVoltageMv() / 1000.0);
            rows.push_back(info("VOLTAGE", volts));
        } else {
            rows.push_back(info("VOLTAGE", DASH));
        }
        rows.push_back(info("SOURCE", powerSourceText()));

        rows.push_back(section("SAVING"));
        T5Row saving = state("POWER SAVING", config.power.is_power_saving, T5Act::SET_POWER_SAVING, "RESTARTS",
                             "Longer battery life. May turn Bluetooth off on this board.");
#if !defined(ARCH_ESP32)
        block(saving, "NOT ON THIS BOARD");
#endif
        rows.push_back(saving);

        rows.push_back(section("DEVICE"));
        rows.push_back(action(std::string("REBOOT") + ELLIPSIS, T5Act::REBOOT));
        rows.push_back(action(std::string("SHUT DOWN") + ELLIPSIS, T5Act::SHUT_DOWN));
        rows.push_back(note("Shut down here before unplugging - some screen settings are only saved on a clean shutdown."));
        break;
    }

    // ---- C10 / C18 ----------------------------------------------------------------------
    case T5Page::DEVICE: {
        rows.push_back(section("IDENTITY"));
        rows.push_back(nav("LONG NAME", t5Parse(owner.long_name), T5Act::EDIT_LONG_NAME));
        rows.push_back(nav("SHORT NAME", t5Parse(owner.short_name), T5Act::EDIT_SHORT_NAME));

        const std::string roleName = roleText(config.device.role);
        T5Row role = nav("ROLE", roleName, T5Act::PICK_ROLE, "RESTARTS");
        if (!roleIsOffered()) {
            // The actual value is useful, so it is shown rather than hidden, and never silently
            // changed (C18, decision 3)
            role.value.clear();
            block(role, roleName, "Change this role in the Meshtastic app.");
        }
        rows.push_back(role);

        rows.push_back(section("TIME"));
        rows.push_back(nav("TIME ZONE", timezoneLabel(), T5Act::PICK_TZ));

        rows.push_back(section("CLEAR DATA"));
        rows.push_back(action(std::string("CLEAR NODE LIST") + ELLIPSIS, T5Act::CLEAR_NODES));
        rows.push_back(action(std::string("CLEAR MESSAGES") + ELLIPSIS, T5Act::CLEAR_MESSAGES));
        rows.push_back(note("Factory reset is only available from the Meshtastic app."));
        break;
    }

    // ---- C14 / K3 -----------------------------------------------------------------------
    case T5Page::ABOUT: {
        const std::string name = T5Applet::join(t5Parse(owner.long_name), t5Parse(owner.short_name));
        const std::string board =
#if defined(T5_S3_EPAPER_PRO_V1)
            std::string("T5 S3 Pro ") + SEP + " H752";
#else
            std::string("T5 S3 Pro ") + SEP + " H752-01";
#endif
        const std::string radio = T5Applet::join(radioChipText(), regionSet() ? regionName(config.lora.region) : "");
        const std::string gps = gpsFitted() ? std::string("FITTED ") + SEP + " " + gpsReceiverText() : "NOT FITTED";
#ifdef T5_DEVTOOLS
        const std::string build = std::string("t5-inkhud ") + SEP + " dev";
#else
        const std::string build = std::string("t5-inkhud ") + SEP + " release";
#endif

        if (console) {
            // The facts grid uses the width; read-only cells are not touch targets (K3)
            rows.push_back(facts({{"NAME", name},
                                  {"NODE ID", t5NodeId(nodeDB->getNodeNum())},
                                  {"BOARD", board},
                                  {"RADIO", radio},
                                  {"GPS", gps},
                                  {"SD CARD", sdCardText()},
                                  {"FIRMWARE", optstr(APP_VERSION)},
                                  {"T5 BUILD", build},
                                  {"UPTIME", uptimeText()}}));
        } else {
            rows.push_back(section("THIS NODE"));
            rows.push_back(info("NAME", name));
            rows.push_back(info("NODE ID", t5NodeId(nodeDB->getNodeNum())));

            rows.push_back(section("HARDWARE"));
            rows.push_back(info("BOARD", board));
            rows.push_back(info("RADIO", radio));
            rows.push_back(info("GPS", gps));
            rows.push_back(info("SD CARD", sdCardText()));

            rows.push_back(section("SOFTWARE"));
            rows.push_back(info("FIRMWARE", optstr(APP_VERSION)));
            rows.push_back(info("T5 BUILD", build));
            rows.push_back(info("UPTIME", uptimeText()));
        }

#ifdef T5_DEVTOOLS
        // Only developer builds have the section and the code behind it: no hidden unlock (decision 1)
        if (!console)
            rows.push_back(section("DEVELOPER"));
        rows.push_back(nav("DEVELOPER TOOLS", "", T5Act::OPEN_DEVTOOLS)); // A value would cut the fixed label in Console
#endif
        break;
    }

    // ---- C15 / K4 -----------------------------------------------------------------------
    case T5Page::DEVTOOLS: {
#ifdef T5_DEVTOOLS
        char heap[48];
        snprintf(heap, sizeof(heap), "%u KB %s MIN %u", (unsigned)(ESP.getFreeHeap() / 1024), SEP,
                 (unsigned)(ESP.getMinFreeHeap() / 1024));
        char psram[24];
        snprintf(psram, sizeof(psram), "%.1f MB", ESP.getFreePsram() / (1024.0 * 1024.0));
        const std::string use = channelUseText() + " " + SEP + " " + airtimeText();

        // The two tools the frozen page wraps are not wired to it. The screenshot tool captures three
        // named screens from a BOOT + side-key chord, and teaching it a fourth means broadening
        // screenshot infrastructure, which Phase 7 is explicitly not doing; touch calibration is its
        // own build-flagged applet. Both rows keep their place and say so rather than being a tap that
        // silently does nothing. Phase 8 owns screenshot automation and can wire them.
        T5Row shot = action("SAVE SCREENSHOT TO SD", T5Act::DEV_SCREENSHOT);
        block(shot, sdCardPresent() ? "NOT IN THIS BUILD" : "NO SD CARD");
        T5Row cal = action(std::string("TOUCH CALIBRATION") + ELLIPSIS, T5Act::DEV_TOUCH_CAL);
        block(cal, "NOT IN THIS BUILD");

        if (console) {
            rows.push_back(facts({{"FREE HEAP", heap},
                                  {"FREE PSRAM", psram},
                                  {"LAST RESET", resetReasonText((int)esp_reset_reason())},
                                  {"SD CARD", sdCardText()},
                                  {std::string("CHANNEL ") + SEP + " AIRTIME", use}}));
            rows.push_back(shot);
            rows.push_back(cal);
        } else {
            rows.push_back(section("TOOLS"));
            rows.push_back(shot);
            rows.push_back(cal);

            rows.push_back(section("SYSTEM"));
            rows.push_back(info("FREE HEAP", heap));
            rows.push_back(info("FREE PSRAM", psram));
            rows.push_back(info("LAST RESET", resetReasonText((int)esp_reset_reason())));

            rows.push_back(section("RADIO"));
            rows.push_back(info(std::string("CHANNEL USE ") + SEP + " AIRTIME", use));
        }
#endif
        break;
    }
    }

    return rows;
}

// ================================================================ pickers

std::string InkHUD::t5PickTitle(T5Act which)
{
    switch (which) {
    case T5Act::PICK_REGION:
        return "REGION";
    case T5Act::PICK_PRESET:
        return "MODEM PRESET";
    case T5Act::PICK_HOP:
        return "HOP LIMIT";
    case T5Act::PICK_ROLE:
        return "ROLE";
    case T5Act::PICK_SLEEP:
        return "SLEEP AFTER";
    case T5Act::PICK_BROADCAST:
        return "BROADCAST EVERY";
    case T5Act::PICK_PRECISION:
        return "PRECISION";
    case T5Act::PICK_TZ:
        return "TIME ZONE";
    case T5Act::PICK_PIN_MODE:
        return "PAIRING PIN";
    default:
        return "";
    }
}

std::string InkHUD::t5PickContext(T5Act which)
{
    switch (which) {
    case T5Act::PICK_REGION:
        return "WHERE YOU ARE " + std::string(SEP) + " SETS LEGAL BANDS"; // Fits 476 (Menu C3, D18)
    case T5Act::PICK_PRESET:
        return "ONLY PRESETS YOUR REGION ALLOWS";
    case T5Act::PICK_HOP:
        return "HOW FAR A MESSAGE MAY TRAVEL";
    case T5Act::PICK_ROLE:
        return "HOW THIS NODE BEHAVES ON THE MESH";
    case T5Act::PICK_SLEEP:
        return "LIGHT AND TOUCH TURN OFF AFTER THIS";
    case T5Act::PICK_BROADCAST:
        return "HOW OFTEN YOUR POSITION IS SENT";
    case T5Act::PICK_PRECISION:
        return "HOW COARSE A POSITION THIS CHANNEL SHARES";
    case T5Act::PICK_TZ:
        return "CLOCKS AND MESSAGE TIMES";
    case T5Act::PICK_PIN_MODE:
        return "A FIXED PIN IS CHOSEN IN THE APP";
    default:
        return "";
    }
}

std::vector<InkHUD::T5Option> InkHUD::t5PickOptions(T5Act which, uint8_t channelIndex)
{
    std::vector<T5Option> out;

    switch (which) {
    case T5Act::PICK_REGION: {
        // Canonical firmware enum order, never locale-sorted. NOT SET is always first, and is shown
        // but not selectable once a region is set (decision 4, C3)
        for (uint8_t code = 0; code <= (uint8_t)meshtastic_Config_LoRaConfig_RegionCode_ITU2_125CM; code++) {
            const auto region = (meshtastic_Config_LoRaConfig_RegionCode)code;
            if (region != meshtastic_Config_LoRaConfig_RegionCode_UNSET && !getRegion(region))
                continue;
            T5Option o;
            o.value = code;
            o.selected = config.lora.region == region;
            // The range right-aligned on the name's line (Menu C3, K5, Panel Fit D18)
            o.label = regionName(region);
            o.detail = regionBand(region);
            o.detailRight = true;
            if (region == meshtastic_Config_LoRaConfig_RegionCode_UNSET)
                o.selectable = o.selected; // Never a way back to UNSET once a region is set
            out.push_back(o);
        }
        break;
    }

    case T5Act::PICK_PRESET: {
        // Filtered to what the region allows, which is what SET_PRESET_FROM_REGION does
        if (myRegion && myRegion->profile) {
            const meshtastic_Config_LoRaConfig_ModemPreset *presets = myRegion->getAvailablePresets();
            const size_t count = myRegion->getNumPresets();
            for (size_t i = 0; i < count; i++) {
                T5Option o;
                o.label = DisplayFormatters::getModemPresetDisplayName(presets[i], false, true);
                o.value = (int32_t)presets[i];
                o.selected = config.lora.use_preset && config.lora.modem_preset == presets[i];
                out.push_back(o);
            }
        }
        break;
    }

    case T5Act::PICK_HOP: {
        for (uint8_t h = 1; h <= 7; h++) {
            T5Option o;
            o.label = to_string(h);
            o.detail = h == 1 ? "neighbours only" : h == 3 ? "the usual choice" : "";
            o.value = h;
            o.selected = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit) == h;
            out.push_back(o);
        }
        break;
    }

    case T5Act::PICK_ROLE: {
        // The two roles that suit a handheld, and only those (decision 3)
        T5Option client;
        client.label = "CLIENT";
        client.detail = "relays for others";
        client.value = (int32_t)meshtastic_Config_DeviceConfig_Role_CLIENT;
        client.selected = config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT;
        out.push_back(client);

        T5Option mute;
        mute.label = "CLIENT MUTE";
        mute.detail = "does not relay";
        mute.value = (int32_t)meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE;
        mute.selected = config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE;
        out.push_back(mute);
        break;
    }

    case T5Act::PICK_SLEEP: {
        for (uint8_t i = 0; i < SLEEP_COUNT; i++) {
            T5Option o;
            o.label = SLEEP_OPTIONS[i].label;
            o.detail = SLEEP_OPTIONS[i].seconds == 0 ? "the screen never sleeps" : "";
            o.value = (int32_t)SLEEP_OPTIONS[i].seconds;
            o.selected = config.display.screen_on_secs == SLEEP_OPTIONS[i].seconds;
            out.push_back(o);
        }
        break;
    }

    case T5Act::PICK_BROADCAST: {
        const uint32_t current =
            Default::getConfiguredOrDefault(config.position.position_broadcast_secs, default_broadcast_interval_secs);
        for (uint8_t i = 0; i < BROADCAST_COUNT; i++) {
            T5Option o;
            o.label = BROADCAST_OPTIONS[i].label;
            o.value = (int32_t)BROADCAST_OPTIONS[i].seconds;
            o.selected = current == BROADCAST_OPTIONS[i].seconds;
            out.push_back(o);
        }
        break;
    }

    case T5Act::PICK_PRECISION: {
        const meshtastic_Channel &ch = channels.getByIndex(channelIndex);
        const uint32_t current = ch.settings.has_module_settings ? ch.settings.module_settings.position_precision : 0;
        for (uint8_t i = 0; i < PRECISION_COUNT; i++) {
            T5Option o;
            o.label = PRECISION_OPTIONS[i].label;
            o.value = (int32_t)PRECISION_OPTIONS[i].bits;
            o.selected = current == PRECISION_OPTIONS[i].bits;
            out.push_back(o);
        }
        break;
    }

    case T5Act::PICK_PIN_MODE: {
        for (uint8_t i = 0; i < 2; i++) {
            T5Option o;
            o.label = i ? "FIXED" : "RANDOM";
            o.value = i;
            o.selected = config.bluetooth.mode == (i ? meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN
                                                     : meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN);
            out.push_back(o);
        }
        break;
    }

    case T5Act::PICK_TZ: {
        for (uint8_t i = 0; i < TIMEZONE_COUNT; i++) {
            T5Option o;
            o.label = TIMEZONES[i].label;
            o.value = i;
            o.selected = strcmp(config.device.tzdef, TIMEZONES[i].tzdef) == 0;
            out.push_back(o);
        }
        break;
    }

    default:
        break;
    }

    return out;
}

// ================================================================ confirmations
//
// Confirm if it restarts, cuts you off, or destroys data. Everything else applies on tap and is
// reversed with one more tap, which keeps confirmations rare enough to be read (Menu Settings 07)

bool InkHUD::t5NeedsConfirm(T5Act which, int32_t value, uint8_t channelIndex, T5Confirm &out)
{
    (void)channelIndex;
    out = T5Confirm();

    switch (which) {

    case T5Act::PICK_REGION: {
        const auto target = (meshtastic_Config_LoRaConfig_RegionCode)value;
        if (target == config.lora.region)
            return false; // Tapping the current region returns silently
        out.title = "CHANGE REGION?";
        out.crumb = std::string("RADIO ") + T5Applet::FWD + " REGION";
        out.from = regionName(config.lora.region);
        out.to = regionName(target);
        out.fromDetail = regionBand(config.lora.region); // RADIO SILENT when unset (D17)
        out.toDetail = regionBand(target);
        out.bullets = {"The device restarts in about 5 seconds.", "You will only hear nodes on the same region and preset.",
                       "Transmitting outside your real region can be illegal."};
        // applyLoRaRegion re-enables transmit and the boot clamp rewrites a preset the new region
        // does not offer, so the copy says so rather than leaving it to be discovered
        out.bullets.push_back("If the preset is not allowed in " + out.to + " it is reset, and transmit is turned back on.");
        out.keptLabel = "KEPT";
        out.kept = {"Channels, names, node list and messages."};
        out.verb = "CHANGE & RESTART";
        out.restarts = true;
        return true;
    }

    case T5Act::PICK_PRESET: {
        const auto target = (meshtastic_Config_LoRaConfig_ModemPreset)value;
        if (config.lora.use_preset && target == config.lora.modem_preset)
            return false;
        out.title = "CHANGE PRESET?";
        out.crumb = std::string("RADIO ") + T5Applet::FWD + " MODEM PRESET";
        out.from = presetName();
        out.to = DisplayFormatters::getModemPresetDisplayName(target, false, true);
        out.bullets = {"The device restarts in about 5 seconds.", "You will only hear nodes on the same region and preset.",
                       "A slower preset reaches further but carries less."};
        out.keptLabel = "KEPT";
        out.kept = {"Channels, names, node list and messages."};
        out.verb = "CHANGE & RESTART";
        out.restarts = true;
        return true;
    }

    case T5Act::PICK_ROLE: {
        const auto target = (meshtastic_Config_DeviceConfig_Role)value;
        if (target == config.device.role)
            return false;
        out.title = "CHANGE ROLE?";
        out.crumb = std::string("DEVICE ") + T5Applet::FWD + " ROLE";
        out.from = roleText(config.device.role);
        out.to = roleText(target);
        out.bullets = {"The device restarts in about 5 seconds.",
                       target == meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE
                           ? "This node stops relaying other people's messages."
                           : "This node relays other people's messages again.",
                       "Your own messages are unaffected."};
        out.verb = "CHANGE & RESTART";
        out.restarts = true;
        return true;
    }

    case T5Act::SET_TRANSMIT: {
        if (value)
            return false; // Turning transmit back on applies at once
        out.title = "STOP TRANSMITTING?";
        out.crumb = std::string("RADIO ") + T5Applet::FWD + " TRANSMIT";
        out.from = "TRANSMIT ON";
        out.to = "LISTEN ONLY";
        out.bullets = {"Nothing you send leaves the device.", "You still hear everyone else.",
                       "No one can see this node on the mesh."};
        out.keptLabel = "TO START AGAIN";
        out.kept = {"Turn TRANSMIT back on here."};
        out.verb = "STOP TRANSMITTING";
        return true;
    }

    case T5Act::SET_TOUCH: {
        if (value)
            return false;
        out.title = "TURN TOUCH OFF?";
        out.crumb = std::string("DISPLAY ") + T5Applet::FWD + " TOUCH";
        out.from = "TOUCH ON";
        out.to = "TOUCH OFF";
        out.bullets = {"The screen stops responding to fingers.", "Buttons keep working."};
        out.keptLabel = "TO TURN IT BACK ON";
        out.kept = {"Press the side button once."};
        out.verb = "TURN TOUCH OFF";
        return true;
    }

    case T5Act::SET_BLUETOOTH: {
        out.title = value ? "TURN BLUETOOTH ON?" : "TURN BLUETOOTH OFF?";
        out.crumb = std::string("CONNECTIVITY ") + T5Applet::FWD + " BLUETOOTH";
        out.from = config.bluetooth.enabled ? "BLUETOOTH ON" : "BLUETOOTH OFF";
        out.to = value ? "BLUETOOTH ON" : "BLUETOOTH OFF";
        out.bullets = {"The device restarts in about 5 seconds."};
        if (value) {
            out.bullets.push_back("Wi-Fi is turned off: this board runs one at a time.");
            out.bullets.push_back("Your phone can pair again after the restart.");
        } else {
            out.bullets.push_back("Your phone loses its connection to this node.");
            out.bullets.push_back("Messages still arrive over the mesh.");
        }
        out.keptLabel = "KEPT";
        out.kept = {"The pairing, so no new PIN is needed."};
        out.verb = value ? "TURN ON & RESTART" : "TURN OFF & RESTART";
        out.restarts = true;
        return true;
    }

    case T5Act::SET_WIFI: {
        out.title = value ? "TURN WI-FI ON?" : "TURN WI-FI OFF?";
        out.crumb = std::string("CONNECTIVITY ") + T5Applet::FWD + " WI-FI";
        out.from = config.network.wifi_enabled ? "WI-FI ON" : "WI-FI OFF";
        out.to = value ? "WI-FI ON" : "WI-FI OFF";
        out.bullets = {"The device restarts in about 5 seconds."};
        if (value)
            out.bullets.push_back("Bluetooth is turned off: this board runs one at a time.");
        out.verb = value ? "TURN ON & RESTART" : "TURN OFF & RESTART";
        out.restarts = true;
        return true;
    }

    case T5Act::SET_POWER_SAVING: {
        out.title = value ? "TURN POWER SAVING ON?" : "TURN POWER SAVING OFF?";
        out.crumb = std::string("POWER ") + T5Applet::FWD + " POWER SAVING";
        out.from = config.power.is_power_saving ? "POWER SAVING ON" : "POWER SAVING OFF";
        out.to = value ? "POWER SAVING ON" : "POWER SAVING OFF";
        out.bullets = {"The device restarts in about 5 seconds."};
        if (value) {
            out.bullets.push_back("The battery lasts longer.");
            out.bullets.push_back("Bluetooth may be turned off on this board.");
        }
        out.verb = value ? "TURN ON & RESTART" : "TURN OFF & RESTART";
        out.restarts = true;
        return true;
    }

    case T5Act::CLEAR_NODES: {
        out.title = "CLEAR NODE LIST?";
        out.crumb = std::string("DEVICE ") + T5Applet::FWD + " CLEAR NODE LIST";
        out.bullets = {"The device restarts in about 5 seconds.", "Nodes come back as they are heard again.",
                       "Names and distances are lost until then."};
        out.keptLabel = "KEPT";
        out.kept = {"Channels, your own name and messages."};
        out.choices = {"KEEP FAVOURITES", "EVERYTHING"};
        out.choiceDefault = 0;
        out.verb = "CLEAR & RESTART";
        out.restarts = true;
        return true;
    }

    case T5Act::CLEAR_MESSAGES: {
        out.title = "DELETE ALL MESSAGES?";
        out.crumb = std::string("DEVICE ") + T5Applet::FWD + " CLEAR MESSAGES";
        out.bullets = {"Every message on this device is deleted.", "This cannot be undone from the device.",
                       "New messages still arrive normally."};
        out.keptLabel = "KEPT";
        out.kept = {"Channels, node list and settings."};
        out.verb = "DELETE MESSAGES";
        return true;
    }

    case T5Act::REBOOT: {
        out.title = "REBOOT?";
        out.crumb = "POWER";
        out.bullets = {"Settings are saved, then the device restarts.", "Back in about 10 seconds.",
                       "No messages are received while it restarts."};
        out.verb = "REBOOT";
        out.restarts = true;
        return true;
    }

    case T5Act::SHUT_DOWN: {
        // No FROM -> TO block: nothing changes value (C12)
        out.title = "SHUT DOWN?";
        out.crumb = "POWER";
        out.bullets = {"Settings are saved, then the device turns off.",
                       "The screen keeps showing the last image - that is normal for e-paper.",
                       "No messages are received while it is off."};
        out.keptLabel = "TO START AGAIN";
        out.kept = {"Press the power button."};
        out.verb = "SHUT DOWN";
        return true;
    }

    default:
        return false;
    }
}

// ================================================================ applying

InkHUD::T5ApplyResult InkHUD::t5Apply(T5Act which, int32_t value, uint8_t channelIndex)
{
    switch (which) {

    // ---- restart class --------------------------------------------------------------------
    case T5Act::PICK_REGION:
        applyLoRaRegion((meshtastic_Config_LoRaConfig_RegionCode)value);
        return T5ApplyResult::RESTARTING;

    case T5Act::PICK_PRESET:
        applyLoRaPreset((meshtastic_Config_LoRaConfig_ModemPreset)value);
        return T5ApplyResult::RESTARTING;

    case T5Act::PICK_ROLE:
        applyDeviceRole((meshtastic_Config_DeviceConfig_Role)value);
        return T5ApplyResult::RESTARTING;

    case T5Act::SET_BLUETOOTH:
        config.bluetooth.enabled = value != 0;
        if (config.bluetooth.enabled)
            config.network.wifi_enabled = false; // One radio at a time on this board
        if (!nodeDB->saveToDisk(SEGMENT_CONFIG))
            return T5ApplyResult::FAILED;
        scheduleRestart();
        return T5ApplyResult::RESTARTING;

    case T5Act::SET_WIFI:
        config.network.wifi_enabled = value != 0;
        if (config.network.wifi_enabled)
            config.bluetooth.enabled = false;
        if (!nodeDB->saveToDisk(SEGMENT_CONFIG))
            return T5ApplyResult::FAILED;
        scheduleRestart();
        return T5ApplyResult::RESTARTING;

    case T5Act::SET_POWER_SAVING:
        config.power.is_power_saving = value != 0;
        if (!nodeDB->saveToDisk(SEGMENT_CONFIG))
            return T5ApplyResult::FAILED;
        scheduleRestart();
        return T5ApplyResult::RESTARTING;

    case T5Act::CLEAR_NODES:
        // The confirmation's choice cells, in frozen order: 0 = KEEP FAVOURITES, 1 = EVERYTHING.
        // resetNodes() takes the number of favourites to keep, so the two are inverses
        InkHUD::InkHUD::getInstance()->notifyApplyingChanges();
        nodeDB->resetNodes(value == 0 ? 1 : 0);
        rebootAtMsec = Time::timerEndsAtMillis(DEFAULT_REBOOT_SECONDS * 1000);
        return T5ApplyResult::RESTARTING;

    case T5Act::REBOOT:
        scheduleRestart();
        return T5ApplyResult::RESTARTING;

    case T5Act::SHUT_DOWN:
        // The terminal frame stays owned by the established power lifecycle: T5SystemLayer draws
        // SWITCHED OFF from onShutdown(), as it has since Phase 3
        shutdownAtMsec = Time::skipZero(Time::getMillis());
        return T5ApplyResult::RESTARTING;

    // ---- live, persisted ------------------------------------------------------------------
    case T5Act::PICK_HOP:
        if (config.lora.hop_limit == (uint8_t)value)
            return T5ApplyResult::DONE; // Nothing changed, so nothing is written
        config.lora.hop_limit = (uint8_t)value;
        return saveAndReload() ? T5ApplyResult::DONE : T5ApplyResult::FAILED;

    case T5Act::SET_TRANSMIT:
        if (config.lora.tx_enabled == (value != 0))
            return T5ApplyResult::DONE;
        config.lora.tx_enabled = value != 0;
        return saveAndReload() ? T5ApplyResult::DONE : T5ApplyResult::FAILED;

    case T5Act::SET_GPS: {
#if !MESHTASTIC_EXCLUDE_GPS && HAS_GPS
        const auto wanted =
            value ? meshtastic_Config_PositionConfig_GpsMode_ENABLED : meshtastic_Config_PositionConfig_GpsMode_DISABLED;
        if (config.position.gps_mode == meshtastic_Config_PositionConfig_GpsMode_NOT_PRESENT ||
            config.position.gps_mode == wanted)
            return T5ApplyResult::DONE;
        config.position.gps_mode = wanted;
        return saveAndReload() ? T5ApplyResult::DONE : T5ApplyResult::FAILED;
#else
        return T5ApplyResult::DONE;
#endif
    }

    case T5Act::SET_FIXED_POSITION: {
        if (config.position.fixed_position == (value != 0))
            return T5ApplyResult::DONE;
        if (value && !havePosition())
            return T5ApplyResult::FAILED; // The row is blocked, but never trust the caller
        config.position.fixed_position = value != 0;
        // Turning it on pins us where we are now. NodeDB already holds that position; the fixed flag
        // is what stops it being replaced, so only the flag has to be written
        return saveAndReload() ? T5ApplyResult::DONE : T5ApplyResult::FAILED;
    }

    case T5Act::SET_SMART_POSITION:
        if (config.position.position_broadcast_smart_enabled == (value != 0))
            return T5ApplyResult::DONE;
        config.position.position_broadcast_smart_enabled = value != 0;
        return saveAndReload() ? T5ApplyResult::DONE : T5ApplyResult::FAILED;

    case T5Act::PICK_BROADCAST:
        if (config.position.position_broadcast_secs == (uint32_t)value)
            return T5ApplyResult::DONE;
        config.position.position_broadcast_secs = (uint32_t)value;
        return saveAndReload() ? T5ApplyResult::DONE : T5ApplyResult::FAILED;

    case T5Act::PICK_TZ:
        if (value < 0 || value >= TIMEZONE_COUNT)
            return T5ApplyResult::DONE;
        if (strcmp(config.device.tzdef, TIMEZONES[value].tzdef) == 0)
            return T5ApplyResult::DONE;
        strncpy(config.device.tzdef, TIMEZONES[value].tzdef, sizeof(config.device.tzdef));
        config.device.tzdef[sizeof(config.device.tzdef) - 1] = '\0';
        setenv("TZ", config.device.tzdef, 1);
        return saveAndReload() ? T5ApplyResult::DONE : T5ApplyResult::FAILED;

    case T5Act::PICK_PIN_MODE:
    case T5Act::SET_PIN_MODE: {
        // The mode only: fixed_pin is the PIN the app chose, and writing a 0 / 1 here would replace it
        const auto wanted = value ? meshtastic_Config_BluetoothConfig_PairingMode_FIXED_PIN
                                  : meshtastic_Config_BluetoothConfig_PairingMode_RANDOM_PIN;
        if (config.bluetooth.mode == wanted)
            return T5ApplyResult::DONE;
        config.bluetooth.mode = wanted;
        return nodeDB->saveToDisk(SEGMENT_CONFIG) ? T5ApplyResult::DONE : T5ApplyResult::FAILED;
    }

    case T5Act::SET_UNITS: {
        // The canonical display-unit setting, which Map and Node detail already read. No T5 copy
        const auto wanted =
            value ? meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL : meshtastic_Config_DisplayConfig_DisplayUnits_METRIC;
        if (config.display.units == wanted)
            return T5ApplyResult::DONE;
        config.display.units = wanted;
        return nodeDB->saveToDisk(SEGMENT_CONFIG) ? T5ApplyResult::DONE : T5ApplyResult::FAILED;
    }

    case T5Act::SET_CLOCK:
        if (config.display.use_12h_clock == (value != 0))
            return T5ApplyResult::DONE;
        config.display.use_12h_clock = value != 0;
        return nodeDB->saveToDisk(SEGMENT_CONFIG) ? T5ApplyResult::DONE : T5ApplyResult::FAILED;

    case T5Act::PICK_SLEEP: {
        if (config.display.screen_on_secs == (uint32_t)value)
            return T5ApplyResult::DONE;
        config.display.screen_on_secs = (uint32_t)value;
        const bool saved = nodeDB->saveToDisk(SEGMENT_CONFIG);
        // Live, through the one timeout owner. Zero genuinely disables it: nothing is armed
        t5ScreenTimeoutApply();
        return saved ? T5ApplyResult::DONE : T5ApplyResult::FAILED;
    }

    // ---- channel settings ----------------------------------------------------------------
    case T5Act::SET_SHARE_POSITION: {
        meshtastic_Channel &ch = channels.getByIndex(channelIndex);
        ch.settings.has_module_settings = true;
        const uint32_t wanted = value ? DEFAULT_PRECISION_BITS : 0;
        if (ch.settings.module_settings.position_precision == wanted)
            return T5ApplyResult::DONE;
        ch.settings.module_settings.position_precision = wanted;
        return saveAndReload(SEGMENT_CHANNELS) ? T5ApplyResult::DONE : T5ApplyResult::FAILED;
    }

    case T5Act::PICK_PRECISION: {
        meshtastic_Channel &ch = channels.getByIndex(channelIndex);
        ch.settings.has_module_settings = true;
        if (ch.settings.module_settings.position_precision == (uint32_t)value)
            return T5ApplyResult::DONE;
        ch.settings.module_settings.position_precision = (uint32_t)value;
        return saveAndReload(SEGMENT_CHANNELS) ? T5ApplyResult::DONE : T5ApplyResult::FAILED;
    }

    case T5Act::SET_UPLINK: {
        meshtastic_Channel &ch = channels.getByIndex(channelIndex);
        if (ch.settings.uplink_enabled == (value != 0))
            return T5ApplyResult::DONE;
        ch.settings.uplink_enabled = value != 0;
        return saveAndReload(SEGMENT_CHANNELS) ? T5ApplyResult::DONE : T5ApplyResult::FAILED;
    }

    case T5Act::SET_DOWNLINK: {
        meshtastic_Channel &ch = channels.getByIndex(channelIndex);
        if (ch.settings.downlink_enabled == (value != 0))
            return T5ApplyResult::DONE;
        ch.settings.downlink_enabled = value != 0;
        return saveAndReload(SEGMENT_CHANNELS) ? T5ApplyResult::DONE : T5ApplyResult::FAILED;
    }

    // ---- T5-local, no shared config -------------------------------------------------------
    case T5Act::SET_MODE:
        // The one canonical mode-change path, which persists and does the single FULL refresh itself
        if ((value ? T5Mode::CONSOLE : T5Mode::CARRY) != t5CurrentMode())
            t5SetMode(value ? T5Mode::CONSOLE : T5Mode::CARRY);
        return T5ApplyResult::DONE;

    case T5Act::SET_FRONT_LIGHT:
        if (t5BacklightIsUserEnabled() != (value != 0))
            t5BacklightSetUserEnabled(value != 0);
        return T5ApplyResult::DONE;

    case T5Act::SET_TOUCH:
        // Not persisted: touch is always on at boot, which is a safety net and not the recovery path
        if (t5TouchPreferenceEnabled() != (value != 0))
            setTouchInputEnabled(value != 0, false);
        return T5ApplyResult::DONE;

    case T5Act::CLEAN_SCREEN:
        // An explicit user request for a FULL, to clear ghosting
        InkHUD::InkHUD::getInstance()->forceUpdate(Drivers::EInk::UpdateTypes::FULL, true);
        return T5ApplyResult::DONE;

    case T5Act::CLEAR_MESSAGES: {
        messageStore.clearAllMessages();
        InkHUD::InkHUD::getInstance()->persistence->loadLatestMessage();
        // The model's unread counts are RAM state keyed by conversation: with the store empty they
        // would otherwise go on claiming messages that no longer exist
        if (T5MessageModel *model = T5MessageModel::get()) {
            for (const T5MessageModel::Convo &c : model->conversations())
                model->markSeen(c);
            model->clearTag();
        }
        return T5ApplyResult::DONE;
    }

    default:
        return T5ApplyResult::DONE;
    }
}

// ================================================================ owner names

std::string InkHUD::t5OwnerName(bool shortName)
{
    return t5Parse(shortName ? owner.short_name : owner.long_name);
}

size_t InkHUD::t5OwnerNameLimit(bool shortName)
{
    // The field caps the short name at four characters, as the frozen row says
    return shortName ? 4 : sizeof(owner.long_name) - 1;
}

InkHUD::T5ApplyResult InkHUD::t5SetOwnerName(bool shortName, const std::string &utf8)
{
    // The admin path rejects a name that is only whitespace; so do we, before writing anything
    bool hasText = false;
    for (char c : utf8)
        if (!isspace((unsigned char)c))
            hasText = true;
    if (!hasText)
        return T5ApplyResult::FAILED;

    // Exactly what AdminModule::handleSetOwner does to the same field, so a name typed here and a
    // name set from the phone end up identically clamped and sanitised
    bool changed;
    if (shortName) {
        char value[sizeof(owner.short_name)];
        strncpy(value, utf8.c_str(), sizeof(value));
        value[sizeof(value) - 1] = '\0';
        sanitizeUtf8(value, sizeof(value));
        changed = strcmp(owner.short_name, value) != 0;
        memcpy(owner.short_name, value, sizeof(owner.short_name));
    } else {
        char value[sizeof(owner.long_name)];
        strncpy(value, utf8.c_str(), sizeof(value));
        value[sizeof(value) - 1] = '\0';
        clampLongName(value);
        changed = strcmp(owner.long_name, value) != 0;
        memcpy(owner.long_name, value, sizeof(owner.long_name));
    }

    if (!changed)
        return T5ApplyResult::DONE; // Nothing really changed: no flash write, no NodeInfo broadcast

    // reloadOwner re-announces us and writes our own NodeDB entry; the segments match the admin path
    service->reloadOwner(false);
    return nodeDB->saveToDisk(SEGMENT_DEVICESTATE | SEGMENT_NODEDATABASE) ? T5ApplyResult::DONE : T5ApplyResult::FAILED;
}

#endif
