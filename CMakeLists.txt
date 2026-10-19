cmake_minimum_required(VERSION 3.21)
project(clipboard VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick)
qt_standard_project_setup(REQUIRES 6.5)

# ---------------------------------------------------------------------------
# Wayland data-control protocols
#
# The client side is generated with the plain wayland-scanner instead of
# QtWaylandScannerTools: the module only needs libwayland-client, so it does
# not depend on QtWaylandClient (or its private headers) at all.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)

set(CLIPBOARD_PROTOCOLS
    protocols/ext-data-control-v1.xml
    protocols/wlr-data-control-unstable-v1.xml
)

set(PROTOCOL_SOURCES)
set(PROTOCOL_HEADERS)
foreach(protocol ${CLIPBOARD_PROTOCOLS})
    get_filename_component(base "${protocol}" NAME_WE)
    set(protocol_file "${CMAKE_CURRENT_SOURCE_DIR}/${protocol}")
    set(header "${CMAKE_CURRENT_BINARY_DIR}/protocols/wayland-${base}-client-protocol.h")
    set(source "${CMAKE_CURRENT_BINARY_DIR}/protocols/wayland-${base}-protocol.c")

    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${WAYLAND_SCANNER}" client-header "${protocol_file}" "${header}"
        DEPENDS "${protocol_file}"
        COMMENT "Generating Wayland client header for ${base}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${source}"
        COMMAND "${WAYLAND_SCANNER}" public-code "${protocol_file}" "${source}"
        DEPENDS "${protocol_file}"
        COMMENT "Generating Wayland protocol code for ${base}"
        VERBATIM)

    list(APPEND PROTOCOL_SOURCES "${source}")
    list(APPEND PROTOCOL_HEADERS "${header}")
endforeach()

add_custom_target(clipboard_protocols DEPENDS ${PROTOCOL_HEADERS} ${PROTOCOL_SOURCES})

# ---------------------------------------------------------------------------
# QML module
# ---------------------------------------------------------------------------

# Same layout as the Qt 6 packages of the distributions: <libdir>/qt6/qml.
# Override the cache entry for a different layout.
set(CLIPBOARD_QML_DIR "${CMAKE_INSTALL_LIBDIR}/qt6/qml/io/yigekuyou/clipboard"
    CACHE STRING "Install directory for the QML module (relative to the prefix or absolute)")

qt_add_library(clipboard SHARED)
add_dependencies(clipboard clipboard_protocols)

qt_add_qml_module(clipboard
    URI io.yigekuyou.clipboard
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/io/yigekuyou/clipboard
    SOURCES
        src/clipboardmanager.cpp src/clipboardmanager.h
        src/waylandclipboard.cpp src/waylandclipboard.h
)

# qt_add_qml_module() drops .c files from SOURCES, so add the generated
# protocol implementation explicitly.
target_sources(clipboard PRIVATE ${PROTOCOL_SOURCES})
target_include_directories(clipboard PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/protocols")
target_link_libraries(clipboard PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick PkgConfig::WAYLAND_CLIENT)

install(TARGETS clipboard clipboardplugin
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CLIPBOARD_QML_DIR})

install(FILES ${CMAKE_BINARY_DIR}/io/yigekuyou/clipboard/qmldir
              ${CMAKE_BINARY_DIR}/io/yigekuyou/clipboard/clipboard.qmltypes
        DESTINATION ${CLIPBOARD_QML_DIR})
