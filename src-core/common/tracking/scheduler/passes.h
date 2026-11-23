#pragma once

#include <vector>
#include "common/tracking/tle.h"

namespace satdump
{
    struct SatellitePass
    {
        int norad;
        double aos_time;
        double los_time;
        float max_elevation;
    };

    // RDAS slots are phase-locked to 00:00 UTC + rdas_slot_offset_seconds (per satellite);
    // rdas_skip_xrit_slots skips the 3-hourly LRIT/HRIT (:14/:44 UTC) slots RDAS cannot use.
    std::vector<SatellitePass> getPassesForSatellite(int norad, double initial_time, double timespan, double qth_lon, double qth_lat, double qth_alt, std::vector<SatellitePass> premade_passes = {}, int rdas_slot_seconds = 900, int rdas_pass_seconds = 600, int rdas_slot_offset_seconds = -15, bool rdas_skip_xrit_slots = true);

    std::vector<SatellitePass> filterPassesByElevation(std::vector<SatellitePass> passes, float min_elevation, float max_elevation);

    std::vector<SatellitePass> selectPassesForAutotrack(std::vector<SatellitePass> passes);
}
