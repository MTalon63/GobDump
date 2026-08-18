cmake_minimum_required(VERSION 3.12)
project(xrit_support)

file(GLOB_RECURSE xrit_support_CPPS *.cpp)

# EUMETSAT wavelet decompressor, upstream
file(GLOB_RECURSE xrit_support_VENDORED DecompWT/*.cpp DecompWT/*.c)
gobdump_mark_vendored_sources(${xrit_support_VENDORED})

add_library(xrit_support SHARED ${xrit_support_CPPS})
target_link_libraries(xrit_support PUBLIC gobdump_core)
target_include_directories(xrit_support PUBLIC .)

install(TARGETS xrit_support
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins)
