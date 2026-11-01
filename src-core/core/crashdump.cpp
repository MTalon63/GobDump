#include "crashdump.h"

#ifdef SATDUMP_CRASHDUMP
#include "utils/format.h"
#ifdef SATDUMP_CRASHDUMP_CPPTRACE
#include <cpptrace/cpptrace.hpp>
#else
#include <backtrace.h>
#endif
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>
#endif

namespace satdump
{
#ifdef SATDUMP_CRASHDUMP
#ifndef SATDUMP_CRASHDUMP_CPPTRACE
    static backtrace_state *bt_state = nullptr;

    static int bt_callback(void *data, uintptr_t pc, const char *filename, int lineno, const char *function)
    {
        (void)pc;
        std::string &report = *((std::string *)data);
        report += satdump::svformat("  at %s (%s:%d)\n", function ? function : "??", filename ? filename : "??", lineno);
        return 0;
    }

    static void bt_error(void *data, const char *msg, int errnum)
    {
        std::string &report = *((std::string *)data);
        report += satdump::svformat("  [backtrace error: %s (%d)]\n", msg, errnum);
    }
#endif

    static void crash_handler(int sig)
    {
        std::string report;

        report += "\n";
        report += "                __        __\n";
        report += "   ____ _____  / /_  ____/ /_  ______ ___  ____\n";
        report += "  / __ `/ __ \\/ __ \\/ __  / / / / __ `__ \\/ __ \\\n";
        report += " / /_/ / /_/ / /_/ / /_/ / /_/ / / / / / / /_/ /\n";
        report += " \\__, /\\____/_.___/\\__,_/\\__,_/_/ /_/ /_/ .___/\n";
        report += "/____/                                 /_/\n";

        report += "GobDump ran into an error and had to terminate.\n";
        report += "You can find a backtrace below to share with the\n";
        report += "developers for debugging purposes.\n";
        report += "\n=== START BACKTRACE ===\n\n";

        report += "Crash report (signal " + std::to_string(sig) + ")\n\n";

#ifdef SATDUMP_CRASHDUMP_CPPTRACE
        report += cpptrace::generate_trace().to_string();
#else
        backtrace_full(bt_state, 0, bt_callback, bt_error, &report);
#endif

        fprintf(stderr, "\n%s\n", report.c_str());

        std::ofstream("gobdump_crash_" + std::to_string(time(NULL)) + ".txt").write(report.c_str(), report.size());

        signal(sig, SIG_DFL);
        exit(1);
    }
#endif

    void initCrashDump()
    {
#ifdef SATDUMP_CRASHDUMP

#ifndef SATDUMP_CRASHDUMP_CPPTRACE
        bt_state = backtrace_create_state(NULL, 1, bt_error, nullptr);
#endif

        signal(SIGSEGV, crash_handler);
        signal(SIGABRT, crash_handler);
        signal(SIGFPE, crash_handler);
        signal(SIGILL, crash_handler);
#ifndef _WIN32
        signal(SIGBUS, crash_handler);
#endif

#endif
    }
} // namespace satdump
