#include "dump_svissr_telemetry.h"

#include <nlohmann/json.hpp>

namespace fengyun_svissr
{
    std::string SVISSRTelemetryDumper::dump_telemetry_to_JSON(OrbitAndAttitudeData OAT_block, AttitudePredictionSubBlock Attitude_subblock, OrbitPredictionSubBlock Orbital_subblock,
                                                              std::string active_sensor, uint8_t active_detectors)
    {
        nlohmann::ordered_json j;

        auto detector = [active_detectors](uint8_t mask)
        {
            return (active_detectors & mask) == mask ? "Primary" : "Backup";
        };

        j["Active VISSR sensor"] = active_sensor;
        j["Active VIS1 detector"] = detector(0b10000);
        j["Active VIS2 detector"] = detector(0b100000);
        j["Active VIS3 detector"] = detector(0b1000000);
        j["Active VIS4 detector"] = detector(0b10000000);
        j["Active IR1 detector"] = detector(0b1);
        j["Active IR2 detector"] = detector(0b10);
        j["Active IR3 detector"] = detector(0b100);
        j["Active IR4 detector"] = detector(0b1000);

        j["Image start time [Unix]"] = ((OAT_block.IMAGE_START_TIME * 1e-8) - 40587) * 86400;
        j["VIS channel stepping along line [rad]"] = OAT_block.VIS_STEPPING_LINE * 1e-8;
        j["IR channel stepping along line [rad]"] = OAT_block.IR_STEPPING_LINE * 1e-8;
        j["VIS channel stepping along pixel [rad]"] = OAT_block.VIS_STEPPING_PIXEL * 1e-10;
        j["IR channel stepping along pixel [rad]"] = OAT_block.IR_STEPPING_PIXEL * 1e-10;
        j["VIS channel center line number"] = OAT_block.VIS_CENTER_LINE * 1e-4;
        j["IR channel center line number"] = OAT_block.IR1_CENTER_LINE * 1e-4;
        j["IR2 channel center line number"] = OAT_block.IR2_channel_center_line_number_of_frame * 1e-4;
        j["IR3 channel center line number"] = OAT_block.IR3_channel_center_line_number_of_frame * 1e-4;
        j["VIS channel center pixel number"] = OAT_block.VIS_CENTER_PIXEL * 1e-4;
        j["IR1 channel center pixel number"] = OAT_block.IR1_CENTER_PIXEL * 1e-4;
        j["Number of VIS sensors"] = OAT_block.VIS_SENSOR_NUMBER.value();
        j["Number of IR sensors"] = OAT_block.IR_SENSOR_NUMBER.value();
        j["Total number of VIS channel lines"] = OAT_block.VIS_TOTAL_LINE_NUMBER.value();
        j["Total number of IR channel lines"] = OAT_block.IR_TOTAL_LINE_NUMBER.value();
        j["Pixels per VIS line"] = OAT_block.VIS_LINE_PX.value();
        j["Pixels per IR line"] = OAT_block.IR_LINE_PX.value();
        j["VISSR misalignment angle around the x axis [rad]"] = OAT_block.VISSR_MISALIGNMENT_ANGLE_X_AXIS * 1e-10;
        j["VISSR misalignment angle around the y axis [rad]"] = OAT_block.VISSR_MISALIGNMENT_ANGLE_Y_AXIS * 1e-10;
        j["VISSR misalignment angle around the z axis [rad]"] = OAT_block.VISSR_MISALIGNMENT_ANGLE_Z_AXIS * 1e-10;
        j["VISSR misalignment matrix row 1 column 1 (Constant)"] = OAT_block.VMM_R1_C1 * 1e-7;
        j["VISSR misalignment matrix row 2 column 1 (Constant)"] = OAT_block.VMM_R2_C1 * 1e-10;
        j["VISSR misalignment matrix row 3 column 1 (Constant)"] = OAT_block.VMM_R3_C1 * 1e-10;
        j["VISSR misalignment matrix row 1 column 2 (Constant)"] = OAT_block.VMM_R1_C2 * 1e-10;
        j["VISSR misalignment matrix row 2 column 2 (Constant)"] = OAT_block.VMM_R2_C2 * 1e-7;
        j["VISSR misalignment matrix row 3 column 2 (Constant)"] = OAT_block.VMM_R3_C2 * 1e-10;
        j["VISSR misalignment matrix row 1 column 3 (Constant)"] = OAT_block.VMM_R1_C3 * 1e-10;
        j["VISSR misalignment matrix row 2 column 3 (Constant)"] = OAT_block.VMM_R2_C3 * 1e-10;
        j["VISSR misalignment matrix row 3 column 3 (Constant)"] = OAT_block.VMM_R3_C3 * 1e-7;
        j["Ratio of circumference (Constant)"] = OAT_block.RATIO_OF_CIRCUMFERENCE * 1e-7;
        j["Earth equatorial radius (Constant) [m]"] = OAT_block.EARTH_RADIUS * 1e-1;
        j["Earth oblateness (Constant)"] = OAT_block.EARTH_OBLATENESS * 1e-10;
        j["Earth eccentricity (Constant)"] = OAT_block.EARTH_ECCENTRICITY * 1e-9;
        j["Angle between VISSR and view of sun sensor at start of data (Constant?) [rad]"] = OAT_block.VISSR_SUN_SENSOR_ANGLE * 1e-8;
        j["Epoch of orbital parameters in mean J2K [MJD]"] = OAT_block.ORBITAL_PARAM_EPOCH * 1e-8;
        j["Semi major axis in mean J2K [km]"] = OAT_block.SEMI_MAJOR_AXIS * 1e-8;
        j["Eccentricity in mean J2K"] = OAT_block.ECCENTRICITY * 1e-10;
        j["Inclination in mean J2K [deg]"] = OAT_block.INCLINATION * 1e-8;
        j["Longitude of ascending node in mean J2K [deg]"] = OAT_block.LONGITUDE_ASCENDING_NODE * 1e-8;
        j["Argument of perigee in mean J2K [deg]"] = OAT_block.PERIGEE * 1e-8;
        j["Mean anomaly in mean J2K [deg]"] = OAT_block.MEAN_ANOMALY * 1e-8;
        j["Sub-satellite East Longitude in mean J2K [deg]"] = OAT_block.SSP_LONGITUDE * 1e-6;
        j["Sub-satellite North latitude in mean J2K [deg]"] = OAT_block.SSP_LATITUDE * 1e-6;
        j["Epoch of attitude parameters in mean J2K [MJD]"] = OAT_block.ATTITUDE_PARAM_EPOCH * 1e-8;
        j["Angle between Z-axis and satellite spin axis projected on yz-plane alpha in mean J2K [rad]"] = OAT_block.ALPHA_Z_AXIS_SAT_SPIN_ON_YZ_PLANE * 1e-8;
        j["Change rate of alpha in mean J2K"] = OAT_block.ALPHA_CHANGE_RATE * 1e-15;
        j["Angle between Satellite Spin Axis and yz-plane delta in mean J2K [rad]"] = OAT_block.DELTA_SAT_SPIN_YZ_PLANE * 1e-11;
        j["Change rate of delta in mean J2K"] = OAT_block.DELTA_CHANGE_RATE * 1e-15;
        j["Estimated daily mean spin rate [rpm]"] = OAT_block.MEAN_SPIN_RATE * 1e-8;
        j["Time of the first attitude prediction record [Unix]"] = ((OAT_block.FIRST_ATTITUDE_PREDICTION * 1e-8) - 40587) * 86400;
        j["Time of the latest attitude prediction record [Unix]"] = ((OAT_block.LATEST_ATTITUDE_PREDICTION * 1e-8) - 40587) * 86400;
        j["Interval time of attitude prediction data (Constant at 0.00347222) [MJD]"] = OAT_block.ATTITUDE_PREDICTION_INTERVAL * 1e-8;
        j["Attitude prediction number"] = static_cast<int>(OAT_block.ATTITUDE_PREDICTION_NUMBER);
        j["Time of first orbit prediction record [Unix]"] = ((OAT_block.FIRST_ORBITAL_PREDICTION * 1e-8) - 40587) * 86400;
        j["Time of last orbit prediction record [Unix]"] = ((OAT_block.LATEST_ORBITAL_PREDICTION * 1e-8) - 40587) * 86400;
        j["Interval of orbital prediction data (Constant at 0.00347222) [MJD]"] = OAT_block.ORBITAL_PREDICTION_INTERVAL * 1e-8;
        j["Orbital prediction number [=8?]"] = static_cast<int>(OAT_block.ORBITAL_PREDICTION_COUNT);
        j["UTC time of attitude prediction processing [Unix]"] = ((Attitude_subblock.PREDICTION_TIME * 1e-8) - 40587) * 86400;
        j["UTC time of attitude prediction processing [BCD]"] = Attitude_subblock.PREDICTION_TIME_BCD.value();
        j["Angle between z-axis and satellite spin axis projected on yz plane in mean of 1950.0 coordinates, mean J2K [rad]"] = Attitude_subblock.Z_AXIS_SAT_SPIN_ON_YZ_PLANE_ANGLE * 1e-8;
        j["Angle between satelltie spin axis and yz-plane, [rad]"] = Attitude_subblock.SAT_SPIN_YZ_PLANE_ANGLE * 1e-11;
        j["Dihedral angle between the Sun and Earth measured clockwise viewing from North, [rad]"] = Attitude_subblock.DIHEDRAL_ANGLE * 1e-8;
        j["Spin speed of satellite [rpm]"] = Attitude_subblock.SPIN_RATE * 1e-8;
        j["Right ascension of satellite spin axis on the sat orbit plane coordinate system [rad]"] = Attitude_subblock.RIGHT_ASCENSION_SPIN_AXIS * 1e-8;
        j["Declination of satellite spin axis on the sat orbit plane coordinate system [rad]"] = Attitude_subblock.DECLINATION_SPIN_AXIS * 1e-8;
        j["UTC time of orbit prediction processing [Unix]"] = ((Orbital_subblock.PREDICTION_TIME_MJD * 1e-8) - 40587) * 86400;
        j["UTC time of orbit prediction processing [BCD]"] = Orbital_subblock.PREDICTION_TIME_BCD.value();
        j["X component of the satellite position, mean J2K [m]"] = Orbital_subblock.X_COMP * 1e-6;
        j["Y component of the satellite position, mean J2K [m]"] = Orbital_subblock.Y_COMP * 1e-6;
        j["Z component of the satellite position, mean J2K [m]"] = Orbital_subblock.Z_COMP * 1e-6;
        j["X component of the satellite position, mean J2K [m/s]"] = Orbital_subblock.X_COMP_SPEED * 1e-8;
        j["Y component of the satellite position, mean J2K [m/s]"] = Orbital_subblock.Y_COMP_SPEED * 1e-8;
        j["Z component of the satellite position, mean J2K [m/s]"] = Orbital_subblock.Z_COMP_SPEED * 1e-8;
        j["X component of the satellite position, ECEF [m]"] = Orbital_subblock.X_COMP_EARTH_FIXED * 1e-6;
        j["Y component of the satellite position, ECEF [m]"] = Orbital_subblock.Y_COMP_EARTH_FIXED * 1e-6;
        j["Z component of the satellite position, ECEF [m]"] = Orbital_subblock.Z_COMP_EARTH_FIXED * 1e-6;
        j["X component of the satellite position, ECEF [m/s]"] = Orbital_subblock.X_COMP_EARTH_FIXED_SPEED * 1e-10;
        j["Y component of the satellite position, ECEF [m/s]"] = Orbital_subblock.Y_COMP_EARTH_FIXED_SPEED * 1e-10;
        j["Z component of the satellite position, ECEF [m/s]"] = Orbital_subblock.Z_COMP_EARTH_FIXED_SPEED * 1e-10;
        j["Greenwhich sidereal time in true of data coordinates, [deg]"] = Orbital_subblock.GREENWHICH_SIDEREAL * 1e-8;
        j["Right ascension from the satellite to the sun in mean J2K [deg]"] = Orbital_subblock.RATS_J2 * 1e-8;
        j["Declination from the satellite to the sun in J2K [deg]"] = Orbital_subblock.DSTS_J2 * 1e-8;
        j["Right ascension from the satellite to the sun in ECEF [deg]"] = Orbital_subblock.RATS_EF * 1e-8;
        j["Declination from the satellite to the sun in ECEF [deg]"] = Orbital_subblock.DSTS_EF * 1e-8;
        j["Nutation and precession matrix row 1 column 1"] = Orbital_subblock.NPM_R1_C1 * 1e-12;
        j["Nutation and precession matrix row 2 column 1"] = Orbital_subblock.NPM_R2_C1 * 1e-14;
        j["Nutation and precession matrix row 3 column 1"] = Orbital_subblock.NPM_R3_C1 * 1e-14;
        j["Nutation and precession matrix row 1 column 2"] = Orbital_subblock.NPM_R1_C2 * 1e-14;
        j["Nutation and precession matrix row 2 column 2"] = Orbital_subblock.NPM_R2_C2 * 1e-12;
        j["Nutation and precession matrix row 3 column 2"] = Orbital_subblock.NPM_R3_C2 * 1e-16;
        j["Nutation and precession matrix row 1 column 3"] = Orbital_subblock.NPM_R1_C3 * 1e-12;
        j["Nutation and precession matrix row 2 column 3"] = Orbital_subblock.NPM_R2_C3 * 1e-16;
        j["Nutation and precession matrix row 3 column 3"] = Orbital_subblock.NPM_R3_C3 * 1e-12;
        j["Sub-satellite North Latitude, [deg]"] = Orbital_subblock.SSP_LATITUDE * 1e-8;
        j["Sub-satellite East Longitude, [deg]"] = Orbital_subblock.SSP_LONGITUDE * 1e-8;
        j["Satellite altitude above Earth surface [m]"] = Orbital_subblock.ALTITUDE * 1e-6;

        return j.dump(4);
    }
} // namespace fengyun_svissr
