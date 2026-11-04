cmake_minimum_required(VERSION 3.12)
project(plutosdr_sdr_support)


# On macOS, do not accidentally pick up stale framework installs from
# /Library/Frameworks.  The project build produces the dylibs in deps/lib,
# and those are the libraries the bundle can actually ship.
if(APPLE)
    # find_library() otherwise reuses old cache entries even after its search
    # paths change, so clear the two dependency results before resolving them.
    unset(IIO_LIBRARY CACHE)
    unset(AD9361_LIBRARY CACHE)
    find_library(IIO_LIBRARY
        NAMES iio
        PATHS "${CMAKE_SOURCE_DIR}/deps/lib"
        NO_DEFAULT_PATH
    )
    find_library(AD9361_LIBRARY
        NAMES ad9361
        PATHS "${CMAKE_SOURCE_DIR}/deps/lib"
        NO_DEFAULT_PATH
    )
else()
    find_library(IIO_LIBRARY iio)
    find_library(AD9361_LIBRARY ad9361)
endif()

if((IIO_LIBRARY AND AD9361_LIBRARY) OR ANDROID)
    message("Building with PlutoSDR support")
    file(GLOB_RECURSE plutosdr_sdr_support_CPPS *.cpp)
    add_library(plutosdr_sdr_support SHARED ${plutosdr_sdr_support_CPPS})
    target_include_directories(plutosdr_sdr_support PUBLIC src)

    if(MSVC)
        target_link_libraries(plutosdr_sdr_support PUBLIC gobdump_core libiio.dll libad9361.dll)
        install(TARGETS plutosdr_sdr_support DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins RUNTIME_DEPENDENCY_SET gobdump_deps)
    elseif(ANDROID)
        target_link_libraries(plutosdr_sdr_support PUBLIC gobdump_core iio ad9361iio)
        target_include_directories(plutosdr_sdr_support PUBLIC ../../../android/deps/libiio ../../../android/deps/libad9361-iio)
        install(TARGETS plutosdr_sdr_support DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins)
    else()
        target_link_libraries(plutosdr_sdr_support PUBLIC gobdump_core ${IIO_LIBRARY} ${AD9361_LIBRARY})
        install(TARGETS plutosdr_sdr_support DESTINATION ${CMAKE_INSTALL_LIBDIR}/gobdump/plugins)
    endif()
else()
    message("IIO or AD9361 Library could not be found! Not building.")
endif()