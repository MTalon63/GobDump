#define SATDUMP_DLL_EXPORT 1
#include "satdump_vars.h"

#include <filesystem>

#if defined(__APPLE__)
#include <filesystem>
#include <libgen.h>
#include <mach-o/dyld.h>
#define LIBRARIES_SEARCH_PATH "/../Resources/"
#define RESOURCES_SEARCH_PATH "/../Resources/"
#elif defined(_WIN32)
#include <Windows.h>
#include <filesystem>
#include <shlwapi.h>
#define LIBRARIES_SEARCH_PATH "\\..\\lib\\gobdump\\"
#define RESOURCES_SEARCH_PATH "\\..\\"
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace satdump
{
#if defined(__APPLE__)
    std::string get_search_path(const char *target)
    {
        uint32_t bufsize = PATH_MAX;
        char exec_path[bufsize], ret_val[bufsize], search_dir[bufsize];
        char *exec_dir;
        _NSGetExecutablePath(exec_path, &bufsize);
        exec_dir = dirname(exec_path);
        strcpy(search_dir, exec_dir);
        strcat(search_dir, target);
        realpath(search_dir, ret_val);
        return std::string(ret_val) + "/";
    }
#elif defined(_WIN32)
    std::string get_search_path(const char *target)
    {
        char exe_path[MAX_PATH], ret_val[MAX_PATH];
        GetModuleFileNameA(NULL, exe_path, MAX_PATH);
        PathRemoveFileSpecA(exe_path);
        strcat_s(exe_path, MAX_PATH, target);
        PathCanonicalizeA(ret_val, exe_path);
        return std::string(ret_val);
    }
#endif

#if defined(__APPLE__) || defined(_WIN32)
    std::string init_res_path()
    {
        std::string search_dir = get_search_path(RESOURCES_SEARCH_PATH);
        if (std::filesystem::exists(search_dir) && std::filesystem::is_directory(search_dir))
            return search_dir;
        else
#ifdef _WIN32
            return get_search_path("\\");
#else
            return std::string(RESOURCES_PATH);
#endif
    }
    std::string init_lib_path()
    {
        std::string search_dir = get_search_path(LIBRARIES_SEARCH_PATH);
        if (std::filesystem::exists(search_dir) && std::filesystem::is_directory(search_dir))
            return search_dir;
        else
#ifdef _WIN32
            return get_search_path("\\");
#else
            return std::string(LIBRARIES_PATH);
#endif
    }
#elif defined(__ANDROID__)
    std::string init_res_path() { return "./"; }
    std::string init_lib_path() { return "./"; }
#else
    std::string init_res_path() { return std::string(RESOURCES_PATH); }
    std::string init_lib_path() { return std::string(LIBRARIES_PATH); }
#endif
    std::string RESPATH = init_res_path();
    std::string LIBPATH = init_lib_path();

    std::string getSatDumpVersionName() { return "GobDump v" + (std::string)SATDUMP_VERSION + (satdump::SATDUMP_VERSION_TAG.size() ? " (" + satdump::SATDUMP_VERSION_TAG + ")" : ""); }

    std::string getExecutableDir()
    {
#if defined(__APPLE__)
        uint32_t bufsize = 0;
        _NSGetExecutablePath(nullptr, &bufsize);
        std::string exec_path(bufsize, '\0');
        if (_NSGetExecutablePath(exec_path.data(), &bufsize) != 0)
            return "";
        return std::filesystem::path(exec_path.c_str()).parent_path().string();
#elif defined(_WIN32)
        char exe_path[MAX_PATH];
        if (GetModuleFileNameA(NULL, exe_path, MAX_PATH) == 0)
            return "";
        PathRemoveFileSpecA(exe_path);
        return std::string(exe_path);
#else
        char exe_path[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len <= 0)
            return "";
        exe_path[len] = '\0';
        return std::filesystem::path(exe_path).parent_path().string();
#endif
    }
} // namespace satdump
