cmake_minimum_required(VERSION 3.15)
project(webkbm C)

set(CMAKE_C_STANDARD 11)

# ---- Embed webapp/ into webapp_data.h at build time ----
set(WEBAPP_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/webapp")
set(WEBAPP_DATA  "${CMAKE_CURRENT_BINARY_DIR}/webapp_data.h")
set(EMBED_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake")

file(GLOB WEBAPP_SRCS "${WEBAPP_DIR}/*")

add_custom_command(
    OUTPUT  "${WEBAPP_DATA}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT_DIR=${WEBAPP_DIR}
            -DOUTPUT_FILE=${WEBAPP_DATA}
            -P ${EMBED_SCRIPT}
    DEPENDS ${WEBAPP_SRCS} ${EMBED_SCRIPT}
    COMMENT "Embedding webapp/ -> webapp_data.h"
    VERBATIM
)

add_executable(webkbm
    webkbm.c
    qrcodegen.c
    "${WEBAPP_DATA}"
)
target_include_directories(webkbm PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

if(WIN32)
    target_link_libraries(webkbm
        ws2_32 iphlpapi user32 shell32 gdi32 bcrypt
    )
    # Build as a GUI subsystem app so no console window appears.
    target_link_options(webkbm PRIVATE
        /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup
    )
endif()
