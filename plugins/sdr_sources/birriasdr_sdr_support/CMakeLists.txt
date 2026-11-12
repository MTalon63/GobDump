cmake_minimum_required(VERSION 3.12)
project(birriasdr_sdr_support)

# BirriaSDR ships no package config, so BIRRIASDR_ROOT may name its install prefix or build tree.
set(BIRRIASDR_ROOT "" CACHE PATH "BirriaSDR install prefix or build tree")

set(_birriasdr_hints
    "${BIRRIASDR_ROOT}/include"
    "${BIRRIASDR_ROOT}/build"
    "${BIRRIASDR_ROOT}/lib"
    "${BIRRIASDR_ROOT}/bin")

find_path(BIRRIASDR_INCLUDE_DIR NAMES birriasdr.h HINTS ${_birriasdr_hints})
find_library(BIRRIASDR_LIBRARY NAMES birriasdr libbirriasdr.dll.a HINTS ${_birriasdr_hints})

if(BIRRIASDR_INCLUDE_DIR AND BIRRIASDR_LIBRARY)
    message("Building with BirriaSDR support")
    file(GLOB_RECURSE birriasdr_sdr_support_CPPS *.cpp)
    add_library(birriasdr_sdr_support SHARED ${birriasdr_sdr_support_CPPS})
    target_include_directories(birriasdr_sdr_support PUBLIC ${BIRRIASDR_INCLUDE_DIR})
    target_link_libraries(birriasdr_sdr_support PUBLIC gobdump_core ${BIRRIASDR_LIBRARY})

    install(TARGETS birriasdr_sdr_support DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins)

    # Windows resolves a plugin's imports against the executable's directory.
    if(WIN32)
        get_filename_component(_birriasdr_libdir "${BIRRIASDR_LIBRARY}" DIRECTORY)
        get_filename_component(_birriasdr_prefix "${_birriasdr_libdir}" DIRECTORY)
        find_file(BIRRIASDR_RUNTIME_DLL NAMES birriasdr.dll libbirriasdr.dll
            HINTS "${_birriasdr_libdir}" "${_birriasdr_prefix}/bin" "${BIRRIASDR_ROOT}/bin" "${BIRRIASDR_ROOT}/build")
        if(BIRRIASDR_RUNTIME_DLL)
            add_custom_command(TARGET birriasdr_sdr_support POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BIRRIASDR_RUNTIME_DLL}" "${CMAKE_BINARY_DIR}")
        else()
            message(WARNING "BirriaSDR runtime library not found, the plugin will not load")
        endif()
    endif()
else()
    message("BirriaSDR not found! Not building.")
endif()
