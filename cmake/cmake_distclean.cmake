set(BUILD_DIR "@CMAKE_BINARY_DIR@")

if(NOT EXISTS "${BUILD_DIR}")
    message(STATUS "${BUILD_DIR} does not exist, nothing to do.")
    return()
endif()

# Guard against a mis-set path taking the source tree with it.
if(EXISTS "${BUILD_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Refusing to wipe ${BUILD_DIR}: it looks like a source directory.")
endif()

file(GLOB entries LIST_DIRECTORIES true "${BUILD_DIR}/*" "${BUILD_DIR}/.*")

foreach(entry ${entries})
    get_filename_component(name "${entry}" NAME)
    if(name STREQUAL "." OR name STREQUAL "..")
        continue()
    endif()
    file(REMOVE_RECURSE "${entry}")
endforeach()

message(STATUS "Emptied ${BUILD_DIR}")
