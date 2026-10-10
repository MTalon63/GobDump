#pragma once

#include "common/geodetic/euler_raytrace.h"
#include "nlohmann/json_utils.h"
#include "projection/raytrace/common/satellite_raytracer_sattrack.h"

namespace satdump
{
    namespace projection
    {
        /**
         * @brief MSU-GS VIS scan geometry: x across-track, y along-track, both mapped to Euler angles,
         * with a small across-track slant correction. One timestamp per scan line.
         */
        class NormalLineXYSatProj : public SatelliteRaytracerSatTrack
        {
        protected:
            std::vector<double> timestamps;

            int image_width, image_height;
            float scan_angle_x, scan_angle_y;

            double timestamp_offset;
            bool invert_scan_x, invert_scan_y;

            float roll_offset, pitch_offset, yaw_offset;

            std::vector<predict_position> sat_positions;

            double slant_off = 0, slant_sc = 0;

        public:
            NormalLineXYSatProj(nlohmann::ordered_json cfg) : SatelliteRaytracerSatTrack(cfg)
            {
                timestamps = d_cfg["timestamps"].get<std::vector<double>>();

                image_width = cfg["image_width"].get<int>();
                image_height = cfg["image_height"].get<int>();
                scan_angle_x = cfg["scan_angle_x"].get<float>();
                scan_angle_y = cfg["scan_angle_y"].get<float>();

                timestamp_offset = getValueOrDefault(cfg["timestamp_offset"], 0.0);
                invert_scan_x = getValueOrDefault(cfg["invert_scan_x"], false);
                invert_scan_y = getValueOrDefault(cfg["invert_scan_y"], false);

                roll_offset = getValueOrDefault(cfg["roll_offset"], 0.0);
                pitch_offset = getValueOrDefault(cfg["pitch_offset"], 0.0);
                yaw_offset = getValueOrDefault(cfg["yaw_offset"], 0.0);

                slant_off = getValueOrDefault(cfg["slant_off"], 0.0);
                slant_sc = getValueOrDefault(cfg["slant_sc"], 0.0);

                for (double timestamp : timestamps)
                    sat_positions.push_back(sat_tracker->get_sat_position_at_raw(timestamp + timestamp_offset));
            }

            bool get_position(double x, double y, geodetic::geodetic_coords_t &pos, double *otime)
            {
                if ((int)y >= (int)timestamps.size())
                    return 1;
                if ((int)y >= image_height)
                    return 1;
                if (timestamps[(int)y] == -1)
                    return 1;

                auto pos_curr = sat_positions[(int)y];

                // Retained from the original: a small latitude-dependent pitch tweak of the GEO bus.
                double added_pitch_off = -pos_curr.latitude * RAD_TO_DEG * 0.1;

                geodetic::euler_coords_t satellite_pointing;

                y += (x / 6004.0) * slant_sc;

                satellite_pointing.roll = -((((invert_scan_x ? -1.0 : 1.0) * x - (image_width / 2.0)) / image_width) * scan_angle_x) + roll_offset;
                satellite_pointing.pitch = -((((invert_scan_y ? -1.0 : 1.0) * y - (image_height / 2.0)) / image_height) * scan_angle_y) + pitch_offset + added_pitch_off;
                satellite_pointing.yaw = yaw_offset;

                geodetic::geodetic_coords_t ground_position;
                int ret = geodetic::raytrace_to_earth(pos_curr.time, pos_curr.position, pos_curr.velocity, satellite_pointing, ground_position);
                pos = ground_position.toDegs();

                return ret;
            }
        };
    } // namespace projection
} // namespace satdump
