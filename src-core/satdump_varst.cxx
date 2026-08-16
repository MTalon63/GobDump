#define SATDUMP_DLL_EXPORT 1
#include "satdump_vars.h"

namespace satdump
{
    // extern is required: namespace-scope const defaults to internal linkage,
    // which GCC rejects when combined with dllexport.
    SATDUMP_DLL extern const std::string RESOURCES_PATH = "@RESOURCES_PATH@";
    SATDUMP_DLL extern const std::string LIBRARIES_PATH = "@LIBRARIES_PATH@";
    SATDUMP_DLL extern const std::string SATDUMP_VERSION = "@GOBDUMP_VERSION@";
    SATDUMP_DLL extern const std::string SATDUMP_VERSION_TAG = "@GOBDUMP_VERSION_TAG@";
} // namespace satdump