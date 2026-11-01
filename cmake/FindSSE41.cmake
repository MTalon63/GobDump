#.rst:
# FindSSE41
# ---------
#
# Finds SSE41 support
#
# Like FindAVX, "support" means the compiler accepts the flags AND this CPU can run the result:
# each flag candidate that compiles is also executed once.
#
# This module can be used to detect SSE41 support in a C compiler.  If
# the compiler supports SSE41, the flags required to compile with
# SSE41 support are returned in variables for the different languages.
# The variables may be empty if the compiler does not need a special
# flag to support SSE41.
#
# The following variables are set:
#
# ::
#
#    SSE41_C_FLAGS - flags to add to the C compiler for SSE41 support
#    SSE41_FOUND - true if SSE41 is detected and this CPU can run it
#
# Cross-compiling probes nothing; opt in with -DSIMD_TRUST_CPU_SSE41=ON.
#
#=============================================================================

set(_SSE41_REQUIRED_VARS)
set(CMAKE_REQUIRED_QUIET_SAVE ${CMAKE_REQUIRED_QUIET})
set(CMAKE_REQUIRED_QUIET ${SSE41_FIND_QUIETLY})

OPTION(SIMD_TRUST_CPU_SSE41 "Enable SSE4.1 without probing the CPU (cross-compiling, target is known)" OFF)

# sample SSE41 source code to test
set(SSE41_C_TEST_SOURCE
"
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <smmintrin.h>
#endif
int foo() {
    __m128i vOne = _mm_set1_epi8(1);
    __m128i result =  _mm_max_epi8(vOne,vOne);
    return _mm_extract_epi8(result, 0);
}
int main(void) { return foo(); }
")

# Must exit 0, and drive the instruction from argc so it is not optimised out.
set(SSE41_C_RUN_SOURCE
"
#include <immintrin.h>
#include <stdint.h>

volatile int sse41_sink;

int main(int argc, char **argv) {
    __m128i a = _mm_set1_epi8((char)argc);
    __m128i b = _mm_set1_epi8((char)(argc + 1));
    __m128i m = _mm_max_epi8(a, b);
    __m128i s = _mm_blendv_epi8(a, b, m);
    sse41_sink = _mm_extract_epi32(s, 0);
    return 0;
}
")

# MSVC, not WIN32: WIN32 is true for MinGW too, so GCC was only ever offered the MSVC flag and
# always failed the probe - silently dropping the simd_sse41 plugin from every MinGW build.
if(MSVC)
  set(SSE41_C_FLAG_CANDIDATES
    #Empty, if compiler automatically accepts SSE41
    " "
    "/arch:SSE2")
else()
  set(SSE41_C_FLAG_CANDIDATES
    #Empty, if compiler automatically accepts SSE41
    " "
    #GNU, clang
    "-msse4.1"
    #GNU 4.4.7 ?
    "-msse4")
endif()

include(CheckCSourceCompiles)
include(CheckCSourceRuns)

set(SSE41_C_FLAGS "")
set(_SSE41_FLAG_I 0)
foreach(FLAG IN LISTS SSE41_C_FLAG_CANDIDATES)
  math(EXPR _SSE41_FLAG_I "${_SSE41_FLAG_I}+1")
  set(SAFE_CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_FLAGS "${FLAG}")
  if(NOT CMAKE_REQUIRED_QUIET)
    message(STATUS "Try SSE41 C flag = [${FLAG}]")
  endif()

  check_c_source_compiles("${SSE41_C_TEST_SOURCE}" HAVE_SSE41_COMPILE_${_SSE41_FLAG_I})
  set(HAVE_SSE41 "")
  if(HAVE_SSE41_COMPILE_${_SSE41_FLAG_I})
    if(CMAKE_CROSSCOMPILING AND NOT SIMD_TRUST_CPU_SSE41)
      message(STATUS "SSE41 : cross-compiling, CPU not probed - disabled (set SIMD_TRUST_CPU_SSE41=ON if the target has it)")
      set(CMAKE_REQUIRED_FLAGS "${SAFE_CMAKE_REQUIRED_FLAGS}")
      break()
    elseif(CMAKE_CROSSCOMPILING)
      set(HAVE_SSE41 1)
    else()
      check_c_source_runs("${SSE41_C_RUN_SOURCE}" HAVE_SSE41_RUN_${_SSE41_FLAG_I})
      if(HAVE_SSE41_RUN_${_SSE41_FLAG_I})
        set(HAVE_SSE41 1)
      else()
        message(STATUS "SSE41 : [${FLAG}] compiles, but this CPU cannot execute it")
      endif()
    endif()
  endif()
  set(CMAKE_REQUIRED_FLAGS "${SAFE_CMAKE_REQUIRED_FLAGS}")

  if(HAVE_SSE41)
    set(SSE41_C_FLAGS_INTERNAL "${FLAG}")
    break()
  endif()
endforeach()

unset(SSE41_C_FLAG_CANDIDATES)
unset(HAVE_SSE41)

# Plain variable: a value cached by the old compile-only probe must not survive.
unset(SSE41_C_FLAGS CACHE)
set(SSE41_C_FLAGS "${SSE41_C_FLAGS_INTERNAL}")

list(APPEND _SSE41_REQUIRED_VARS SSE41_C_FLAGS)

set(CMAKE_REQUIRED_QUIET ${CMAKE_REQUIRED_QUIET_SAVE})

if(_SSE41_REQUIRED_VARS)
  include(FindPackageHandleStandardArgs)

  find_package_handle_standard_args(SSE41
                                    REQUIRED_VARS ${_SSE41_REQUIRED_VARS})

  mark_as_advanced(${_SSE41_REQUIRED_VARS})

  unset(_SSE41_REQUIRED_VARS)
else()
  message(SEND_ERROR "FindSSE41 requires C or CXX language to be enabled")
endif()

# begin tests for SSE4.1 specfic features

set(SSE41_C_TEST_SOURCE_INSERT64
"
#include <stdint.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <smmintrin.h>
#endif
__m128i foo() {
    __m128i vOne = _mm_set1_epi8(1);
    return _mm_insert_epi64(vOne,INT64_MIN,0);
}
int main(void) { foo(); return 0; }
")

if(SSE41_C_FLAGS)
  include(CheckCSourceCompiles)
  set(SAFE_CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_FLAGS "${SSE41_C_FLAGS}")
  check_c_source_compiles("${SSE41_C_TEST_SOURCE_INSERT64}" HAVE_SSE41_MM_INSERT_EPI64)
  set(CMAKE_REQUIRED_FLAGS "${SAFE_CMAKE_REQUIRED_FLAGS}")
endif()

set(SSE41_C_TEST_SOURCE_EXTRACT64
"
#include <stdint.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <smmintrin.h>
#endif
int64_t foo() {
    __m128i vOne = _mm_set1_epi8(1);
    return (int64_t)_mm_extract_epi64(vOne,0);
}
int main(void) { return (int)foo(); }
")

if(SSE41_C_FLAGS)
  include(CheckCSourceCompiles)
  set(SAFE_CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_FLAGS "${SSE41_C_FLAGS}")
  check_c_source_compiles("${SSE41_C_TEST_SOURCE_EXTRACT64}" HAVE_SSE41_MM_EXTRACT_EPI64)
  set(CMAKE_REQUIRED_FLAGS "${SAFE_CMAKE_REQUIRED_FLAGS}")
endif()
