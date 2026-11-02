#include "crashdump.h"

#ifdef SATDUMP_CRASHDUMP
#include "utils/format.h"
#ifdef SATDUMP_CRASHDUMP_CPPTRACE
#include <cpptrace/cpptrace.hpp>
#elif defined(SATDUMP_CRASHDUMP_WINDOWS)
#include <windows.h>
#include <dbghelp.h>
#else
#include <backtrace.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef SATDUMP_CRASHDUMP_WINDOWS
#include <dbghelp.h>
#endif
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <string>
#ifdef SATDUMP_CRASHDUMP_WINDOWS
#include <vector>
#endif
#endif

namespace satdump
{
#ifdef SATDUMP_CRASHDUMP
#if !defined(SATDUMP_CRASHDUMP_CPPTRACE) && !defined(SATDUMP_CRASHDUMP_WINDOWS)
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

#ifdef SATDUMP_CRASHDUMP_WINDOWS
    static BOOL CALLBACK read_process_memory(HANDLE, DWORD64 base, PVOID buffer, DWORD size, LPDWORD read)
    {
        SIZE_T bytes_read = 0;
        BOOL ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(base), buffer, size, &bytes_read);
        if (read)
            *read = static_cast<DWORD>(bytes_read);
        return ok;
    }

    static void append_windows_frame(std::string &report, HANDLE process, DWORD64 address)
    {
        DWORD64 displacement = 0;
        std::vector<char> symbol_buffer(sizeof(SYMBOL_INFO) + MAX_SYM_NAME);
        SYMBOL_INFO *symbol = reinterpret_cast<SYMBOL_INFO *>(symbol_buffer.data());
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        if (SymFromAddr(process, address, &displacement, symbol))
        {
            report += satdump::svformat("  at %s + 0x%llx", symbol->Name, (unsigned long long)displacement);

            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD line_displacement = 0;
            if (SymGetLineFromAddr64(process, address, &line_displacement, &line))
                report += satdump::svformat(" (%s:%lu)", line.FileName, line.LineNumber);

            report += "\n";
        }
        else
        {
            report += satdump::svformat("  at 0x%llx\n", (unsigned long long)address);
        }
    }

    static void append_windows_backtrace(std::string &report, void *exception_info_ptr)
    {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        if (!SymInitialize(process, nullptr, TRUE))
        {
            report += "  [SymInitialize failed]\n";
            return;
        }

        bool walked = false;
        EXCEPTION_POINTERS *exception_info = reinterpret_cast<EXCEPTION_POINTERS *>(exception_info_ptr);
        if (exception_info && exception_info->ContextRecord)
        {
            STACKFRAME64 frame = {};
            DWORD machine = IMAGE_FILE_MACHINE_AMD64;

#if defined(_M_ARM64) || defined(__aarch64__)
            machine = IMAGE_FILE_MACHINE_ARM64;
            frame.AddrPC.Offset = exception_info->ContextRecord->Pc;
            frame.AddrFrame.Offset = exception_info->ContextRecord->Fp;
            frame.AddrStack.Offset = exception_info->ContextRecord->Sp;
#elif defined(_M_IX86) || defined(__i386__)
            machine = IMAGE_FILE_MACHINE_I386;
            frame.AddrPC.Offset = exception_info->ContextRecord->Eip;
            frame.AddrFrame.Offset = exception_info->ContextRecord->Ebp;
            frame.AddrStack.Offset = exception_info->ContextRecord->Esp;
#else
            frame.AddrPC.Offset = exception_info->ContextRecord->Rip;
            frame.AddrFrame.Offset = exception_info->ContextRecord->Rbp;
            frame.AddrStack.Offset = exception_info->ContextRecord->Rsp;
#endif
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Mode = AddrModeFlat;

            for (int i = 0; i < 64; i++)
            {
                if (!StackWalk64(machine, process, GetCurrentThread(), &frame, exception_info->ContextRecord, read_process_memory, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                    break;

                if (frame.AddrPC.Offset == 0)
                    break;

                append_windows_frame(report, process, frame.AddrPC.Offset);
                walked = true;
            }
        }

        if (!walked)
        {
            void *frames[64];
            USHORT frame_count = CaptureStackBackTrace(0, 64, frames, nullptr);
            for (USHORT i = 0; i < frame_count; i++)
                append_windows_frame(report, process, reinterpret_cast<DWORD64>(frames[i]));
        }

        SymCleanup(process);
    }
#endif

    static void crash_handler_impl(int sig, void *exception_info)
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
#elif defined(SATDUMP_CRASHDUMP_WINDOWS)
        append_windows_backtrace(report, exception_info);
#else
        (void)exception_info;
        backtrace_full(bt_state, 0, bt_callback, bt_error, &report);
#endif

        fprintf(stderr, "\n%s\n", report.c_str());

        std::ofstream("gobdump_crash_" + std::to_string(time(NULL)) + ".txt").write(report.c_str(), report.size());

        signal(sig, SIG_DFL);
        exit(1);
    }

    static void crash_handler(int sig)
    {
        crash_handler_impl(sig, nullptr);
    }

#ifdef _WIN32
    static LONG WINAPI crash_filter(EXCEPTION_POINTERS *exception_info)
    {
        int sig = SIGSEGV;

        if (exception_info && exception_info->ExceptionRecord)
        {
            switch (exception_info->ExceptionRecord->ExceptionCode)
            {
            case EXCEPTION_ILLEGAL_INSTRUCTION:
                sig = SIGILL;
                break;
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
            case EXCEPTION_INT_OVERFLOW:
            case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            case EXCEPTION_FLT_OVERFLOW:
                sig = SIGFPE;
                break;
            default:
                sig = SIGSEGV;
                break;
            }
        }

        crash_handler_impl(sig, exception_info);
        return EXCEPTION_EXECUTE_HANDLER;
    }
#endif
#endif

    void initCrashDump()
    {
#ifdef SATDUMP_CRASHDUMP

#ifndef SATDUMP_CRASHDUMP_CPPTRACE
#ifndef SATDUMP_CRASHDUMP_WINDOWS
        bt_state = backtrace_create_state(NULL, 1, bt_error, nullptr);
#endif
#endif

#ifdef _WIN32
        SetUnhandledExceptionFilter(crash_filter);
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
