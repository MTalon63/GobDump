cmake_minimum_required(VERSION 3.12)
project(simd_avx2)

# FindAVX also defines CXX_HAS_AVX_2, which is the AVX probe, not this one.
if(ANDROID)
    if(ANDROID_ABI STREQUAL "x86" OR ANDROID_ABI STREQUAL "x86_64")
        set(SIMD_HAVE_AVX2 1)
        set(FINAL_FLAGS_AVX2 "-mavx2 -mfma")
    else()
        set(SIMD_HAVE_AVX2 0)
    endif()
else()
    include(FindAVX)
    set(SIMD_HAVE_AVX2 ${CXX_AVX2_FOUND})
    set(FINAL_FLAGS_AVX2 "${CXX_AVX2_FLAGS}")
endif()

if(SIMD_HAVE_AVX2)
    message("Found AVX2 features, enabling AVX2 plugin")

    file(GLOB_RECURSE simd_avx2_CPPS *.cpp)
    add_library(simd_avx2 SHARED ${simd_avx2_CPPS})
    target_link_libraries(simd_avx2 PUBLIC gobdump_core)
    target_include_directories(simd_avx2 PUBLIC src)

    # Enable on all files but main.cpp
    foreach(_file ${simd_avx2_CPPS})
        if(NOT(${_file} MATCHES "main.cpp$"))
            set_source_files_properties(${_file} PROPERTIES COMPILE_FLAGS "${FINAL_FLAGS_AVX2}")
        endif()
    endforeach()

    install(TARGETS simd_avx2 DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins)
else()
    message("AVX2 Features not found! This is only an error on x86 CPUs that should support AVX2")
endif()