#pragma once

#include "DebugConfiguration.h"
#include "NodeDB.h"
#include "configuration.h"
#include "mesh/generated/meshtastic/telemetry.pb.h"
#include "sleep.h"

class BaseTelemetryModule
{
  public:
    /*
      The last telemetry this module measured from hardware attached to THIS node.

      Each telemetry module is the authoritative owner of its own local readings: it is the only
      thing that reads those sensors, so it is the only thing that can say what they last said and
      when. Recorded at the point the reading is produced, which is also where the measurement
      timestamp is generated, so `time` is the measurement's own and not the time of a later read.

      Distinct from the display slot a module may also keep, which follows a display-source setting
      and can legitimately hold another node's telemetry. Nothing remote reaches this.
    */

    /// True once this module has produced at least one local reading. False means "no local
    /// measurement yet", not "no sensor" - a caller that needs the latter checks sensor detection.
    bool hasLocalMeasurement() const { return localMeasurementValid; }

    /// Copies the last local reading, with its original measurement timestamp, into `out`. False
    /// when none has been taken yet, leaving `out` untouched. Reads retained state only: it never
    /// touches a sensor, starts a measurement or moves a broadcast.
    bool getLastLocalMeasurement(meshtastic_Telemetry &out) const
    {
        if (!localMeasurementValid)
            return false;
        out = localMeasurement;
        return true;
    }

  protected:
    /// Called by a subclass at the point it has just read its own sensors, with the telemetry it
    /// produced. Only a locally measured reading belongs here.
    void recordLocalMeasurement(const meshtastic_Telemetry &m)
    {
        localMeasurement = m;
        localMeasurementValid = true;
    }

  protected:
    bool isSensorOrRouterRole() const
    {
        return config.device.role == meshtastic_Config_DeviceConfig_Role_SENSOR ||
               config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
               config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
    }

    /// True for a SENSOR role node with power saving enabled, the only combination that deep
    /// sleeps between telemetry broadcasts
    bool isPowerSavingSensor() const
    {
        return config.device.role == meshtastic_Config_DeviceConfig_Role_SENSOR && config.power.is_power_saving;
    }

    /**
     * Call while a deep sleep is pending (sleepOnNextExecution): the telemetry packet queued by
     * sendTelemetry() goes out asynchronously, and sleeping while it is still queued or on air
     * truncates the transmission. Returns true if the caller should reschedule in
     * PREFLIGHT_SLEEP_RETRY_MS and check again. Bounded by MAX_PREFLIGHT_SLEEP_DEFERRALS so a
     * busy mesh can't keep the node awake forever. Reset preflightSleepDeferrals to 0 whenever
     * sleepOnNextExecution is armed.
     */
    bool shouldDeferDeepSleep()
    {
        if (doPreflightSleep(true) || preflightSleepDeferrals >= MAX_PREFLIGHT_SLEEP_DEFERRALS)
            return false;
        preflightSleepDeferrals++;
        LOG_DEBUG("Radio busy, defer deep sleep");
        return true;
    }

    // While sleepOnNextExecution is pending, counts how often the deep sleep was postponed
    // because doPreflightSleep() vetoed it (e.g. radio still transmitting)
    uint32_t preflightSleepDeferrals = 0;

  private:
    bool localMeasurementValid = false;
    meshtastic_Telemetry localMeasurement = meshtastic_Telemetry_init_zero;
};
