cmake_minimum_required(VERSION 3.12)
project(dvb_support)

file(GLOB_RECURSE dvb_support_CPPS *.cpp)
add_library(dvb_support SHARED ${dvb_support_CPPS})
target_link_libraries(dvb_support PUBLIC gobdump_core)
target_include_directories(dvb_support PUBLIC src .)

# SSE41
if(NOT ANDROID)
    include(FindSSE41)
endif()

if(ANDROID)
    set(SSE41_FOUND 0)

    if(ANDROID_ABI STREQUAL "x86")
        set(SSE41_FOUND 1)
    endif()

    if(ANDROID_ABI STREQUAL "x86_64")
        set(SSE41_FOUND 1)
    endif()
endif()

if(SSE41_FOUND)
    message("Found SSE features, enabling building SSE code for DVB")

    # MINGW matched neither branch, so the SSE4.1 path compiled without SSE4.1 enabled
    if(UNIX OR ANDROID OR MINGW)
        set(FINAL_FLAGS_SSE41 "-msse4.1")
    elseif(MSVC)
        set(FINAL_FLAGS_SSE41 "${SSE41_C_FLAGS}")
    endif()

    # Enable on all files but main.cpp
    foreach(_file ${dvb_support_CPPS})
        if(NOT(${_file} MATCHES "main.cpp$"))
            set_source_files_properties(${_file} PROPERTIES COMPILE_FLAGS "${FINAL_FLAGS_SSE41}")
        endif()
    endforeach()

    target_compile_definitions(dvb_support PRIVATE DVB_HAS_SSE41)
else()
    message("SSE Features not found! This is only an error on x86 CPUs that should support SSE4.1")
endif()

# NEON
if(NOT ANDROID)
    include(FindNEON)
endif()

if(ANDROID)
    set(NEON_FOUND 0)

    if(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(NEON_FOUND 1)
    endif()

    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(NEON_FOUND 1)
    endif()
endif()

if(NEON_FOUND)
    message("Found NEON features, enabling building NEON code for DVB")

    if(UNIX OR ANDROID OR MINGW OR MSVC OR BUILD_MSVC)
        set(FINAL_FLAGS_NEON "${NEON_C_FLAGS}")
    endif()

    # Enable on all files but main.cpp
    foreach(_file ${dvb_support_CPPS})
        if(NOT(${_file} MATCHES "main.cpp$"))
            set_source_files_properties(${_file} PROPERTIES COMPILE_FLAGS "${FINAL_FLAGS_NEON}")
        endif()
    endforeach()

    target_compile_definitions(dvb_support PRIVATE DVB_HAS_NEON)
else()
    message("NEON Features not found! This is only an error on ARM CPUs that should support NEON")
endif()

# AVX2 selects the 32-lane DVB-S2 LDPC kernel. It must be target-wide: the
# simd_type it selects is visible from bbframe_ldpc.h, so a per-source define
# would give different TUs different class layouts.
option(DVB_LDPC_AVX2 "Use the 32-lane AVX2 DVB-S2 LDPC kernel" ON)
if(DVB_LDPC_AVX2 AND NOT ANDROID)
    include(FindAVX)
endif()

if(DVB_LDPC_AVX2 AND CXX_AVX2_FOUND)
    message("Found AVX2 features, enabling 32-lane DVB-S2 LDPC")
    separate_arguments(DVB_AVX2_FLAGS NATIVE_COMMAND "${CXX_AVX2_FLAGS}")
    target_compile_options(dvb_support PUBLIC ${DVB_AVX2_FLAGS})
    target_compile_definitions(dvb_support PUBLIC DVB_HAS_AVX2)
else()
    message("AVX2 not used for DVB-S2 LDPC; keeping 16-lane path")
endif()

install(TARGETS dvb_support DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins)