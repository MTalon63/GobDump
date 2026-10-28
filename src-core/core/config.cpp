#define SATDUMP_DLL_EXPORT 1
#include "config.h"
#include "logger.h"
#include "nlohmann/json_utils.h"
#include <filesystem>

#if defined(__APPLE__)
#include <glob.h>
#include <sysdir.h>
#elif defined(_WIN32)
#include <Shlobj.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

#include <cstdlib>

namespace satdump
{
    SATDUMP_DLL SatDumpConfigHandler satdump_cfg;

#if !defined(__APPLE__) && !defined(__ANDROID__)
    static bool isPathWritable(const char *path)
    {
#if defined(_WIN32)
        HANDLE test_handle = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (test_handle == INVALID_HANDLE_VALUE)
            return false;
        CloseHandle(test_handle);
        return true;
#else
        return access(path, W_OK | X_OK) == 0;
#endif
    }
#endif

    void SatDumpConfigHandler::checkOutputDirs()
    {
        std::string fallback_dir = "";
#if defined(__APPLE__)
        // Always overwrite . on macOS since inside the app bundle can be writable
        char documents_glob[PATH_MAX];
        glob_t globbuf;
        sysdir_search_path_enumeration_state search_state = sysdir_start_search_path_enumeration(SYSDIR_DIRECTORY_DOCUMENT, SYSDIR_DOMAIN_MASK_USER);
        sysdir_get_next_search_path_enumeration(search_state, documents_glob);
        glob(documents_glob, GLOB_TILDE, nullptr, &globbuf);
        fallback_dir = std::string(globbuf.gl_pathv[0]);
        globfree(&globbuf);

#elif defined(_WIN32)
        // Only set output to Documents if the current dir is not writable
        if (!isPathWritable("."))
        {
            PWSTR documents_wide;
            SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, NULL, &documents_wide);
            std::wstring documents_widestr = std::wstring(documents_wide);
            int len = WideCharToMultiByte(CP_UTF8, 0, documents_widestr.c_str(), (int)documents_widestr.size(), nullptr, 0, nullptr, nullptr);
            fallback_dir = std::string(len, '\0');
            WideCharToMultiByte(CP_UTF8, 0, documents_widestr.c_str(), (int)documents_widestr.size(), (LPSTR)fallback_dir.data(), (int)fallback_dir.size(), nullptr, nullptr);
        }
#elif !defined(__ANDROID__)
        // AppImages are mounted read-only and some launchers start the binary with
        // the mount as its working directory, so creating ./live_output fails with
        // "Read-only file system". Fall back to $HOME, which is what the relative
        // defaults resolve to when an installed build is started from the desktop.
        // A writable working directory is still honored, so running from a terminal
        // keeps writing where the program was started.
        if (!isPathWritable("."))
        {
            const char *home = getenv("HOME");
            if (home != nullptr && home[0] == '/' && isPathWritable(home))
            {
                fallback_dir = home;
                logger->info("Current directory is not writable, using $HOME for default output directories");
            }
            else
                logger->warn("Current directory is not writable and no usable $HOME was found for output directories");
        }
#endif
        if (fallback_dir == "")
            return;

        bool bad_path = false;
        if (main_cfg["satdump_directories"]["recording_path"]["value"].get<std::string>() == ".")
        {
            bad_path = true;
            main_cfg["satdump_directories"]["recording_path"]["value"] = fallback_dir;
        }
        if (main_cfg["satdump_directories"]["live_processing_path"]["value"].get<std::string>() == "./live_output")
        {
            bad_path = true;
            main_cfg["satdump_directories"]["live_processing_path"]["value"] = fallback_dir + "/live_output";
        }
        if (main_cfg["satdump_directories"]["default_input_directory"]["value"].get<std::string>() == ".")
        {
            bad_path = true;
            main_cfg["satdump_directories"]["default_input_directory"]["value"] = fallback_dir;
        }
        if (main_cfg["satdump_directories"]["default_output_directory"]["value"].get<std::string>() == ".")
        {
            bad_path = true;
            main_cfg["satdump_directories"]["default_output_directory"]["value"] = fallback_dir;
        }
        if (main_cfg["satdump_directories"]["default_image_output_directory"]["value"].get<std::string>() == ".")
        {
            bad_path = true;
            main_cfg["satdump_directories"]["default_image_output_directory"]["value"] = fallback_dir;
        }
        if (main_cfg["satdump_directories"]["default_projection_output_directory"]["value"].get<std::string>() == ".")
        {
            bad_path = true;
            main_cfg["satdump_directories"]["default_projection_output_directory"]["value"] = fallback_dir;
        }
        if (bad_path)
            saveUser();
    }

    void SatDumpConfigHandler::load(std::string path, std::string user_path)
    {
        if (!std::filesystem::exists(path))
        {
            logger->error("Couldn't load config file! Was trying : " + path);
            exit(1);
        }

        logger->info("Loading config " + path);
        default_cfg = loadJsonFile(path);
        main_cfg = default_cfg;

        loadUser(user_path);
    }

    void SatDumpConfigHandler::loadUser(std::string user_path)
    {
        std::string final_path = "";

        if (std::filesystem::exists("settings.json")) // First try loading in current folder
            final_path = "settings.json";
        else if (std::filesystem::exists(user_path + "/settings.json"))
            final_path = user_path + "/settings.json";

        if (final_path != "")
        {
            logger->info("Loading user settings " + final_path);
            nlohmann::ordered_json diff_json = loadJsonFile(final_path);
            main_cfg = merge_json_diffs(default_cfg, diff_json);
            user_cfg_path = final_path;
        }
        else
        {
            logger->warn("No user configuration found! Keeping defaults.");
            user_cfg_path = user_path + "/settings.json";
        }

        checkOutputDirs();
    }

    void SatDumpConfigHandler::saveUser()
    {
        nlohmann::ordered_json diff_json = perform_json_diff(default_cfg, main_cfg); // Get diff between user settings and the master CFG

        try
        {
            if (!std::filesystem::exists(std::filesystem::path(user_cfg_path).parent_path()) && std::filesystem::path(user_cfg_path).has_parent_path())
                std::filesystem::create_directories(std::filesystem::path(user_cfg_path).parent_path());
        }
        catch (std::exception &e)
        {
            logger->error("Cannot create directory for user config: %s", e.what());
            return;
        }

        logger->info("Saving user config at " + user_cfg_path);
        saveJsonFile(user_cfg_path, diff_json);
    }
} // namespace satdump
