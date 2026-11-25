#include "common/geodetic/geodetic_coordinates.h"
#include "common/tracking/tle.h"
#include "common/utils.h"
#include "init.h"
#include "logger.h"
#include "object_tracker.h"
#include "utils/time.h"
#include <cfloat>

namespace satdump
{
    namespace
    {
        // A well-formed pass converges in tens of steps: a measured ISS pass needs ~39, and
        // even a two-hour pass is ~720. The AOS search only fails to converge when the orbit
        // predictions stop moving, which means a malformed TLE reached the propagator, so
        // these caps can never fire for valid data. They replace an endless spin while
        // holding general_mutex, which used to freeze the entire UI and stop autotrack.
        const int MAX_AOS_SEARCH_STEPS = 10000;
        const int MAX_MERIDIAN_SCAN_STEPS = 200000;
    }

    void ObjectTracker::backend_run()
    {
        while (backend_should_run)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            if (!has_tle)
                continue;

            general_mutex.lock();

            double current_time = getTime() + tracking_time_offset;

            if (tracking_mode == TRACKING_HORIZONS)
            {
                if (current_time > last_horizons_fetch_time + 3600)
                {
                    loadHorizons(current_time);
                    updateNextPass(current_time);
                    backend_needs_update = false;
                }

                if (horizons_data.size() > 0)
                {
                    //    size_t iter = 0;
                    //    for (size_t i = 0; i < horizons_data.size(); i++)
                    //        if (horizons_data[i].timestamp < current_time)
                    //            iter = i;

                    if (current_time > next_los_time)
                        updateNextPass(current_time);

                    horizons_interpolate(current_time, &sat_current_pos.az, &sat_current_pos.el);
                }
            }
            else if (tracking_mode == TRACKING_SATELLITE)
            {
                if (satellite_object != nullptr)
                {
                    predict_orbit(satellite_object, &satellite_orbit, predict_to_julian_double(current_time));
                    predict_observe_orbit(satellite_observer_station, &satellite_orbit, &satellite_observation_pos);

                    if (current_time > next_los_time)
                        updateNextPass(current_time);

                    sat_current_pos.az = satellite_observation_pos.azimuth * RAD_TO_DEG;
                    sat_current_pos.el = satellite_observation_pos.elevation * RAD_TO_DEG;
                }
            }

            // Update
            if (backend_needs_update)
            {
                logger->trace("Updating elements...");

                if (tracking_mode == TRACKING_HORIZONS)
                {
                    loadHorizons(current_time);
                    updateNextPass(current_time);
                }
                else if (tracking_mode == TRACKING_SATELLITE)
                {
                    // current_satellite_id is a position recorded when the object was selected, but
                    // all_ is rebuilt on every TLE update, so it can now point at a different
                    // satellite or past the end of the vector. Reading out of bounds fed the
                    // propagator garbage element lines, and the unbounded AOS search in
                    // updateNextPass() then spun forever while holding general_mutex, freezing the
                    // UI and stopping autotrack. Refuse an out-of-range or malformed row and keep
                    // the last good orbit instead.
                    auto &reg = db_keplers->all_;
                    if (current_satellite_id < 0 || current_satellite_id >= (int)reg.size())
                    {
                        logger->error("Tracked satellite index %d is out of range (%d TLEs in registry); keeping the current orbit",
                                      current_satellite_id, (int)reg.size());
                    }
                    else
                    {
                        auto &tle = reg[current_satellite_id];
                        if (!isValidTle(tle.line1, tle.line2))
                        {
                            logger->error("TLE for NORAD %d is malformed; keeping the current orbit", tle.norad);
                        }
                        else
                        {
                            if (satellite_object != nullptr)
                                predict_destroy_orbital_elements(satellite_object);

                            satellite_object = predict_parse_tle(tle.line1.c_str(), tle.line2.c_str());
                            updateNextPass(current_time);
                        }
                    }
                }

                backend_needs_update = false;
            }

            general_mutex.unlock();
        }
    }

    void ObjectTracker::updateNextPass(double current_time)
    {
        upcoming_passes_mtx.lock();
        logger->trace("Update pass trajectory...");

        upcoming_pass_points.clear();
        next_aos_time = 0;
        next_los_time = 0;
        northbound_cross = false;
        southbound_cross = false;

        if (tracking_mode == TRACKING_HORIZONS)
        {
            if (horizons_data.size() == 0)
            {
                upcoming_passes_mtx.unlock();
                return;
            }

            int iter = 0;
            for (int i = 0; i < (int)horizons_data.size(); i++)
                if (horizons_data[i].timestamp < current_time)
                    iter = i;

            if (horizons_data[iter].el > 0) // Already got AOS
            {
                next_aos_time = current_time;

                for (int i = iter - 1; i >= 0; i--) // Attempt to find previous AOS
                {
                    if (horizons_data[i].el <= 0)
                    {
                        next_aos_time = horizons_data[i].timestamp;
                        sat_next_aos_pos.az = horizons_data[i].az;
                        sat_next_aos_pos.el = 0;
                        break;
                    }
                }

                for (int i = iter; i < (int)horizons_data.size(); i++) // Find LOS
                {
                    if (horizons_data[i].el <= 0)
                    {
                        next_los_time = horizons_data[i].timestamp;
                        break;
                    }
                }
            }
            else
            {
                int aos_iter = 0;
                for (int i = iter; i < (int)horizons_data.size(); i++) // Find AOS
                {
                    if (horizons_data[i].el > 0)
                    {
                        next_aos_time = horizons_data[i].timestamp;
                        sat_next_aos_pos.az = horizons_data[i].az;
                        sat_next_aos_pos.el = 0;
                        aos_iter = i;
                        break;
                    }
                }

                if (next_aos_time != 0)
                {
                    for (int i = aos_iter; i < (int)horizons_data.size(); i++) // Find LOS
                    {
                        if (horizons_data[i].el <= 0)
                        {
                            next_los_time = horizons_data[i].timestamp;
                            break;
                        }
                    }
                }
            }

            if (/*is_gui &&*/ next_aos_time != 0 && next_los_time != 0)
            {
                double time_step = abs(next_los_time - next_aos_time) / 50.0;

                // A zero or non-finite step would make the sampling loop below never advance.
                if (!(time_step > 0))
                    time_step = 1.0;

                for (double ctime = next_aos_time; ctime <= next_los_time; ctime += time_step)
                {
                    int iter = 0;
                    for (int i = 0; i < (int)horizons_data.size(); i++)
                        if (horizons_data[i].timestamp < ctime)
                            iter = i;

                    upcoming_pass_points.push_back({horizons_data[iter].az, horizons_data[iter].el});
                }
            }
        }
        else if (tracking_mode == TRACKING_SATELLITE)
        {
            if (predict_is_geosynchronous(satellite_object))
            {
                next_aos_time = 0;
                next_los_time = DBL_MAX;
                upcoming_passes_mtx.unlock();
                return;
            }

            // Get next LOS
            predict_observation next_aos, next_los;
            next_aos = next_los = predict_next_los(satellite_observer_station, satellite_object, predict_to_julian_double(getTime()));

            // Calculate the AOS before that LOS. This search only terminates when the predictions
            // actually move; with a malformed TLE predict_next_aos() can keep returning the same
            // value and the loop never ends, spinning forever while holding general_mutex.
            next_aos_time = next_los_time = predict_from_julian(next_los.time);
            int aos_search_steps = 0;
            do
            {
                next_aos = predict_next_aos(satellite_observer_station, satellite_object, predict_to_julian_double(next_aos_time));
                next_aos_time -= 10;
                if (++aos_search_steps > MAX_AOS_SEARCH_STEPS)
                {
                    logger->error("AOS search did not converge after %d steps; aborting pass calculation", aos_search_steps);
                    upcoming_passes_mtx.unlock();
                    return;
                }
            } while (predict_from_julian(next_aos.time) >= next_los_time);

            next_los_time = predict_from_julian(next_los.time);
            next_aos_time = predict_from_julian(next_aos.time);

            sat_next_aos_pos.az = next_aos.azimuth * RAD_TO_DEG;
            sat_next_aos_pos.el = 0;
            sat_next_los_pos.az = next_los.azimuth * RAD_TO_DEG;
            sat_next_los_pos.el = next_los.elevation * RAD_TO_DEG;

            if (meridian_flip_correction)
            {
                // Determine pass direction
                predict_position satellite_orbit2;
                predict_observation observation_pos_cur;
                predict_observation observation_pos_prev;
                double currTime = next_aos_time;
                predict_orbit(satellite_object, &satellite_orbit2, predict_to_julian_double(currTime));
                predict_observe_orbit(satellite_observer_station, &satellite_orbit2, &observation_pos_prev);
                currTime += 1.0;
                int meridian_scan_steps = 0;
                do
                {
                    predict_orbit(satellite_object, &satellite_orbit2, predict_to_julian_double(currTime));
                    predict_observe_orbit(satellite_observer_station, &satellite_orbit2, &observation_pos_cur);

                    if (std::abs(observation_pos_prev.azimuth - observation_pos_cur.azimuth) > (180 * DEG_TO_RAD))
                    {
                        if (next_los.azimuth > (90 * DEG_TO_RAD) && next_los.azimuth < (270 * DEG_TO_RAD))
                            southbound_cross = true;
                        else
                            northbound_cross = true;
                    }
                    currTime += 1.0;
                    observation_pos_prev = observation_pos_cur;
                    if (++meridian_scan_steps > MAX_MERIDIAN_SCAN_STEPS)
                    {
                        logger->error("Meridian scan did not converge after %d steps; aborting pass calculation", meridian_scan_steps);
                        upcoming_passes_mtx.unlock();
                        return;
                    }
                } while (next_los_time > currTime);
            }

            if (true) //(is_gui)
            {
                // Calculate a few points during the pass
                predict_position satellite_orbit2;
                predict_observation observation_pos2;

                double time_step = abs(next_los_time - next_aos_time) / 50.0;

                // A zero or non-finite step would make the sampling loop below never advance.
                if (!(time_step > 0))
                    time_step = 1.0;

                for (double ctime = next_aos_time; ctime <= next_los_time; ctime += time_step)
                {
                    predict_orbit(satellite_object, &satellite_orbit2, predict_to_julian_double(ctime));
                    predict_observe_orbit(satellite_observer_station, &satellite_orbit2, &observation_pos2);
                    upcoming_pass_points.push_back({float(observation_pos2.azimuth * RAD_TO_DEG), float(observation_pos2.elevation * RAD_TO_DEG)});
                }
            }
        }

        upcoming_passes_mtx.unlock();
    }
} // namespace satdump
