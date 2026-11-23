#include "scheduler.h"
#include "common/utils.h"
#include "core/plugin.h"
#include "init.h"
#include "logger.h"
#include "utils/time.h"

namespace satdump
{
    AutoTrackScheduler::AutoTrackScheduler()
    {
        auto &tle_registry = db_keplers->all_;
        if (tle_registry.size() > 0)
            has_tle = true;

        for (auto &tle : tle_registry)
            satoptions.push_back(tle.name);

        // Updates on registry updates
        tle_updated_handler_id = eventBus->register_handler<TLEsUpdatedEvent>(
            [this](TLEsUpdatedEvent)
            {
                upcoming_satellite_passes_mtx.lock();

                auto &tle_registry = db_keplers->all_;
                if (tle_registry.size() > 0)
                    has_tle = true;

                satoptions.clear();
                for (auto &tle : tle_registry)
                    satoptions.push_back(tle.name);

                upcoming_satellite_passes_mtx.unlock();
            });
    }

    AutoTrackScheduler::~AutoTrackScheduler()
    {
        eventBus->unregister_handler(tle_updated_handler_id);

        if (backend_should_run)
        {
            backend_should_run = false;
            if (backend_thread.joinable())
                backend_thread.join();
        }
    }

    void AutoTrackScheduler::processAutotrack(double curr_time)
    {
        if (autotrack_engaged)
        {
            upcoming_satellite_passes_mtx.lock();

            if (autotrack_cfg.multi_mode)
            {
                bool update = false;

                for (int i = 0; i < (int)upcoming_satellite_passes_sel.size(); i++)
                {
                    auto &pass = upcoming_satellite_passes_sel[i];
                    if (curr_time > pass.aos_time)
                    {
                        if (vfo_mode_norads_vis.count(pass.norad) == 0)
                        {
                            vfo_mode_norads_vis.insert({pass.norad, pass});

                            logger->critical("AOS!!!!!!!!!!!!!! %d", pass.norad);
                            eventBus->fire_event<events::TrackingSchedulerAOSEvent>({pass});
                            TrackedObject obj;
                            for (auto &v : enabled_satellites)
                                if (v.norad == pass.norad)
                                    obj = v;
                            aos_callback(autotrack_cfg, pass, obj);
                        }
                    }
                }

            recheck:
                for (auto &p : vfo_mode_norads_vis)
                {
                    if (curr_time > p.second.los_time)
                    {
                        // Continuous GEO pass (~12h window): refresh schedule and extend LOS if still active
                        if ((p.second.los_time - p.second.aos_time) >= (12 * 3600 - 5))
                        {
                            updateAutotrackPasses(curr_time);
                            bool still_active = false;
                            for (auto &next_pass : upcoming_satellite_passes_sel)
                            {
                                if (next_pass.norad == p.first && next_pass.aos_time <= curr_time + 2.0)
                                {
                                    p.second.los_time = next_pass.los_time;
                                    still_active = true;
                                    break;
                                }
                            }
                            if (still_active)
                                continue;
                        }

                        logger->critical("LOS!!!!!!!!!!!!!! %d ", p.first);
                        eventBus->fire_event<events::TrackingSchedulerLOSEvent>({p.second});
                        TrackedObject obj;
                        for (auto &v : enabled_satellites)
                            if (v.norad == p.first)
                                obj = v;
                        los_callback(autotrack_cfg, p.second, obj);

                        vfo_mode_norads_vis.erase(p.first);
                        update = true;
                        goto recheck;
                    }
                }

                if (update)
                {
                    updateAutotrackPasses(curr_time);

                    if (upcoming_satellite_passes_sel.size() > 0)
                    {
                        TrackedObject obj;
                        for (auto &v : enabled_satellites)
                            if (v.norad == upcoming_satellite_passes_sel[0].norad)
                                obj = v;
                        eng_callback(autotrack_cfg, upcoming_satellite_passes_sel[0], obj);
                    }
                }
            }
            else
            {
                if (upcoming_satellite_passes_sel.size() > 0)
                {
                    if (!autotrack_pass_has_started && curr_time > upcoming_satellite_passes_sel[0].aos_time)
                    {
                        logger->critical("AOS!!!!!!!!!!!!!! %d", upcoming_satellite_passes_sel[0].norad);
                        eventBus->fire_event<events::TrackingSchedulerAOSEvent>({upcoming_satellite_passes_sel[0]});
                        TrackedObject obj;
                        for (auto &v : enabled_satellites)
                            if (v.norad == upcoming_satellite_passes_sel[0].norad)
                                obj = v;
                        aos_callback(autotrack_cfg, upcoming_satellite_passes_sel[0], obj);
                        autotrack_pass_has_started = true;
                    }
                }

                if (upcoming_satellite_passes_sel.size() > 0 && curr_time > upcoming_satellite_passes_sel[0].los_time)
                {
                    SatellitePass ended_pass = upcoming_satellite_passes_sel[0];
                    bool was_continuous_geo = autotrack_pass_has_started &&
                                              ((ended_pass.los_time - ended_pass.aos_time) >= (12 * 3600 - 5));

                    updateAutotrackPasses(curr_time);

                    // If the refreshed schedule immediately continues the same GEO satellite, keep it running without LOS/AOS restart
                    if (was_continuous_geo &&
                        upcoming_satellite_passes_sel.size() > 0 &&
                        upcoming_satellite_passes_sel[0].norad == ended_pass.norad &&
                        upcoming_satellite_passes_sel[0].aos_time <= curr_time + 2.0)
                    {
                        // Leave autotrack_pass_has_started = true so neither LOS nor AOS fires
                    }
                    else
                    {
                        if (autotrack_pass_has_started)
                        {
                            logger->critical("LOS!!!!!!!!!!!!!! %d", ended_pass.norad);
                            eventBus->fire_event<events::TrackingSchedulerLOSEvent>({ended_pass});
                            TrackedObject obj;
                            for (auto &v : enabled_satellites)
                                if (v.norad == ended_pass.norad)
                                    obj = v;
                            los_callback(autotrack_cfg, ended_pass, obj);
                        }
                        autotrack_pass_has_started = false;

                        if (upcoming_satellite_passes_sel.size() > 0)
                        {
                            TrackedObject obj;
                            for (auto &v : enabled_satellites)
                                if (v.norad == upcoming_satellite_passes_sel[0].norad)
                                    obj = v;
                            eng_callback(autotrack_cfg, upcoming_satellite_passes_sel[0], obj);
                        }
                    }
                }
            }

            upcoming_satellite_passes_mtx.unlock();
        }

    }

    void AutoTrackScheduler::updateAutotrackPasses(double curr_time)
    {
        upcoming_satellite_passes_all.clear();

        for (TrackedObject &obj : enabled_satellites)
        {
#if 0
            if (obj.norad == 41105)
            {
                std::vector<satdump::SatellitePass> dump_passes;
                for (uint64_t ctime = curr_time; ctime < curr_time + 12 * 3600; ctime++)
                {
                    if ((ctime - 9 * 60) % (30 * 60) == 0)
                    {
                        dump_passes.push_back({0, ctime - 600, ctime, 0});
                    }
                }

                auto passes = getPassesForSatellite(41105, curr_time, 0, qth_lon, qth_lat, qth_alt, dump_passes);
                upcoming_satellite_passes_all.insert(upcoming_satellite_passes_all.end(), passes.begin(), passes.end());
            }
            else
#endif
            {
                int slot_seconds = 900;
                auto slot_it = autotrack_cfg.rdas_slot_seconds.find(obj.norad);
                if (slot_it != autotrack_cfg.rdas_slot_seconds.end())
                    slot_seconds = slot_it->second;

                int slot_offset = -15;
                auto off_it = autotrack_cfg.rdas_slot_offset_seconds.find(obj.norad);
                if (off_it != autotrack_cfg.rdas_slot_offset_seconds.end())
                    slot_offset = off_it->second;

                auto passes = getPassesForSatellite(obj.norad, curr_time, 12 * 3600, qth_lon, qth_lat, qth_alt, {}, slot_seconds, autotrack_cfg.rdas_pass_seconds, slot_offset, autotrack_cfg.rdas_skip_xrit_slots);
                for (auto &pass : passes)
                    if (pass.max_elevation > obj.min_elevation)
                        upcoming_satellite_passes_all.push_back(pass);
            }
        }

        upcoming_satellite_passes_all = filterPassesByElevation(upcoming_satellite_passes_all, autotrack_cfg.autotrack_min_elevation, 90); // TODO

        std::sort(upcoming_satellite_passes_all.begin(), upcoming_satellite_passes_all.end(), [](SatellitePass &el1, SatellitePass &el2) { return el1.aos_time < el2.aos_time; });

        upcoming_satellite_passes_sel.clear();

        if (autotrack_cfg.multi_mode)
            upcoming_satellite_passes_sel = upcoming_satellite_passes_all;
        else
            upcoming_satellite_passes_sel = selectPassesForAutotrack(upcoming_satellite_passes_all);

        // for (auto ppp : upcoming_satellite_passes_sel)
        // logger->debug("Pass of %s at AOS %s LOS %s elevation %.2f",
        //               general_tle_registry->get_from_norad(ppp.norad).value().name.c_str(),
        //               timestamp_to_string(ppp.aos_time).c_str(),
        //               timestamp_to_string(ppp.los_time).c_str(),
        //               ppp.max_elevation);
    }

    void AutoTrackScheduler::setQTH(double qth_lon, double qth_lat, double qth_alt)
    {
        upcoming_satellite_passes_mtx.lock();
        this->qth_lon = qth_lon;
        this->qth_lat = qth_lat;
        this->qth_alt = qth_alt;
        // backend_needs_update = true;
        upcoming_satellite_passes_mtx.unlock();
    }

    void AutoTrackScheduler::setEngaged(bool v, double curr_time)
    {
        upcoming_satellite_passes_mtx.lock();

        if (!v)
        {
            // Disengaging must fire LOS for whatever is currently running, otherwise a live
            // pipeline/VFO is left open; multi-VFO and single-pass modes are handled separately.
            if (autotrack_cfg.multi_mode)
            {
                for (auto &p : vfo_mode_norads_vis)
                {
                    eventBus->fire_event<events::TrackingSchedulerLOSEvent>({p.second});
                    TrackedObject obj;
                    for (auto &sat : enabled_satellites)
                        if (sat.norad == p.first)
                            obj = sat;
                    los_callback(autotrack_cfg, p.second, obj);
                }
                vfo_mode_norads_vis.clear();
            }
            else if (autotrack_pass_has_started && upcoming_satellite_passes_sel.size() > 0)
            {
                eventBus->fire_event<events::TrackingSchedulerLOSEvent>({upcoming_satellite_passes_sel[0]});
                TrackedObject obj;
                for (auto &sat : enabled_satellites)
                    if (sat.norad == upcoming_satellite_passes_sel[0].norad)
                        obj = sat;
                los_callback(autotrack_cfg, upcoming_satellite_passes_sel[0], obj);
            }

            autotrack_pass_has_started = false;
            autotrack_engaged = false;
            upcoming_satellite_passes_mtx.unlock();
            return;
        }

        vfo_mode_norads_vis.clear();
        autotrack_pass_has_started = false;
        autotrack_engaged = v;
        updateAutotrackPasses(curr_time);

        if (autotrack_engaged && upcoming_satellite_passes_sel.size() > 0)
        {
            TrackedObject obj;
            for (auto &sat : enabled_satellites)
                if (sat.norad == upcoming_satellite_passes_sel[0].norad)
                    obj = sat;
            eng_callback(autotrack_cfg, upcoming_satellite_passes_sel[0], obj);
            autotrack_pass_has_started = false;
        }
        else
        {
            autotrack_engaged = false;
        }

        upcoming_satellite_passes_mtx.unlock();
    }

    bool AutoTrackScheduler::getEngaged() { return autotrack_engaged; }

    std::vector<TrackedObject> AutoTrackScheduler::getTracked() { return enabled_satellites; }

    void AutoTrackScheduler::setTracked(std::vector<TrackedObject> tracked)
    {
        upcoming_satellite_passes_mtx.lock();
        enabled_satellites = tracked;
        upcoming_satellite_passes_mtx.unlock();
    }

    void AutoTrackScheduler::start()
    {
        backend_should_run = true;
        backend_thread = std::thread(&AutoTrackScheduler::backend_run, this);
    }

    void AutoTrackScheduler::backend_run()
    {
        while (backend_should_run)
        {
            processAutotrack(getTime());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    AutoTrackCfg AutoTrackScheduler::getAutoTrackCfg() { return autotrack_cfg; }

    void AutoTrackScheduler::setAutoTrackCfg(AutoTrackCfg v)
    {
        upcoming_satellite_passes_mtx.lock();
        autotrack_cfg = v;
        upcoming_satellite_passes_mtx.unlock();
    }
} // namespace satdump
