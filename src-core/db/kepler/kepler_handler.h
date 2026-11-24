#pragma once

#include "common/tracking/tle.h"
#include "core/exception.h"
#include "db/db_handler.h"
#include "logger.h"
#include <mutex>
#include <vector>

namespace satdump
{
    // Fields are zero-initialized: a row parsed from a malformed feed used to leave whatever was
    // on the stack (notably epoch) and that garbage got persisted.
    struct KeplerData
    {
        int satellite_number = 0;                 // Satellite number
        long element_number = 0;                  // Element number
        std::string name;                         // Satellite Name
        std::string designator;                   // International designator
        double epoch = 0;                         // Epoch (Unix Time)
        double inclination = 0;                   // Inclination
        double right_ascension = 0;               // Right Ascension of the Ascending Node [Degrees]
        double eccentricity = 0;                  // Eccentricity
        double argument_of_perigee = 0;           // Argument of Perigee [Degrees]
        double mean_anomaly = 0;                  // Mean Anomaly [Degrees]
        double mean_motion = 0;                   // Mean Motion [Revs per day]
        double derivative_mean_motion = 0;        // First Time Derivative of the Mean Motion divided by two
        double second_derivative_mean_motion = 0; // Second Time Derivative of Mean Motion divided by six
        double bstar_drag_term = 0;               // BSTAR drag term
        int revolutions_at_epoch = 0;             // Number of revolutions around Earth at epoch
    };

    TLE keplerToTle(KeplerData kep);
    bool ccsdsOmmToKepler(std::string omm_line, KeplerData &kep_out);
    bool tleToKepler(TLE tle, KeplerData &kep_out);

    std::vector<KeplerData> parseCcsdsOmmFile(std::string fileCont);

    std::vector<KeplerData> tryFetchOMMFileFromURL(std::string url_str);
    std::vector<KeplerData> tryFetchSingleOMMwithNorad(int norad);

    // Fetch OMM elements for many NORADs in one request. The URL template must contain a
    // %NORADS% placeholder, replaced with a comma-delimited NORAD list; absent => skipped.
    std::vector<KeplerData> tryFetchOMMsForNorads(const std::vector<int> &norads,
                                                  const std::string &batch_url_template);

    class KeplerDBHandler : public DBHandlerBase
    {
    private:
        struct AutoUpdateKeplersEvent
        {
        };

    public:
        KeplerDBHandler(std::shared_ptr<DBHandler> h);

        void init();

        void putKepler(KeplerData kep);
        bool getKepler(KeplerData &kep, int norad, time_t time = -1);
        std::vector<KeplerData> getAllNewestKepler();

        void autoUpdateKeplers();
        void updateKeplerDatabase();

    public: // Legacy
        std::optional<TLE> get_from_norad(int norad)
        {
            KeplerData kep;
            if (getKepler(kep, norad))
                return keplerToTle(kep);
            else
                return {};
        }

        std::optional<TLE> get_from_norad_time(int norad, time_t timestamp)
        {
            // TODOTLE Space-track

            KeplerData kep;
            if (getKepler(kep, norad, timestamp))
                return keplerToTle(kep);
            else
                return {};
        }

    private:
        std::vector<TLE> get_all_tles()
        {
            std::vector<TLE> tles;
            auto keps = getAllNewestKepler();
            for (auto &kep : keps)
            {
                // One unusable row must not take the whole app down: this runs during startup, and
                // the TLE builder throws for values it cannot encode (a feed writing BSTAR as
                // "-.2934936E+1" = -2.93 was persisted, then every later start died here). Such rows
                // are already in users' databases, so skip them instead of aborting.
                try
                {
                    TLE tle = keplerToTle(kep);
                    if (!tle.line1.empty())
                        tles.push_back(tle);
                    else
                        logger->warn("Skipping Kepler %d (%s): epoch out of range", kep.satellite_number, kep.name.c_str());
                }
                catch (std::exception &e)
                {
                    logger->warn("Skipping Kepler %d (%s): %s", kep.satellite_number, kep.name.c_str(), e.what());
                }
            }
            return tles;
        }

    public:
        std::vector<TLE> all_;
    };
} // namespace satdump