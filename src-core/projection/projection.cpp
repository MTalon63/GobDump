#include "projection.h"
#include "logger.h"

#include <utility>

#include "common/geodetic/vincentys_calculations.h" // TODOREWORK MOVE OUT
#include "projection/standard/proj_json.h"
#include "raytrace/gcp_compute.h"

namespace satdump
{
    namespace projection
    {
        Projection::Projection() {}

        Projection::~Projection()
        {
            if (fwd_type == PROJ_STANDARD || inv_type == PROJ_STANDARD)
                ::proj::projection_free(&std_proj);
        }

        // The projection_t params are plain values, so re-running projection_setup on the copy rebuilds an
        // identical proj_dat that this instance alone owns. Set the types last so a failed setup can't be freed.
        Projection::Projection(const Projection &o)
            : d_cfg(o.d_cfg), fwd_valid(o.fwd_valid), rev_valid(o.rev_valid), width(o.width), height(o.height), raytracer(o.raytracer), transform(o.transform), std_proj(o.std_proj),
              tps_fwd(o.tps_fwd), proj_timestamp(o.proj_timestamp), has_2nd_transform(o.has_2nd_transform), transform2(o.transform2)
        {
            std_proj.proj_dat = nullptr;
            if (o.fwd_type == PROJ_STANDARD || o.inv_type == PROJ_STANDARD)
                if (::proj::projection_setup(&std_proj))
                    return;
            fwd_type = o.fwd_type;
            inv_type = o.inv_type;
        }

        Projection::Projection(Projection &&o)
            : d_cfg(std::move(o.d_cfg)), fwd_valid(o.fwd_valid), rev_valid(o.rev_valid), fwd_type(o.fwd_type), inv_type(o.inv_type), width(o.width), height(o.height),
              raytracer(std::move(o.raytracer)), transform(std::move(o.transform)), std_proj(o.std_proj), tps_fwd(std::move(o.tps_fwd)), proj_timestamp(o.proj_timestamp),
              has_2nd_transform(o.has_2nd_transform), transform2(std::move(o.transform2))
        {
            o.std_proj.proj_dat = nullptr; // Ownership stolen, so the source dtor must not free it
            o.fwd_type = o.inv_type = PROJ_INVALID;
        }

        // Copy-and-swap: the copy ctor does the only work that can throw, and it does it before this object is
        // touched. Whichever object ends up owning the old proj_dat frees it exactly once.
        Projection &Projection::operator=(const Projection &o)
        {
            if (this != &o)
                *this = Projection(o);
            return *this;
        }

        Projection &Projection::operator=(Projection &&o)
        {
            if (this != &o)
            {
                std::swap(d_cfg, o.d_cfg);
                std::swap(fwd_valid, o.fwd_valid);
                std::swap(rev_valid, o.rev_valid);
                std::swap(fwd_type, o.fwd_type);
                std::swap(inv_type, o.inv_type);
                std::swap(width, o.width);
                std::swap(height, o.height);
                std::swap(raytracer, o.raytracer);
                std::swap(transform, o.transform);
                std::swap(std_proj, o.std_proj);
                std::swap(tps_fwd, o.tps_fwd);
                std::swap(proj_timestamp, o.proj_timestamp);
                std::swap(has_2nd_transform, o.has_2nd_transform);
                std::swap(transform2, o.transform2);
            }
            return *this;
        }

        bool Projection::init(bool fwd, bool inv)
        {
            ///////////////////////////////////////////////////////////
            // We need image width/height
            ///////////////////////////////////////////////////////////
            if (d_cfg.contains("width"))
                width = d_cfg["width"];
            else
                throw satdump_exception("Image width must be present!");

            if (d_cfg.contains("height"))
                height = d_cfg["height"];
            else
                throw satdump_exception("Image height must be present!");

            ///////////////////////////////////////////////////////////
            // Get channel pre-transform, if present
            ///////////////////////////////////////////////////////////
            if (d_cfg.contains("transform"))
                transform = d_cfg["transform"];
            else
                transform.init_none();

            ///////////////////////////////////////////////////////////
            // Get optional second channel pre-transform, if present
            ///////////////////////////////////////////////////////////
            if (d_cfg.contains("transform2"))
            {
                has_2nd_transform = true;
                transform2 = d_cfg["transform2"];
            }

            ///////////////////////////////////////////////////////////
            // Attempt to setup a standard projection first
            ///////////////////////////////////////////////////////////
            try
            {
                std_proj = d_cfg;

                if (!::proj::projection_setup(&std_proj))
                {
                    fwd_type = inv_type = PROJ_STANDARD;
                    if (d_cfg.contains("proj_timestamp"))
                    {
                        // logger->warn("Using projection timestamps for timestamp feedback. May be inacurate!"); // Disabled usually as it triggers... Everytime the proj is init
                        proj_timestamp = d_cfg["proj_timestamp"];
                    }
                    return true;
                }
            }
            catch (std::exception &)
            {
                logger->trace("Not a standard projection!");
            }

            ///////////////////////////////////////////////////////////
            // If this didn't work, we can attempt a raytraced proj
            ///////////////////////////////////////////////////////////
            try
            {
                raytracer = get_satellite_raytracer(d_cfg);
                if (raytracer)
                {
                    fwd_type = PROJ_INVALID;
                    inv_type = PROJ_RAYTRACER;

                    if (fwd)
                    {
                        logger->critical("Forward on raytrace is imperfect!");
                        auto gcps = compute_gcps(d_cfg);
                        tps_fwd = std::make_shared<satdump::proj::LatLonTpsProjHelper>(gcps, 1, 0);
                        fwd_type = PROJ_THINPLATESPLINE;
                    }
                }
                return true;
            }
            catch (std::exception &e)
            {
                logger->trace("Not a raytraced projection! : %s", e.what());
            }

            ///////////////////////////////////////////////////////////
            // And finally, the special case of simple GCPs
            ///////////////////////////////////////////////////////////
            if (d_cfg["type"] == "normal_gcps")
                logger->critical("GCPs ALONE SUPPORT TBD!");

            return false;
        }

        bool Projection::forward(geodetic::geodetic_coords_t pos, double &x, double &y, bool except)
        {
            if (fwd_type == PROJ_STANDARD)
            {
                pos.toDegs(); // TODOREWORK?
                if (::proj::projection_perform_fwd(&std_proj, pos.lon, pos.lat, &x, &y))
                    return 1;
            }
            else if (fwd_type == PROJ_THINPLATESPLINE)
            {
                // Perform TPS
                tps_fwd->forward(pos, x, y);
                return 0; // We do NOT want to run the ChannelTransform in reverse, TPS takes care of it already!
            }
            else
            {
                if (except)
                    throw satdump_exception("Invalid forward projection type! " + d_cfg["type"].get<std::string>());
                else
                    return 1;
            }

            // Run channel transform, might do nothing if no transform is needed
            transform.reverse(&x, &y);
            if (has_2nd_transform)
                transform2.reverse(&x, &y);
            return 0;
        }

        bool Projection::inverse(double x, double y, geodetic::geodetic_coords_t &pos, double *otime, bool except)
        {
            // Run channel transform, might do nothing if no transform is needed
            if (has_2nd_transform)
                transform2.forward(&x, &y);
            transform.forward(&x, &y);

            if (inv_type == PROJ_STANDARD)
            {
                pos.toDegs(); // TODOREWORK?
                if (otime != nullptr)
                    *otime = proj_timestamp;
                return ::proj::projection_perform_inv(&std_proj, x, y, &pos.lon, &pos.lat);
            }
            else if (inv_type == PROJ_RAYTRACER)
            {
                return raytracer->get_position(x, y, pos, otime);
            }
            else
            {
                if (except)
                    throw satdump_exception("Invalid inverse projection type! " + d_cfg["type"].get<std::string>());
                else
                    return 1;
            }
        }

        void Projection::to_json(nlohmann::json &j) const
        {
            j = d_cfg;
            if (height != -1)
                j["height"] = height;
            if (width != -1)
                j["width"] = width;
        }

        void Projection::from_json(const nlohmann::json &j)
        {
            d_cfg = j; // TODOREWORK de-init?
            height = j.contains("height") ? j["height"].get<int>() : -1;
            width = j.contains("width") ? j["width"].get<int>() : -1;
            fwd_type = PROJ_INVALID;
            inv_type = PROJ_INVALID;
            init(0, 0);
        }
    } // namespace projection
} // namespace satdump