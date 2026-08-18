#pragma once

// _WIN32_WINNT changes SDK struct sizes; a TU disagreeing with the rest is an ODR violation. Set it in CMakeLists.txt only.
#if defined(_WIN32) && defined(_WIN32_WINNT) && (_WIN32_WINNT != 0x0601)
#error "_WIN32_WINNT redefined away from the 0x0601 baseline. Do not #define it in a source file."
#endif

#ifdef _MSC_VER
#ifdef SATDUMP_DLL_EXPORT
#define SATDUMP_DLL __declspec(dllexport)
#else
#define SATDUMP_DLL __declspec(dllimport)
#endif
#else
#define SATDUMP_DLL
#endif

#ifdef _MSC_VER
#ifdef SATDUMP_DLL_EXPORT2
#define SATDUMP_DLL2 __declspec(dllexport)
#else
#define SATDUMP_DLL2 __declspec(dllimport)
#endif
#else
#define SATDUMP_DLL2
#endif