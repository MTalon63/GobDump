#include "warp_bkd.h"
#include "logger.h"
#include "core/exception.h"
#include <map>
#include "utils/binary.h"
#include "core/resources.h"
#include "core/opencl.h"
#include <chrono>
#include <cmath>
#include "utils/stats.h"

#include "common/geodetic/geodetic_coordinates.h"

namespace satdump
{
    namespace warp
    {
        double lon_shift(double lon, double shift)
        {
            if (shift == 0)
                return lon;
            lon += shift;
            if (lon > 180)
                lon -= 360;
            if (lon < -180)
                lon += 360;
            return lon;
        }

        void shift_latlon_by_lat(double *lat, double *lon, double shift)
        {
            if (shift == 0)
                return;

            double x = cos(*lat * DEG_TO_RAD) * cos(*lon * DEG_TO_RAD);
            double y = cos(*lat * DEG_TO_RAD) * sin(*lon * DEG_TO_RAD);
            double z = sin(*lat * DEG_TO_RAD);

            double theta = shift * DEG_TO_RAD;

            double x2 = x * cos(theta) + z * sin(theta);
            double y2 = y;
            double z2 = z * cos(theta) - x * sin(theta);

            *lon = atan2(y2, x2) * RAD_TO_DEG;
            double hyp = sqrt(x2 * x2 + y2 * y2);
            *lat = atan2(z2, hyp) * RAD_TO_DEG;
        }

        std::shared_ptr<projection::VizGeorefSpline2D> initTPSTransform(WarpOperation &op)
        {
            return initTPSTransform(op.ground_control_points, op.shift_lon, op.shift_lat);
        }

        std::shared_ptr<projection::VizGeorefSpline2D> initTPSTransform(std::vector<projection::GCP> gcps, int shift_lon, int shift_lat)
        {
            std::shared_ptr<projection::VizGeorefSpline2D> spline_transform = std::make_shared<projection::VizGeorefSpline2D>(2);

            // Attach (non-redundant) points to the transformation.
            std::map<std::pair<double, double>, int> oMapPixelLineToIdx;
            std::map<std::pair<double, double>, int> oMapXYToIdx;
            for (int iGCP = 0; iGCP < (int)gcps.size(); iGCP++)
            {
                double final_lon = lon_shift(gcps[iGCP].lon, shift_lon);
                double final_lat = gcps[iGCP].lat;

                shift_latlon_by_lat(&final_lat, &final_lon, shift_lat);

                const double afPL[2] = {gcps[iGCP].x, gcps[iGCP].y};
                const double afXY[2] = {final_lon, final_lat};

                std::map<std::pair<double, double>, int>::iterator oIter(oMapPixelLineToIdx.find(std::pair<double, double>(afPL[0], afPL[1])));

                if (oIter != oMapPixelLineToIdx.end())
                {
                    if (afXY[0] == gcps[oIter->second].lon && afXY[1] == gcps[oIter->second].lat)
                        continue;
                    else
                    {
                        logger->warn("2 GCPs have the same X,Y!");
                        continue;
                    }
                }
                else
                    oMapPixelLineToIdx[std::pair<double, double>(afPL[0], afPL[1])] = iGCP;

                if (oMapXYToIdx.find(std::pair<double, double>(afXY[0], afXY[1])) != oMapXYToIdx.end())
                {
                    logger->warn("2 GCPs have the same Lat,Lon!");
                    continue;
                }
                else
                    oMapXYToIdx[std::pair<double, double>(afXY[0], afXY[1])] = iGCP;

                if (!spline_transform->add_point(afXY[0], afXY[1], afPL))
                {
                    logger->error("Error generating transformer!");
                    // return 1;
                }
            }

            logger->info("Solving TPS equations for %d GCPs...", gcps.size());
            auto solve_start = std::chrono::system_clock::now();
            bool solved = spline_transform->solve() != 0;
            if (solved)
                logger->info("Solved! Took %f", (std::chrono::system_clock::now() - solve_start).count() / 1e9);
            else
                logger->error("Failure solving!");

            return spline_transform;
        }

        WarpCropSettings choseCropArea(WarpOperation &op)
        {
            WarpCropSettings cset;
            cset.lat_min = -90;
            cset.lat_max = 90;
            cset.lon_min = -180;
            cset.lon_max = 180;
            cset.y_min = 0;
            cset.y_max = op.output_height;
            cset.x_min = 0;
            cset.x_max = op.output_width;

            std::vector<double> lat_values;
            std::vector<double> lon_values;
            for (projection::GCP &g : op.ground_control_points)
            {
                lat_values.push_back(g.lat);
                lon_values.push_back(g.lon);
            }

            double lat_min = 0;
            double lat_max = 0;
            double lon_min = 0;
            double lon_max = 0;
            lat_min = lat_max = avg_overflowless(lat_values);
            lon_min = lon_max = avg_overflowless(lon_values);

            for (projection::GCP &g : op.ground_control_points)
            {
                if (g.lat > lat_max)
                    lat_max = g.lat;
                if (g.lat < lat_min)
                    lat_min = g.lat;

                if (g.lon > lon_max)
                    lon_max = g.lon;
                if (g.lon < lon_min)
                    lon_min = g.lon;
            }

            // Round to integer degrees
            cset.lat_min = floor(lat_min);
            cset.lon_min = floor(lon_min);
            cset.lat_max = ceil(lat_max);
            cset.lon_max = ceil(lon_max);

            if (op.shift_lat == 90)
                cset.lat_max = 90;
            if (op.shift_lat == -90)
                cset.lat_min = -90;

            // Compute to pixels
            cset.y_max = op.output_height - ((90.0f + cset.lat_min) / 180.0f) * op.output_height;
            cset.y_min = op.output_height - ((90.0f + cset.lat_max) / 180.0f) * op.output_height;
            cset.x_min = (cset.lon_min / 360.0f) * op.output_width + (op.output_width / 2);
            cset.x_max = (cset.lon_max / 360.0f) * op.output_width + (op.output_width / 2);

            // Pixels can offset it a bit - recompute to be 100% accurate
            cset.lat_max = ((op.output_height - cset.y_min) / (double)op.output_height) * 180.0f - 90.0f;
            cset.lat_min = ((op.output_height - cset.y_max) / (double)op.output_height) * 180.0f - 90.0f;
            cset.lon_min = (cset.x_min / (double)op.output_width) * 360.0f - 180.0f;
            cset.lon_max = (cset.x_max / (double)op.output_width) * 360.0f - 180.0f;

            return cset;
        }

        void ImageWarper::warpOnCPU(WarpResult &result)
        {
            // Now, warp the image
            auto cpu_start = std::chrono::system_clock::now();
            {
#pragma omp parallel for
                for (int64_t xy_ptr = 0; xy_ptr < (int64_t)result.output_image.width() * (int64_t)result.output_image.height(); xy_ptr++)
                {
                    double xx, yy;
                    double xy[2];
                    int x = (xy_ptr % result.output_image.width());
                    int y = (xy_ptr / result.output_image.width());

                    // Scale to the map
                    double lat = -((double)(y + crop_set.y_min) / (double)op.output_height) * 180 + 90;
                    double lon = ((double)(x + crop_set.x_min) / (double)op.output_width) * 360 - 180;

                    // Perform TPS
                    shift_latlon_by_lat(&lat, &lon, op.shift_lat);
                    tps->get_point(lon_shift(lon, op.shift_lon), lat, xy);
                    xx = xy[0];
                    yy = xy[1];

                    // isfinite first: NaN passes every comparison, and casting a non-finite or
                    // out-of-range double to int is UB (measured: (int)NaN == (int)1e300 == INT_MIN).
                    if (!std::isfinite(xx) || !std::isfinite(yy))
                        continue;

                    if (xx < 0 || yy < 0)
                        continue;

                    if (xx >= (double)op.input_image->width() || yy >= (double)op.input_image->height())
                        continue;

                    if (result.output_image.channels() == 4)
                    {
                        if (op.input_image->channels() == 1)
                            for (int c = 0; c < 3; c++)
                                result.output_image.set(c, y * result.output_image.width() + x, op.input_image->get_pixel_bilinear(0, xx, yy));
                        else if (op.input_image->channels() == 3 || op.input_image->channels() == 4)
                            for (int c = 0; c < 3; c++)
                                result.output_image.set(c, y * result.output_image.width() + x, op.input_image->get_pixel_bilinear(c, xx, yy));

                        if (op.input_image->channels() == 4)
                            result.output_image.set(3, y * result.output_image.width() + x, op.input_image->get_pixel_bilinear(3, xx, yy));
                        else
                            result.output_image.set(3, y * result.output_image.width() + x, 65535);
                    }
                    else
                    {
                        for (int c = 0; c < op.input_image->channels(); c++)
                            result.output_image.set(c, y * result.output_image.width() + x, op.input_image->get_pixel_bilinear(c, xx, yy));
                    }
                }
            }
            auto cpu_time = (std::chrono::system_clock::now() - cpu_start);
            logger->debug("CPU Processing Time %f", cpu_time.count() / 1e9);
        }

#ifdef USE_OPENCL
        void ImageWarper::warpOnGPU_fp64(WarpResult &result)
        {
            cl_int err = 0;
            auto &context = satdump::opencl::ocl_context;
            auto &device = satdump::opencl::ocl_device;

            // RAII wrappers ensure every OpenCL object is released even if a throw
            // happens midway through setup.
            opencl::cl_program_raii warping_program(opencl::buildCLKernel(resources::getResourcePath("opencl/warp_image_thin_plate_spline_fp64.cl")));

            auto gpu_start = std::chrono::system_clock::now();

            // Images
            opencl::cl_mem_raii buffer_map(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(uint16_t) * result.output_image.size(), NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_map! Code " + std::to_string(err));
            opencl::cl_mem_raii buffer_img(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(uint16_t) * op.input_image->size(), NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_img! Code " + std::to_string(err));

            // TPS Stuff
            opencl::cl_mem_raii buffer_tps_npoints(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(int), NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_tps_npoints! Code " + std::to_string(err));
            opencl::cl_mem_raii buffer_tps_x(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(double) * tps->_nof_points, NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_tps_x! Code " + std::to_string(err));
            opencl::cl_mem_raii buffer_tps_y(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(double) * tps->_nof_points, NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_tps_y! Code " + std::to_string(err));
            opencl::cl_mem_raii buffer_tps_coefs1(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(double) * tps->_nof_eqs, NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_tps_coefs1! Code " + std::to_string(err));
            opencl::cl_mem_raii buffer_tps_coefs2(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(double) * tps->_nof_eqs, NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_tps_coefs2! Code " + std::to_string(err));
            opencl::cl_mem_raii buffer_tps_xmean(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(double), NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_tps_xmean! Code " + std::to_string(err));
            opencl::cl_mem_raii buffer_tps_ymean(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(double), NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_tps_ymean! Code " + std::to_string(err));

            int img_settings[] = {op.output_width, op.output_height,
                                  (int)op.input_image->width(), (int)op.input_image->height(),
                                  op.input_image->channels(),
                                  result.output_image.channels(),
                                  crop_set.y_min, crop_set.y_max,
                                  crop_set.x_min, crop_set.x_max,
                                  op.shift_lon, op.shift_lat};

            opencl::cl_mem_raii buffer_img_settings(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(int) * 12, NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_img_settings! Code " + std::to_string(err));

            // Create an OpenCL queue
            opencl::cl_queue_raii queue(clCreateCommandQueue(context, device, 0, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't create OpenCL queue! Code " + std::to_string(err));

            // Write all of buffers to the GPU
            clEnqueueWriteBuffer(queue.get(), buffer_map.get(), true, 0, sizeof(uint16_t) * result.output_image.size(), result.output_image.raw_data(), 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_img.get(), true, 0, sizeof(uint16_t) * op.input_image->size(), op.input_image->raw_data(), 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_tps_npoints.get(), true, 0, sizeof(int), &tps->_nof_points, 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_tps_x.get(), true, 0, sizeof(double) * tps->_nof_points, tps->x, 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_tps_y.get(), true, 0, sizeof(double) * tps->_nof_points, tps->y, 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_tps_coefs1.get(), true, 0, sizeof(double) * tps->_nof_eqs, tps->coef[0], 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_tps_coefs2.get(), true, 0, sizeof(double) * tps->_nof_eqs, tps->coef[1], 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_tps_xmean.get(), true, 0, sizeof(double), &tps->x_mean, 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_tps_ymean.get(), true, 0, sizeof(double), &tps->y_mean, 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_img_settings.get(), true, 0, sizeof(int) * 12, img_settings, 0, NULL, NULL);

            // Init the kernel
            opencl::cl_kernel_raii warping_kernel(clCreateKernel(warping_program.get(), "warp_image_thin_plate_spline", &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't create OpenCL kernel! Code " + std::to_string(err));
            clSetKernelArg(warping_kernel.get(), 0, sizeof(cl_mem), &buffer_map);
            clSetKernelArg(warping_kernel.get(), 1, sizeof(cl_mem), &buffer_img);
            clSetKernelArg(warping_kernel.get(), 2, sizeof(cl_mem), &buffer_tps_npoints);
            clSetKernelArg(warping_kernel.get(), 3, sizeof(cl_mem), &buffer_tps_x);
            clSetKernelArg(warping_kernel.get(), 4, sizeof(cl_mem), &buffer_tps_y);
            clSetKernelArg(warping_kernel.get(), 5, sizeof(cl_mem), &buffer_tps_coefs1);
            clSetKernelArg(warping_kernel.get(), 6, sizeof(cl_mem), &buffer_tps_coefs2);
            clSetKernelArg(warping_kernel.get(), 7, sizeof(cl_mem), &buffer_tps_xmean);
            clSetKernelArg(warping_kernel.get(), 8, sizeof(cl_mem), &buffer_tps_ymean);
            clSetKernelArg(warping_kernel.get(), 9, sizeof(cl_mem), &buffer_img_settings);

            // Get proper workload size
            size_t size_wg = 0;
            cl_uint compute_units = 0;
            clGetDeviceInfo(device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(size_t), &size_wg, NULL);
            clGetDeviceInfo(device, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cl_uint), &compute_units, NULL);

            logger->debug("Workgroup size %d", size_wg * compute_units);

            // Run the kernel!
            size_t total_wg_size = int(size_wg) * int(compute_units);
            err = clEnqueueNDRangeKernel(queue.get(), warping_kernel.get(), 1, NULL, &total_wg_size, NULL, 0, NULL, NULL);
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't clEnqueueNDRangeKernel! Code " + std::to_string(err));

            // Read image result back from VRAM
            clEnqueueReadBuffer(queue.get(), buffer_map.get(), true, 0, sizeof(uint16_t) * result.output_image.size(), result.output_image.raw_data(), 0, NULL, NULL);

            // RAII wrappers release everything on scope exit, including the program.
            auto gpu_time = (std::chrono::system_clock::now() - gpu_start);
            logger->debug("GPU Processing Time %f", gpu_time.count() / 1e9);
        }

        void ImageWarper::warpOnGPU_fp32(WarpResult &result)
        {
            cl_int err = 0;
            auto &context = satdump::opencl::ocl_context;
            auto &device = satdump::opencl::ocl_device;

            opencl::cl_program_raii warping_program(opencl::buildCLKernel(resources::getResourcePath("opencl/warp_image_thin_plate_spline_fp32.cl")));

            auto gpu_start = std::chrono::system_clock::now();

            opencl::cl_mem_raii buffer_map(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(uint16_t) * result.output_image.size(), NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_map! Code " + std::to_string(err));
            opencl::cl_mem_raii buffer_img(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(uint16_t) * op.input_image->size(), NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_img! Code " + std::to_string(err));

            opencl::cl_mem_raii buffer_tps_npoints(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(int), NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_tps_npoints! Code " + std::to_string(err));
            opencl::cl_mem_raii buffer_tps_x(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(float) * tps->_nof_points, NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_tps_x! Code " + std::to_string(err));
            opencl::cl_mem_raii buffer_tps_y(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(float) * tps->_nof_points, NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_tps_y! Code " + std::to_string(err));
            opencl::cl_mem_raii buffer_tps_coefs1(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(float) * tps->_nof_eqs, NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_tps_coefs1! Code " + std::to_string(err));
            opencl::cl_mem_raii buffer_tps_coefs2(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(float) * tps->_nof_eqs, NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_tps_coefs2! Code " + std::to_string(err));
            opencl::cl_mem_raii buffer_tps_xmean(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(float), NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_tps_xmean! Code " + std::to_string(err));
            opencl::cl_mem_raii buffer_tps_ymean(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(float), NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_tps_ymean! Code " + std::to_string(err));

            int img_settings[] = {op.output_width, op.output_height,
                                  (int)op.input_image->width(), (int)op.input_image->height(),
                                  op.input_image->channels(),
                                  result.output_image.channels(),
                                  crop_set.y_min, crop_set.y_max,
                                  crop_set.x_min, crop_set.x_max,
                                  op.shift_lon, op.shift_lat};

            opencl::cl_mem_raii buffer_img_settings(clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(int) * 12, NULL, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't load buffer_img_settings! Code " + std::to_string(err));

            opencl::cl_queue_raii queue(clCreateCommandQueue(context, device, 0, &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't create OpenCL queue! Code " + std::to_string(err));

            clEnqueueWriteBuffer(queue.get(), buffer_map.get(), true, 0, sizeof(uint16_t) * result.output_image.size(), result.output_image.raw_data(), 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_img.get(), true, 0, sizeof(uint16_t) * op.input_image->size(), op.input_image->raw_data(), 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_tps_npoints.get(), true, 0, sizeof(int), &tps->_nof_points, 0, NULL, NULL);
            std::vector<float> tps_x = double_buffer_to_float(tps->x, tps->_nof_points);
            std::vector<float> tps_y = double_buffer_to_float(tps->y, tps->_nof_points);
            std::vector<float> tps_coef1 = double_buffer_to_float(tps->coef[0], tps->_nof_eqs);
            std::vector<float> tps_coef2 = double_buffer_to_float(tps->coef[1], tps->_nof_eqs);
            float tps_x_mean = tps->x_mean;
            float tps_y_mean = tps->y_mean;
            clEnqueueWriteBuffer(queue.get(), buffer_tps_x.get(), true, 0, sizeof(float) * tps->_nof_points, tps_x.data(), 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_tps_y.get(), true, 0, sizeof(float) * tps->_nof_points, tps_y.data(), 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_tps_coefs1.get(), true, 0, sizeof(float) * tps->_nof_eqs, tps_coef1.data(), 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_tps_coefs2.get(), true, 0, sizeof(float) * tps->_nof_eqs, tps_coef2.data(), 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_tps_xmean.get(), true, 0, sizeof(float), &tps_x_mean, 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_tps_ymean.get(), true, 0, sizeof(float), &tps_y_mean, 0, NULL, NULL);
            clEnqueueWriteBuffer(queue.get(), buffer_img_settings.get(), true, 0, sizeof(int) * 12, img_settings, 0, NULL, NULL);

            opencl::cl_kernel_raii warping_kernel(clCreateKernel(warping_program.get(), "warp_image_thin_plate_spline", &err));
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't create OpenCL kernel! Code " + std::to_string(err));
            clSetKernelArg(warping_kernel.get(), 0, sizeof(cl_mem), &buffer_map);
            clSetKernelArg(warping_kernel.get(), 1, sizeof(cl_mem), &buffer_img);
            clSetKernelArg(warping_kernel.get(), 2, sizeof(cl_mem), &buffer_tps_npoints);
            clSetKernelArg(warping_kernel.get(), 3, sizeof(cl_mem), &buffer_tps_x);
            clSetKernelArg(warping_kernel.get(), 4, sizeof(cl_mem), &buffer_tps_y);
            clSetKernelArg(warping_kernel.get(), 5, sizeof(cl_mem), &buffer_tps_coefs1);
            clSetKernelArg(warping_kernel.get(), 6, sizeof(cl_mem), &buffer_tps_coefs2);
            clSetKernelArg(warping_kernel.get(), 7, sizeof(cl_mem), &buffer_tps_xmean);
            clSetKernelArg(warping_kernel.get(), 8, sizeof(cl_mem), &buffer_tps_ymean);
            clSetKernelArg(warping_kernel.get(), 9, sizeof(cl_mem), &buffer_img_settings);

            size_t size_wg = 0;
            cl_uint compute_units = 0;
            clGetDeviceInfo(device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(size_t), &size_wg, NULL);
            clGetDeviceInfo(device, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cl_uint), &compute_units, NULL);

            logger->debug("Workgroup size %d", size_wg * compute_units);

            size_t total_wg_size = int(size_wg) * int(compute_units);
            err = clEnqueueNDRangeKernel(queue.get(), warping_kernel.get(), 1, NULL, &total_wg_size, NULL, 0, NULL, NULL);
            if (err != CL_SUCCESS)
                throw satdump_exception("Couldn't clEnqueueNDRangeKernel! Code " + std::to_string(err));

            clEnqueueReadBuffer(queue.get(), buffer_map.get(), true, 0, sizeof(uint16_t) * result.output_image.size(), result.output_image.raw_data(), 0, NULL, NULL);

            auto gpu_time = (std::chrono::system_clock::now() - gpu_start);
            logger->debug("GPU Processing Time %f", gpu_time.count() / 1e9);
        }
#endif

        void ImageWarper::update(bool skip_tps)
        {
            if (!skip_tps)
                tps = initTPSTransform(op);
            crop_set = choseCropArea(op);
        }

        WarpResult ImageWarper::warp(bool force_double)
        {
            WarpResult result;

            // Prepare the output
            result.output_image = image::Image(16, // TODOIMG ALLOW 8-bits
                                                crop_set.x_max - crop_set.x_min, crop_set.y_max - crop_set.y_min,
                                                op.output_rgba ? 4 : op.input_image->channels());
            result.top_left = {0, 0, (double)crop_set.lon_min, (double)crop_set.lat_max};                                                                                  // 0,0
            result.top_right = {(double)result.output_image.width() - 1, 0, (double)crop_set.lon_max, (double)crop_set.lat_max};                                           // 1,0
            result.bottom_left = {0, (double)result.output_image.height() - 1, (double)crop_set.lon_min, (double)crop_set.lat_min};                                        // 0,1
            result.bottom_right = {(double)result.output_image.width() - 1, (double)result.output_image.height() - 1, (double)crop_set.lon_max, (double)crop_set.lat_min}; // 1,1

#ifdef USE_OPENCL
            if (satdump::opencl::useCL())
            {
                try
                {
                    logger->debug("Using GPU! Double precision requested %d", (int)force_double);
                    satdump::opencl::setupOCLContext();
                    if (force_double)
                        warpOnGPU_fp64(result);
                    else
                        warpOnGPU_fp32(result);
                    return result;
                }
                catch (std::runtime_error &e)
                {
                    logger->error("Error warping on GPU : %s", e.what());
                }
            }
#endif

            logger->debug("Using CPU!");
            warpOnCPU(result);

            return result;
        }
    }
}