cmake_minimum_required(VERSION 3.12)
project(usrp_sdr_support)


find_library(UHD_LIBRARY uhd)

if(UHD_LIBRARY) # OR ANDROID)
    message("Building with USRP support")
    file(GLOB_RECURSE usrp_sdr_support_CPPS *.cpp)
    add_library(usrp_sdr_support SHARED ${usrp_sdr_support_CPPS})
    target_include_directories(usrp_sdr_support PUBLIC src)

    # find_library() locates UHD, but does not propagate its headers.  The
    # bundled build lives in deps/, while package-manager installs (such as
    # radioconda on macOS) keep headers beside the library prefix.
    get_filename_component(UHD_LIBRARY_DIR "${UHD_LIBRARY}" DIRECTORY)
    find_path(UHD_INCLUDE_DIR NAMES uhd.h
        PATHS
            "${CMAKE_SOURCE_DIR}/deps/include"
            "${UHD_LIBRARY_DIR}/../include"
    )
    if(UHD_INCLUDE_DIR)
        target_include_directories(usrp_sdr_support PRIVATE "${UHD_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR "UHD library was found at ${UHD_LIBRARY}, but uhd.h was not found")
    endif()

    if(MSVC)
        target_link_libraries(usrp_sdr_support PUBLIC gobdump_core uhd.dll)
        install(TARGETS usrp_sdr_support DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins RUNTIME_DEPENDENCY_SET gobdump_deps)
    # elseif(ANDROID)
    #   target_link_libraries(usrp_sdr_support PUBLIC gobdump_core airspy usb)
    #   target_include_directories(usrp_sdr_support PUBLIC ../../../android/deps/libairspy)
    #   install(TARGETS usrp_sdr_support DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins)
    else()
        target_link_libraries(usrp_sdr_support PUBLIC gobdump_core ${UHD_LIBRARY})
        install(TARGETS usrp_sdr_support DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins)

        if(UNIX AND NOT APPLE AND NOT ANDROID)
            get_filename_component(UHD_LIBRARY_REAL "${UHD_LIBRARY}" REALPATH)
            string(FIND "${UHD_LIBRARY_REAL}" "${CMAKE_SOURCE_DIR}/deps/" UHD_DEPS_PREFIX)
            if(UHD_DEPS_PREFIX EQUAL 0)
                install(FILES "${UHD_LIBRARY_REAL}" DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump)
            endif()
        endif()
    endif()
else()
    message("UHD Library could not be found! Not building.")
endif()