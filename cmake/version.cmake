# GobDump version string computation.
#
# This file is used in two places, so that the logic cannot drift apart:
#   * included by the top-level CMakeLists.txt, providing GOBDUMP_VERSION /
#     GOBDUMP_VERSION_TAG (used for CPack and error messages) and the list of git files that
#     change when the version can change (GOBDUMP_VERSION_DEPENDS);
#   * run in script mode (`cmake -P`) by a custom command in src-core/CMakeLists.txt, which
#     (re)generates src-core's version translation unit at BUILD time.
#
# Why the version TU is generated at build time: configuring happens once per build directory, so a
# configure-time configure_file() froze the commit into the binary. Building and installing that
# same build directory after further commits (or on another branch, without a reconfigure) produced
# a binary whose banner and --help still named the OLD commit - which mislabels installs handed out
# for testing and CI artifacts, exactly when it matters most. Regenerating at build time makes the
# banner always name the commit actually being built. configure_file() only rewrites its output
# when the contents differ, so this does not dirty the build or force needless relinks.
#
# Inputs: GOBDUMP_SOURCE_DIR, GOBDUMP_PROJECT_VERSION, GOBDUMP_VERSION_TYPE, GOBDUMP_BUILD_TYPE.
# In script mode also: GOBDUMP_VERSION_TEMPLATE, GOBDUMP_VERSION_OUTPUT, RESOURCES_PATH,
# LIBRARIES_PATH (the template is shared with the resources/libraries paths).

if(NOT DEFINED GOBDUMP_SOURCE_DIR)
    get_filename_component(GOBDUMP_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
if(NOT DEFINED GOBDUMP_BUILD_TYPE)
    set(GOBDUMP_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
endif()

# Check if the current commit has a tag
execute_process(
    COMMAND git describe --tags --exact-match HEAD
    WORKING_DIRECTORY ${GOBDUMP_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HAS_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# Get the current branch
execute_process(
    COMMAND git rev-parse --abbrev-ref HEAD
    WORKING_DIRECTORY ${GOBDUMP_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HAS_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Get the latest abbreviated commit hash of current branch
execute_process(
    COMMAND git log -1 --format=%h
    WORKING_DIRECTORY ${GOBDUMP_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# GitHub Actions tag builds can have a shallow checkout without the tag ref.
if(GIT_HAS_TAG STREQUAL "" AND "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag")
    set(GIT_HAS_TAG "$ENV{GITHUB_REF_NAME}")
endif()

# Set version
if(GIT_HAS_TAG STREQUAL "undefined" OR GIT_HAS_TAG STREQUAL "nightly" OR GIT_HAS_TAG STREQUAL "")
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        message("-- Commit has no tag/nightly tag (${GIT_HAS_TAG}). Assume dev build")
    endif()
    if(NOT GOBDUMP_VERSION_TYPE STREQUAL "")
        set(GOBDUMP_VERSION "${GOBDUMP_PROJECT_VERSION}-${GOBDUMP_VERSION_TYPE}-${GIT_HASH}")
    else()
        set(GOBDUMP_VERSION "${GOBDUMP_PROJECT_VERSION}-${GIT_HASH}")
    endif()
else()
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        message("-- Commit has tag (${GIT_HAS_TAG}). Assume release build")
    endif()
    # Use the actual tag for release builds instead of the development PROJECT_VERSION.
    set(GOBDUMP_VERSION "${GIT_HAS_TAG}")
    string(REGEX REPLACE "^tags/" "" GOBDUMP_VERSION "${GOBDUMP_VERSION}")
    string(REGEX REPLACE "^v" "" GOBDUMP_VERSION "${GOBDUMP_VERSION}")
endif()

# Set branch
if(GIT_HAS_BRANCH STREQUAL "master")
    set(GOBDUMP_VERSION_TAG "")
else()
    set(GOBDUMP_VERSION_TAG "${GIT_HAS_BRANCH}/${GOBDUMP_BUILD_TYPE}")
endif()

# The git files that change when the version string can change: a commit rewrites the branch ref, a
# checkout rewrites HEAD. The build-time regeneration depends on these, so it re-runs exactly when
# it has to. On a source export (no .git) they simply do not exist and the dependency is dropped.
# A detached HEAD has no refs/heads/<name> entry, but .git/HEAD itself changes on checkout.
set(GOBDUMP_VERSION_DEPENDS "")
foreach(_candidate "HEAD" "refs/heads/${GIT_HAS_BRANCH}" "packed-refs")
    if(EXISTS "${GOBDUMP_SOURCE_DIR}/.git/${_candidate}")
        list(APPEND GOBDUMP_VERSION_DEPENDS "${GOBDUMP_SOURCE_DIR}/.git/${_candidate}")
    endif()
endforeach()

if(CMAKE_SCRIPT_MODE_FILE AND DEFINED GOBDUMP_VERSION_OUTPUT)
    configure_file("${GOBDUMP_VERSION_TEMPLATE}" "${GOBDUMP_VERSION_OUTPUT}" @ONLY)
endif()
