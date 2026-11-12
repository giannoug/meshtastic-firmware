#if defined(T5_S3_EPAPER_PRO) && defined(MESHTASTIC_INCLUDE_INKHUD)

/*

The two read-only tools: RF STATUS (Apps R1, K5) and SENSORS (S1, S2, K6).

Both read firmware state and keep none of their own. RF Status reads airTime and the same radio and
router counters the local-stats telemetry is built from, rather than a copy of them. Sensors reads
each telemetry module's own last local measurement, including the timestamp taken when the sensors
were actually read - not the time we happened to look, and never a remote node's reading, which
belongs to Node detail (Apps 10, 18).

Nothing here polls on a short timer. The applet's tick is 60 s and a value redraw is skipped
entirely when the formatted text has not changed (Apps 12, 16).

*/

#include "./T5AppsApplet.h"
#include "./T5Text.h"

#include "NodeStatus.h"
#include "UptimeClock.h"
#include "airtime.h"
#include "gps/RTC.h"
#include "mesh/Default.h"
#include "mesh/NodeDB.h"
#include "mesh/RadioLibInterface.h"
#include "mesh/Router.h"
#include "modules/Telemetry/Sensor/TelemetrySensor.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR
#include "modules/Telemetry/EnvironmentTelemetry.h"
#endif
#if !MESHTASTIC_EXCLUDE_AIR_QUALITY_SENSOR
#include "modules/Telemetry/AirQualityTelemetry.h"
#endif
#if !MESHTASTIC_EXCLUDE_POWER_TELEMETRY
#include "modules/Telemetry/PowerTelemetry.h"
#endif

using namespace NicheGraphics;

namespace
{
using std::to_string;

constexpr int16_t RF_PANE = 464; // Console: numbers left, history right (Apps K5)

// The detected sensor's own name, for the frozen section label "ENVIRONMENT \xB7 BME280". The names
// are the protobuf sensor-type spellings, which is what the frozen copy uses
const char *const SENSOR_NAMES[] = {
    "BME280",   "BME680",        "MCP9808",  "INA260",     "INA219",      "BMP280",       "SHTC3",     "LPS22",        "QMC6310",
    "QMI8658",  "QMC5883L",      "SHT31",    "PMSA003I",   "INA3221",     "BMP085",       "RCWL9620",  "SHT4X",        "VEML7700",
    "MLX90632", "OPT3001",       "LTR390UV", "TSL25911FN", "AHT10",       "DFROBOT_LARK", "NAU7802",   "BMP3XX",       "ICM20948",
    "MAX17048", "CUSTOM_SENSOR", "MAX30102", "MLX90614",   "SCD4X",       "RADSENS",      "INA226",    "DFROBOT_RAIN", "DPS310",
    "RAK12035", "MAX17261",      "PCT2075",  "ADS1X15",    "ADS1X15_ALT", "SFA30",        "SEN5X",     "TSL2561",      "BH1750",
    "HDC1080",  "SHT21",         "STC31",    "SCD30",      "SHTXX",       "DS248X",       "MMC5983MA", "ICM42607P",    "SPA06",
    "HM330X",   "SEN6X",         "AS3935",
};

// Frozen number style: a space every three digits, so "4 812" rather than "4812"
std::string grouped(uint32_t n)
{
    std::string digits = to_string(n), out;
    for (size_t i = 0; i < digits.size(); i++) {
        if (i && (digits.size() - i) % 3 == 0)
            out += ' ';
        out += digits[i];
    }
    return out;
}

std::string uptimeText(uint32_t secs)
{
    char out[24];
    if (secs >= SEC_PER_DAY)
        snprintf(out, sizeof(out), "%u D %02u H", (unsigned)(secs / SEC_PER_DAY),
                 (unsigned)((secs % SEC_PER_DAY) / SEC_PER_HOUR));
    else
        snprintf(out, sizeof(out), "%02u H %02u M", (unsigned)(secs / SEC_PER_HOUR),
                 (unsigned)((secs % SEC_PER_HOUR) / SEC_PER_MIN));
    return out;
}

std::string pct(float v, int decimals)
{
    char out[16];
    snprintf(out, sizeof(out), decimals ? "%.1f %%" : "%.0f %%", (double)v);
    return out;
}

// A sensor type's display name, or empty when it is one we have no spelling for
std::string sensorName(uint8_t type)
{
    if (type == 0 || type > sizeof(SENSOR_NAMES) / sizeof(SENSOR_NAMES[0]))
        return "";
    const char *name = SENSOR_NAMES[type - 1];
    return name ? name : "";
}

// Degrees in the unit the user already chose for the rest of the UI. No unit control lives in Apps
std::string temperature(float celsius)
{
    char out[16];
    if (config.display.units == meshtastic_Config_DisplayConfig_DisplayUnits_IMPERIAL)
        snprintf(out, sizeof(out),
                 "%.1f \xB0"
                 "F",
                 (double)(celsius * 9.0f / 5.0f + 32.0f));
    else
        snprintf(out, sizeof(out),
                 "%.1f \xB0"
                 "C",
                 (double)celsius);
    return out;
}

std::string fixed(float v, int decimals, const char *unit)
{
    char out[24];
    snprintf(out, sizeof(out), "%.*f %s", decimals, (double)v, unit);
    return out;
}
} // namespace

// ---------------------------------------------------------------- RF status

void InkHUD::T5AppsApplet::renderRf()
{
    const bool console = isConsole();
    const std::string clock = getTimeString();
    drawHeader("RF STATUS", clock.empty() ? "READ JUST NOW" : "UPDATED " + clock, "APPS");

    // Everything below is read straight from the firmware's own counters: no second store, and no
    // value is invented when its source is absent (Apps 15)
    const float chanUtil = airTime ? airTime->channelUtilizationPercent() : 0.0f;
    const float txUtil = airTime ? airTime->utilizationTXPercent() : 0.0f;

    std::vector<Reading> counters;
    if (RadioLibInterface::instance) {
        counters.push_back({"HEARD", grouped(RadioLibInterface::instance->rxGood)});
        counters.push_back({"BAD", grouped(RadioLibInterface::instance->rxBad)});
    } else {
        // No radio interface on this build: the frozen rule is to show the unavailable mark, never a
        // fabricated zero
        counters.push_back({"HEARD", DASH});
        counters.push_back({"BAD", DASH});
    }
    counters.push_back({console ? "DUPES" : "DUPLICATES", router ? grouped(router->rxDupe) : std::string(DASH)});
    counters.push_back({"SENT", RadioLibInterface::instance ? grouped(RadioLibInterface::instance->txGood) : std::string(DASH)});
    counters.push_back(
        {"RELAYED", RadioLibInterface::instance ? grouped(RadioLibInterface::instance->txRelay) : std::string(DASH)});
    counters.push_back({console ? "ONLINE" : "NODES ONLINE",
                        to_string(nodeStatus ? nodeStatus->getNumOnline() : 0) + " / " + to_string(nodeDB->getNumMeshNodes())});

    // The hourly log: eight buckets, newest first, copied out by the owner rather than handed over
    uint32_t rx[AirTime::getPeriodsToLog()] = {0}, tx[AirTime::getPeriodsToLog()] = {0};
    bool haveHistory = false;
    if (airTime)
        haveHistory = airTime->airtimeReport(RX_LOG, rx, AirTime::getPeriodsToLog()) &&
                      airTime->airtimeReport(TX_LOG, tx, AirTime::getPeriodsToLog());
    uint32_t peak = 1;
    for (uint8_t i = 0; i < AirTime::getPeriodsToLog(); i++)
        peak = std::max(peak, std::max(rx[i], tx[i]));

    // Slot 0 is the hour in progress, and each later slot is one hour older
    const uint32_t now = getValidTime(RTCQuality::RTCQualityDevice, true);
    auto hourLabel = [&](uint8_t age) {
        char out[16];
        if (!now) {
            snprintf(out, sizeof(out), "-%u H", (unsigned)age);
            return std::string(out);
        }
        const int hour = (int)((now % SEC_PER_DAY) / SEC_PER_HOUR) - age;
        const int from = ((hour % 24) + 24) % 24, to = (from + 1) % 24;
        snprintf(out, sizeof(out), "%02d\x96%02d", from, to); // En dash: the frozen "13-14"
        return std::string(out);
    };

    auto drawNow = [&](int16_t &y) {
        rowSection(y, "NOW");
        rowInfo(y, console ? "CHANNEL USE " + std::string(SEP) + " 1 MIN" : "CHANNEL USE " + std::string(SEP) + " LAST MINUTE",
                pct(chanUtil, 0));
        rowInfo(y, console ? "MY AIRTIME " + std::string(SEP) + " 1 H" : "MY AIRTIME " + std::string(SEP) + " LAST HOUR",
                pct(txUtil, 1));
        rowSection(y, "SINCE BOOT " + std::string(SEP) + " " + uptimeText(Time::getUptimeSecs()));
        rowFacts(y, counters, console ? 3 : 2, false);
    };

    auto drawHistory = [&](int16_t &y) {
        rowSection(y, "AIRTIME BY HOUR");
        if (!haveHistory) {
            rowNote(y, "No airtime history yet.");
            return;
        }
        rowBarHeader(y);
        for (uint8_t i = 0; i < AirTime::getPeriodsToLog(); i++) {
            if (y + 34 > bodyBottom())
                break;
            rowBar(y, hourLabel(i), rx[i], tx[i], peak);
        }
    };

    if (console) {
        int16_t y = headerHeight();
        setPane(0, RF_PANE);
        drawNow(y);
        clearPane();
        drawLine(RF_PANE, headerHeight(), RF_PANE, navTop() - 1, BLACK);
        int16_t hy = headerHeight();
        setPane(RF_PANE + 2, width() - RF_PANE - 2);
        drawHistory(hy);
        clearPane();
    } else {
        int16_t y = headerHeight();
        drawNow(y);
        drawHistory(y);
    }

    drawNav();
}

// The figures the RF screen puts on the panel, formatted the same way. Compared on the tick: if
// this string is unchanged there is nothing new to show, so no frame is requested (Apps 12)
std::string InkHUD::T5AppsApplet::rfSignature()
{
    if (!airTime)
        return "";
    std::string out = pct(airTime->channelUtilizationPercent(), 0) + pct(airTime->utilizationTXPercent(), 1);
    if (RadioLibInterface::instance)
        out += grouped(RadioLibInterface::instance->rxGood) + grouped(RadioLibInterface::instance->rxBad) +
               grouped(RadioLibInterface::instance->txGood) + grouped(RadioLibInterface::instance->txRelay);
    if (router)
        out += grouped(router->rxDupe);
    return out;
}

std::string InkHUD::T5AppsApplet::sensorSignature()
{
    std::string out;
    for (const Section &s : sensorSections()) {
        out += s.label;
        for (const Reading &r : s.readings)
            out += r.value;
    }
    return out;
}

// ---------------------------------------------------------------- sensors

// One section per detected sensor, one cell per field it actually reports. A field the sensor cannot
// measure gets no cell at all rather than an empty one (Apps S1)
std::vector<InkHUD::T5AppsApplet::Section> InkHUD::T5AppsApplet::sensorSections() const
{
    std::vector<Section> out;

    // Which sensor a module is reading. The detection map is the firmware's own, so a sensor that
    // failed to open has already been removed from it
    auto detectedName = [](const uint8_t *types, size_t count) {
        for (size_t i = 0; i < count; i++)
            if (nodeTelemetrySensorsMap[types[i]].first > 0)
                return sensorName(types[i]);
        return std::string();
    };

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR
    if (environmentTelemetryModule) {
        static const uint8_t envTypes[] = {
            meshtastic_TelemetrySensorType_BME280, meshtastic_TelemetrySensorType_BME680,  meshtastic_TelemetrySensorType_BMP280,
            meshtastic_TelemetrySensorType_BMP3XX, meshtastic_TelemetrySensorType_BMP085,  meshtastic_TelemetrySensorType_SHTC3,
            meshtastic_TelemetrySensorType_SHT31,  meshtastic_TelemetrySensorType_SHT4X,   meshtastic_TelemetrySensorType_SHT21,
            meshtastic_TelemetrySensorType_AHT10,  meshtastic_TelemetrySensorType_MCP9808, meshtastic_TelemetrySensorType_LPS22,
            meshtastic_TelemetrySensorType_DPS310, meshtastic_TelemetrySensorType_SPA06,   meshtastic_TelemetrySensorType_HDC1080,
            meshtastic_TelemetrySensorType_SCD4X,  meshtastic_TelemetrySensorType_PCT2075, meshtastic_TelemetrySensorType_SFA30,
        };
        const std::string name = detectedName(envTypes, sizeof(envTypes) / sizeof(envTypes[0]));
        if (!name.empty()) {
            Section s;
            s.label = "ENVIRONMENT " + std::string(SEP) + " " + name;
            meshtastic_Telemetry m;
            if (environmentTelemetryModule->getLastLocalMeasurement(m) &&
                m.which_variant == meshtastic_Telemetry_environment_metrics_tag) {
                const meshtastic_EnvironmentMetrics &e = m.variant.environment_metrics;
                if (e.has_temperature)
                    s.readings.push_back({"TEMPERATURE", temperature(e.temperature)});
                if (e.has_relative_humidity)
                    s.readings.push_back({"HUMIDITY", fixed(e.relative_humidity, 0, "%")});
                if (e.has_barometric_pressure)
                    s.readings.push_back({"PRESSURE", fixed(e.barometric_pressure, 0, "hPa")});
                if (e.has_gas_resistance)
                    s.readings.push_back(
                        {"GAS", fixed(e.gas_resistance * 1000.0f, 0, "k\xD9")}); // Source is MOhm; the frozen cell is kilo-ohms
                if (e.has_lux)
                    s.readings.push_back({"LIGHT", fixed(e.lux, 0, "lx")});
                if (e.has_iaq)
                    s.readings.push_back({"AIR QUALITY", to_string(e.iaq)});
                if (e.has_distance)
                    s.readings.push_back({"DISTANCE", fixed(e.distance, 0, "mm")});
                if (e.has_weight)
                    s.readings.push_back({"WEIGHT", fixed(e.weight, 2, "kg")});
                if (e.has_radiation)
                    s.readings.push_back({"RADIATION", fixed(e.radiation, 2, "uR/h")});
                if (e.has_soil_temperature)
                    s.readings.push_back({"SOIL TEMP", temperature(e.soil_temperature)});
                if (e.has_soil_moisture)
                    s.readings.push_back({"SOIL MOISTURE", to_string(e.soil_moisture) + " %"});
            }
            out.push_back(s); // Readings may be empty: found, nothing measured yet (S2)
        }
    }
#endif

#if !MESHTASTIC_EXCLUDE_AIR_QUALITY_SENSOR
    if (airQualityTelemetryModule) {
        static const uint8_t aqTypes[] = {
            meshtastic_TelemetrySensorType_PMSA003I,
            meshtastic_TelemetrySensorType_SEN5X,
            meshtastic_TelemetrySensorType_HM330X,
            meshtastic_TelemetrySensorType_SEN6X,
        };
        const std::string name = detectedName(aqTypes, sizeof(aqTypes) / sizeof(aqTypes[0]));
        if (!name.empty()) {
            Section s;
            s.label = "AIR QUALITY " + std::string(SEP) + " " + name;
            meshtastic_Telemetry m;
            if (airQualityTelemetryModule->getLastLocalMeasurement(m) &&
                m.which_variant == meshtastic_Telemetry_air_quality_metrics_tag) {
                const meshtastic_AirQualityMetrics &a = m.variant.air_quality_metrics;
                if (a.has_pm10_standard)
                    s.readings.push_back({"PM1.0", to_string(a.pm10_standard) + " ug/m3"});
                if (a.has_pm25_standard)
                    s.readings.push_back({"PM2.5", to_string(a.pm25_standard) + " ug/m3"});
                if (a.has_pm100_standard)
                    s.readings.push_back({"PM10", to_string(a.pm100_standard) + " ug/m3"});
                if (a.has_co2)
                    s.readings.push_back({"CO2", to_string(a.co2) + " ppm"});
            }
            out.push_back(s);
        }
    }
#endif

#if !MESHTASTIC_EXCLUDE_POWER_TELEMETRY
    if (powerTelemetryModule) {
        static const uint8_t pwrTypes[] = {
            meshtastic_TelemetrySensorType_INA219,   meshtastic_TelemetrySensorType_INA226,
            meshtastic_TelemetrySensorType_INA260,   meshtastic_TelemetrySensorType_INA3221,
            meshtastic_TelemetrySensorType_MAX17048, meshtastic_TelemetrySensorType_MAX17261,
        };
        const std::string name = detectedName(pwrTypes, sizeof(pwrTypes) / sizeof(pwrTypes[0]));
        if (!name.empty()) {
            Section s;
            s.label = "POWER " + std::string(SEP) + " " + name;
            meshtastic_Telemetry m;
            if (powerTelemetryModule->getLastLocalMeasurement(m) && m.which_variant == meshtastic_Telemetry_power_metrics_tag) {
                const meshtastic_PowerMetrics &p = m.variant.power_metrics;
                if (p.has_ch1_voltage)
                    s.readings.push_back({"CH 1 VOLTAGE", fixed(p.ch1_voltage, 2, "V")});
                if (p.has_ch1_current)
                    s.readings.push_back({"CH 1 CURRENT", fixed(p.ch1_current / 1000.0f, 2, "A")});
                if (p.has_ch2_voltage)
                    s.readings.push_back({"CH 2 VOLTAGE", fixed(p.ch2_voltage, 2, "V")});
                if (p.has_ch2_current)
                    s.readings.push_back({"CH 2 CURRENT", fixed(p.ch2_current / 1000.0f, 2, "A")});
                if (p.has_ch3_voltage)
                    s.readings.push_back({"CH 3 VOLTAGE", fixed(p.ch3_voltage, 2, "V")});
                if (p.has_ch3_current)
                    s.readings.push_back({"CH 3 CURRENT", fixed(p.ch3_current / 1000.0f, 2, "A")});
            }
            out.push_back(s);
        }
    }
#endif

    return out;
}

void InkHUD::T5AppsApplet::renderSensors()
{
    const bool console = isConsole();
    const std::vector<Section> sections = sensorSections();

    // The context line carries the measurement's own time, which is not the time it was last sent
    uint32_t measured = 0;
#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR
    if (environmentTelemetryModule) {
        meshtastic_Telemetry m;
        if (environmentTelemetryModule->getLastLocalMeasurement(m))
            measured = m.time;
    }
#endif
    const uint32_t everyMin = Default::getConfiguredOrDefaultMs(moduleConfig.telemetry.environment_update_interval,
                                                                default_telemetry_broadcast_interval_secs) /
                              60000U;
    std::string context;
    if (measured) {
        const std::string when = t5ListTime(measured, false);
        context = "MEASURED " + (when.empty() ? std::string("RECENTLY") : when);
        if (everyMin)
            context +=
                std::string(" ") + SEP + (console ? " SENT EVERY " : " SENT TO MESH EVERY ") + to_string(everyMin) + " MIN";
    } else if (!sections.empty())
        context = sections.front().label.substr(sections.front().label.find_last_of(' ') + 1) + " FOUND";
    else
        context = "NOT AVAILABLE";

    drawHeader("SENSORS", context, "APPS");
    int16_t y = headerHeight();

    if (sections.empty()) {
        // The tool was opened and its source has nothing to give: the frozen generic failure (X1)
        rowBig(y, "Sensors could not open",
               "The sensor stopped responding. Check the connection. This tool disappears from APPS after the next restart "
               "if the sensor is gone.",
               100);
        rowAction(y, Hit::BACK_TO_APPS, "BACK TO APPS", false);
        drawNav();
        return;
    }

    for (const Section &s : sections) {
        if (y >= bodyBottom())
            break;
        rowSection(y, s.label);
        if (s.readings.empty()) {
            // S2: the module and the sensor are both there, but nothing has been measured yet
            rowBig(y, "Waiting for first reading", "Sensor found. The first reading appears here at the next sensor update.", 60);
            continue;
        }
        rowFacts(y, s.readings, console ? 4 : 2, true);
    }

    if (y < bodyBottom())
        rowNote(y, console ? "Showing latest readings only."
                           : "Showing latest readings only. Units follow MENU " + std::string(FWD) + " DISPLAY.");

    drawNav();
}

#endif
