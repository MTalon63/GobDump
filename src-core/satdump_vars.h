#pragma once

#include "dll_export.h"
#include <string>

namespace satdump
{
    // Constant-initialized literals: RESPATH/LIBPATH read these during static init.
    SATDUMP_DLL extern const char *const RESOURCES_PATH;
    SATDUMP_DLL extern const char *const LIBRARIES_PATH;
    SATDUMP_DLL extern const std::string SATDUMP_VERSION;
    SATDUMP_DLL extern const std::string SATDUMP_VERSION_TAG;

    SATDUMP_DLL extern std::string RESPATH;
    SATDUMP_DLL extern std::string LIBPATH;

    std::string getSatDumpVersionName();
    std::string getExecutableDir();
} // namespace satdump