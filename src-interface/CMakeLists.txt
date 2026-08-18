cmake_minimum_required(VERSION 3.12)

file(GLOB_RECURSE INTERFACE_CPPS *.cpp)

add_library(gobdump_interface SHARED ${INTERFACE_CPPS})
target_include_directories(gobdump_interface PUBLIC . ../src-core)
#set_target_properties(gobdump_interface PROPERTIES COMPILE_DEFINITIONS BUILDER_STATIC_DEFINE)

if(BUILD_ZIQ)
    target_compile_definitions(gobdump_interface PUBLIC BUILD_ZIQ="1")
endif()

if(BUILD_ZIQ2)
    target_compile_definitions(gobdump_interface PUBLIC BUILD_ZIQ2="1")
endif()

# Link against the core
target_link_libraries(gobdump_interface PUBLIC gobdump_core)

# Per-artifact destinations: the plain form sent the .dll to lib/, where Windows does not look
install(TARGETS gobdump_interface
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})