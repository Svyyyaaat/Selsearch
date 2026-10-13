cmake_minimum_required(VERSION 3.25)
project(selsearch VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)

option(SELSEARCH_WITH_ONNX "PP-OCRv5 text recognition through ONNX Runtime" ON)
option(SELSEARCH_WITH_TESSERACT "Tesseract text recognition (fallback engine)" ON)
option(SELSEARCH_WITH_WEBENGINE "Embedded results panel (selsearch-web, QtWebEngine)" ON)
option(SELSEARCH_BUILD_TESTS "Build unit tests" ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Svg Network DBus Concurrent ShaderTools WaylandClient)
find_package(LayerShellQt 6.0 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

qt_standard_project_setup(REQUIRES 6.8)

# ---------------------------------------------------------------------------
# Wayland protocol code (client side), used by the in-process screen capture.
set(PROTO_OUT ${CMAKE_CURRENT_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${PROTO_OUT})
set(PROTOCOL_SOURCES)
function(selsearch_wayland_protocol xml name)
    add_custom_command(
        OUTPUT ${PROTO_OUT}/${name}-client.h ${PROTO_OUT}/${name}-protocol.c
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${PROTO_OUT}/${name}-client.h
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${PROTO_OUT}/${name}-protocol.c
        DEPENDS ${xml} VERBATIM)
    set(PROTOCOL_SOURCES ${PROTOCOL_SOURCES} ${PROTO_OUT}/${name}-client.h ${PROTO_OUT}/${name}-protocol.c PARENT_SCOPE)
endfunction()
selsearch_wayland_protocol(${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-screencopy-unstable-v1.xml wlr-screencopy-unstable-v1)
selsearch_wayland_protocol(${WAYLAND_PROTOCOLS_DIR}/staging/ext-image-capture-source/ext-image-capture-source-v1.xml ext-image-capture-source-v1)
selsearch_wayland_protocol(${WAYLAND_PROTOCOLS_DIR}/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml ext-image-copy-capture-v1)
selsearch_wayland_protocol(${WAYLAND_PROTOCOLS_DIR}/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml ext-foreign-toplevel-list-v1)

# ---------------------------------------------------------------------------
# Core library: everything that is not UI. Shared by the daemon and tests.
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS
    src/core/*.cpp src/core/*.h
    src/capture/*.cpp src/capture/*.h
    src/compositor/*.cpp src/compositor/*.h
    src/ocr/OcrCommon.cpp src/ocr/OcrService.cpp src/ocr/ModelStore.cpp src/ocr/*.h
    src/launcher/*.cpp src/launcher/*.h
    src/translate/*.cpp src/translate/*.h
    src/ai/*.cpp src/ai/*.h
    src/search/*.cpp src/search/*.h)

add_library(selsearch_core STATIC ${CORE_SOURCES} ${PROTOCOL_SOURCES})
target_include_directories(selsearch_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROTO_OUT}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tomlplusplus)
target_link_libraries(selsearch_core PUBLIC
    Qt6::Core Qt6::Gui Qt6::Network Qt6::DBus Qt6::Concurrent
    PkgConfig::WAYLAND_CLIENT)
target_compile_definitions(selsearch_core PUBLIC
    SELSEARCH_VERSION="${PROJECT_VERSION}"
    QT_NO_KEYWORDS)

if(SELSEARCH_WITH_ONNX)
    find_package(onnxruntime QUIET)
    if(NOT onnxruntime_FOUND)
        find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h PATH_SUFFIXES onnxruntime onnxruntime/core/session)
        find_library(ONNXRUNTIME_LIBRARY onnxruntime)
        if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
            add_library(onnxruntime::onnxruntime UNKNOWN IMPORTED)
            set_target_properties(onnxruntime::onnxruntime PROPERTIES
                IMPORTED_LOCATION ${ONNXRUNTIME_LIBRARY}
                INTERFACE_INCLUDE_DIRECTORIES ${ONNXRUNTIME_INCLUDE_DIR})
            set(onnxruntime_FOUND TRUE)
        endif()
    endif()
    if(onnxruntime_FOUND)
        target_sources(selsearch_core PRIVATE src/ocr/PaddleOcr.cpp)
        target_link_libraries(selsearch_core PUBLIC onnxruntime::onnxruntime)
        target_compile_definitions(selsearch_core PUBLIC SELSEARCH_HAVE_ONNX=1)
        message(STATUS "Selsearch: PP-OCR (ONNX Runtime) enabled")
    else()
        message(WARNING "ONNX Runtime not found; PP-OCR engine disabled")
    endif()
endif()

if(SELSEARCH_WITH_TESSERACT)
    pkg_check_modules(TESSERACT IMPORTED_TARGET tesseract lept)
    if(TESSERACT_FOUND)
        target_sources(selsearch_core PRIVATE src/ocr/TesseractOcr.cpp)
        target_link_libraries(selsearch_core PUBLIC PkgConfig::TESSERACT)
        target_compile_definitions(selsearch_core PUBLIC SELSEARCH_HAVE_TESSERACT=1)
        message(STATUS "Selsearch: Tesseract enabled")
    else()
        message(WARNING "Tesseract not found; Tesseract engine disabled")
    endif()
endif()

# ---------------------------------------------------------------------------
# selsearchd: the resident daemon with the overlay and launcher UI.
file(GLOB DAEMON_SOURCES CONFIGURE_DEPENDS src/overlay/*.cpp src/overlay/*.h src/app/*.cpp src/app/*.h)
qt_add_executable(selsearchd ${DAEMON_SOURCES})
target_link_libraries(selsearchd PRIVATE selsearch_core
    Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Svg Qt6::WaylandClient LayerShellQt::Interface)

file(GLOB QML_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS qml/*.qml)
file(GLOB ICON_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS data/icons/*.svg)
qt_add_qml_module(selsearchd
    URI Selsearch
    VERSION 1.0
    QML_FILES ${QML_FILES}
    RESOURCES ${ICON_FILES}
    NO_RESOURCE_TARGET_PATH)
qt_add_shaders(selsearchd "selsearch_shaders"
    PREFIX "/"
    FILES shaders/glass.frag)

# ---------------------------------------------------------------------------
# selsearch: tiny client used from compositor keybinds. No Qt, starts fast.
add_executable(selsearch src/cli/main.cpp)
target_compile_definitions(selsearch PRIVATE SELSEARCH_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# selsearch-web: results panel, started on demand so idle RAM stays low.
if(SELSEARCH_WITH_WEBENGINE)
    find_package(Qt6 6.8 QUIET COMPONENTS WebEngineQuick)
    if(Qt6WebEngineQuick_FOUND)
        qt_add_executable(selsearch-web src/web/main.cpp)
        qt_add_qml_module(selsearch-web URI SelsearchWeb VERSION 1.0 QML_FILES qml/web/WebPanel.qml NO_RESOURCE_TARGET_PATH)
        target_link_libraries(selsearch-web PRIVATE Qt6::Quick Qt6::WebEngineQuick LayerShellQt::Interface)
        install(TARGETS selsearch-web RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    else()
        message(WARNING "QtWebEngine not found; results open in the browser")
    endif()
endif()

install(TARGETS selsearchd selsearch RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(packaging/selsearch.service.in ${CMAKE_CURRENT_BINARY_DIR}/selsearch.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/selsearch.service DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user)
install(FILES packaging/selsearch.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

if(SELSEARCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
