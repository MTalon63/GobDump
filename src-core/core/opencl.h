#pragma once

#ifdef USE_OPENCL
#define CL_TARGET_OPENCL_VERSION 110
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#elif defined(__ANDROID__)
#include "libs/libopencl_loader/opencl_loader.h"
#include "libs/libopencl_loader/CL/cl.hpp"
#else
#include <CL/cl.h>
#endif
#include <string>
#include <vector>
#include <memory>

namespace satdump
{
    namespace opencl
    {
        struct OCLDevice
        {
            int platform_id;
            int device_id;
            std::string name;
        };

        std::vector<OCLDevice> getAllDevices();

        void initOpenCL();

        extern cl_context ocl_context;
        extern cl_device_id ocl_device;

        bool useCL();
        void setupOCLContext();
        std::vector<OCLDevice> resetOCLContext();
        // If the cache is enabled, you should NOT free the kernel
        cl_program buildCLKernel(std::string path, bool use_cache = true);

        // RAII wrappers for OpenCL objects. Each releases its handle on destruction,
        // so throwing out of a GPU warp function can no longer leak buffers, the
        // queue, the kernel, or the program.
        struct CLObjectDeleter
        {
            void operator()(cl_mem obj) const { if (obj) clReleaseMemObject(obj); }
            void operator()(cl_kernel obj) const { if (obj) clReleaseKernel(obj); }
            void operator()(cl_command_queue obj) const { if (obj) clReleaseCommandQueue(obj); }
            void operator()(cl_program obj) const { if (obj) clReleaseProgram(obj); }
        };
        using cl_mem_raii = std::unique_ptr<std::remove_pointer<cl_mem>::type, CLObjectDeleter>;
        using cl_kernel_raii = std::unique_ptr<std::remove_pointer<cl_kernel>::type, CLObjectDeleter>;
        using cl_queue_raii = std::unique_ptr<std::remove_pointer<cl_command_queue>::type, CLObjectDeleter>;
        using cl_program_raii = std::unique_ptr<std::remove_pointer<cl_program>::type, CLObjectDeleter>;
    }
}
#endif
