cmake_minimum_required(VERSION 3.12)
project(inmarsat_support)

find_package(ZLIB REQUIRED)

file(GLOB_RECURSE inmarsat_support_CPPS *.cpp aero/libacars/*.c aero/mbelib/*.c)

# Upstream libacars/mbelib
file(GLOB_RECURSE inmarsat_support_VENDORED aero/libacars/*.c aero/libacars/*.cpp aero/mbelib/*.c aero/mbelib/*.cpp)
gobdump_mark_vendored_sources(${inmarsat_support_VENDORED})

add_library(inmarsat_support SHARED ${inmarsat_support_CPPS})
target_link_libraries(inmarsat_support PUBLIC gobdump_core ZLIB::ZLIB)
target_include_directories(inmarsat_support PUBLIC src .)

# WIN32, not MSVC: MinGW linked this only by inheritance from gobdump_core
if(WIN32)
    target_link_libraries(inmarsat_support PRIVATE ws2_32)
endif()

install(TARGETS inmarsat_support DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins)