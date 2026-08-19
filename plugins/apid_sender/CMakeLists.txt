cmake_minimum_required(VERSION 3.12)
project(apid_sender)

file(GLOB_RECURSE apid_sender_CPPS *.cpp)
add_library(apid_sender SHARED ${apid_sender_CPPS})
# Only the GUI (drawUI) needs gobdump_interface (ImGui). In a CLI-only build
# (BUILD_GUI=OFF) that library isn't built, so link it conditionally and expose
# BUILD_GUI so drawUI() can be compiled out.
target_link_libraries(apid_sender PUBLIC gobdump_core)
if(BUILD_GUI)
    target_link_libraries(apid_sender PUBLIC gobdump_interface)
    target_compile_definitions(apid_sender PRIVATE BUILD_GUI=1)
endif()
target_include_directories(apid_sender PUBLIC
    ${CMAKE_SOURCE_DIR}/src-core
    ${CMAKE_SOURCE_DIR}/src-interface
)

install(TARGETS apid_sender DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins)
