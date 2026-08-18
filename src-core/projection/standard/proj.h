#pragma once

#include <cmath>
#define M_HALFPI M_PI_2
#define M_TWOPI (M_PI * 2)

#define DEG2RAD ((2.0 * M_PI) / 360.0)
#define RAD2DEG (360.0 / (2.0 * M_PI))

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace proj
{
    enum projection_type_t
    {
        ProjType_Invalid,
        ProjType_Equirectangular,
        ProjType_Stereographic,
        ProjType_UniversalTransverseMercator,
        ProjType_Geos,
        ProjType_Tpers,
        ProjType_WebMerc,
    };

    struct projection_setup_t
    {
        int zone = -1;        // For UTM
        bool south = false;   // For UTM
        bool sweep_x = false; // For GEOS
        double altitude = 0;  // For GEOS, TPERS
        double tilt = 0;      // For TPERS
        double azimuth = 0;   // For TPERS
    };

    /*
    Owning handle over the projection-specific data block. Each projection_setup_* attaches a deleter
    that knows how to tear its own type down (some of them own further allocations of their own), so
    the block is released exactly once when the last projection_t referring to it goes away.

    Ownership is shared rather than exclusive because projection_t is routinely passed and stored by
    value. The block is only written during setup and read-only afterwards, so sharing it between
    copies is safe, including from several threads at once.
    */
    typedef std::shared_ptr<void> projection_data_t;

    /*
    Helper for the common case of a data block that owns nothing else, and can therefore just be freed.
    Zero-initialized so that setup routines bailing out midway still leave every field in a known state.
    */
    template <typename T>
    inline T *projection_alloc_dat(projection_data_t &dat)
    {
        T *ptr = (T *)calloc(1, sizeof(T));
        if (ptr != nullptr)
            dat = projection_data_t(ptr, free);
        return ptr;
    }

    struct projection_t
    {
        // Core
        projection_type_t type = ProjType_Invalid; // Projection type
        projection_setup_t params;                 // Other setup parameters
        projection_data_t proj_dat;                // Owning handle on projection-specific info

        // Offsets & Scalars.
        double proj_offset_x = 0; // False Easting
        double proj_offset_y = 0; // False Northing
        double proj_scalar_x = 1; // X Scalar
        double proj_scalar_y = 1; // Y Scalar

        // Internal Offsets & Scalars
        double lam0 = 0; // Central Meridian
        double phi0 = 0; // Central Parrallel
        double k0 = 1;   // General scaling factor - e.g. the 0.9996 of UTM
        double x0 = 0;   // False Easting
        double y0 = 0;   // False Northing

        // Ellispoid definition
        double a;
        double e;
        double es;
        double n;
        double one_es;
        double rone_es;
    };

    /*
    Setup a projection. This expects the struct to already
    be pre-configured by the user and takes care of
    allocations and setting the ellispoid.
    */
    bool projection_setup(projection_t *proj);

    /*
    Release the memory allocated by projection_setup. Only needed to drop it early, as the projection_t
    releases it on its own once destroyed. Safe to call more than once.
    */
    void projection_free(projection_t *proj);

    /*
    Perform a forward projection.
    This converts Lat/Lon into X/Y in the projected plane.
    Lat/Lon should be in degrees.
    Returns true if an error occurred, false on success.
    */
    bool projection_perform_fwd(const projection_t *proj, double lon, double lat, double *x, double *y);

    /*
    Perform an inverse projection.
    This converts X/Y from the projected plane into Lat/Lon.
    Lat/Lon will be in degrees.
    Returns true if an error occurred, false on success.
    */
    bool projection_perform_inv(const projection_t *proj, double x, double y, double *lon, double *lat);
}