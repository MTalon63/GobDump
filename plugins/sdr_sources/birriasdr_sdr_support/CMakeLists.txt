cmake_minimum_required(VERSION 3.12)
project(birriasdr_sdr_support)

find_path(BIRRIASDR_INCLUDE_DIR birriasdr.h)
find_library(BIRRIASDR_LIBRARY birriasdr)

if(BIRRIASDR_INCLUDE_DIR AND BIRRIASDR_LIBRARY)
    message("Building with BirriaSDR support")
    file(GLOB_RECURSE birriasdr_sdr_support_CPPS *.cpp)
    add_library(birriasdr_sdr_support SHARED ${birriasdr_sdr_support_CPPS})
    target_include_directories(birriasdr_sdr_support PUBLIC ${BIRRIASDR_INCLUDE_DIR})
    target_link_libraries(birriasdr_sdr_support PUBLIC gobdump_core ${BIRRIASDR_LIBRARY})

    if(MSVC)
        install(TARGETS birriasdr_sdr_support DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins RUNTIME_DEPENDENCY_SET gobdump_deps)
    else()
        install(TARGETS birriasdr_sdr_support DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins)

        # Ship the driver next to the plugin when it came from deps/, as USRP does with the
        # patched UHD. Only the SONAME file is needed at load time.
        if(UNIX AND NOT APPLE AND NOT ANDROID)
            get_filename_component(_birriasdr_libdir "${BIRRIASDR_LIBRARY}" DIRECTORY)
            string(FIND "${_birriasdr_libdir}" "${CMAKE_SOURCE_DIR}/deps/" _birriasdr_deps_prefix)
            if(_birriasdr_deps_prefix EQUAL 0)
                file(GLOB _birriasdr_soname "${_birriasdr_libdir}/libbirriasdr.so.*")
                foreach(_candidate ${_birriasdr_soname})
                    get_filename_component(_name "${_candidate}" NAME)
                    if(_name MATCHES "^libbirriasdr\\.so\\.[0-9]+$")
                        # install(FILES) keeps symlinks, so copy the target under the SONAME.
                        get_filename_component(_real "${_candidate}" REALPATH)
                        install(FILES "${_real}" DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump RENAME "${_name}")
                    endif()
                endforeach()
            endif()
        endif()
    endif()
else()
    message("BirriaSDR Library could not be found! Not building.")
endif()
