cmake_minimum_required(VERSION 3.12)
project(rtltcp_support)


message("Building with RTL-TCP support")
file(GLOB_RECURSE rtltcp_support_CPPS *.cpp)
add_library(rtltcp_support SHARED ${rtltcp_support_CPPS})
target_link_libraries(rtltcp_support PUBLIC gobdump_core)
target_include_directories(rtltcp_support PUBLIC src)

# WIN32, not MSVC: MinGW linked this only by inheritance from gobdump_core
if(WIN32)
    target_link_libraries(rtltcp_support PRIVATE ws2_32)
endif()

install(TARGETS rtltcp_support DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins)