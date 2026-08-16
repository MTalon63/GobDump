if(NOT DEFINED DEST_DIR OR NOT DEFINED MINGW_BIN)
    message(FATAL_ERROR "DEST_DIR and MINGW_BIN must be set")
endif()

if(NOT DEFINED OBJDUMP)
    set(OBJDUMP objdump)
endif()

file(GLOB roots
    "${DEST_DIR}/*.exe"
    "${DEST_DIR}/*.dll"
    "${DEST_DIR}/plugins/*.dll"
)

set(resolved "")
set(pending ${roots})

while(pending)
    list(POP_FRONT pending current)

    execute_process(
        COMMAND ${OBJDUMP} -p "${current}"
        OUTPUT_VARIABLE dump
        ERROR_QUIET
    )

    string(REGEX MATCHALL "DLL Name: [^\n\r]+" matches "${dump}")

    foreach(match ${matches})
        string(REPLACE "DLL Name: " "" name "${match}")
        string(STRIP "${name}" name)

        if(name IN_LIST resolved)
            continue()
        endif()

        # Only DLLs shipped by MSYS2; system libraries stay where they are.
        if(NOT EXISTS "${MINGW_BIN}/${name}")
            continue()
        endif()

        list(APPEND resolved "${name}")
        list(APPEND pending "${MINGW_BIN}/${name}")
    endforeach()
endwhile()

set(copied 0)
foreach(name ${resolved})
    if(NOT EXISTS "${DEST_DIR}/${name}")
        file(COPY "${MINGW_BIN}/${name}" DESTINATION "${DEST_DIR}")
        math(EXPR copied "${copied} + 1")
    endif()
endforeach()

list(LENGTH resolved total)
message(STATUS "Runtime DLLs: ${total} required, ${copied} newly copied to ${DEST_DIR}")

get_filename_component(_prefix "${MINGW_BIN}" DIRECTORY)
if("libuhd.dll" IN_LIST resolved AND EXISTS "${_prefix}/share/uhd")
    if(NOT EXISTS "${DEST_DIR}/share/uhd")
        file(COPY "${_prefix}/share/uhd" DESTINATION "${DEST_DIR}/share")
        message(STATUS "Copied UHD data to ${DEST_DIR}/share/uhd")
    endif()
endif()
