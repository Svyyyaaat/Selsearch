cmake_minimum_required(VERSION 3.25)
project(selsearch VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(GNUInstallDirs)

option(SELSEARCH_WITH_ONNX "PP-OCRv5 text recognition through ONNX Runtime" ON)
# Tesseract is a system package on Linux; on Windows it is opt-in.
if(WIN32)
    set(SELSEARCH_TESSERACT_DEFAULT OFF)
else()
    set(SELSEARCH_TESSERACT_DEFAULT ON)
endif()
option(SELSEARCH_WITH_TESSERACT "Tesseract text recognition (fallback engine)" ${SELSEARCH_TESSERACT_DEFAULT})
option(SELSEARCH_WITH_WEBENGINE "Embedded results panel (selsearch-web, QtWebEngine)" ON)
option(SELSEARCH_BUILD_TESTS "Build unit tests" ON)

if(WIN32)
    # Windows: in-app hotkeys, GDI capture, Win32 window list; QtWidgets only
    # for the tray icon (QSystemTrayIcon).
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Svg Network Concurrent ShaderTools Widgets)
    find_package(PkgConfig QUIET)
else()
    find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Svg Network DBus Concurrent ShaderTools WaylandClient)
    find_package(LayerShellQt 6.0 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
endif()

qt_standard_project_setup(REQUIRES 6.8)

if(WIN32)
    # One output dir for every program and test: Windows looks for DLLs next
    # to the executable first (System32 even ships an old onnxruntime.dll that
    # would win over PATH), and selsearchd finds selsearch-web there.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

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
if(NOT WIN32)
    selsearch_wayland_protocol(${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-screencopy-unstable-v1.xml wlr-screencopy-unstable-v1)
    selsearch_wayland_protocol(${WAYLAND_PROTOCOLS_DIR}/staging/ext-image-capture-source/ext-image-capture-source-v1.xml ext-image-capture-source-v1)
    selsearch_wayland_protocol(${WAYLAND_PROTOCOLS_DIR}/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml ext-image-copy-capture-v1)
    selsearch_wayland_protocol(${WAYLAND_PROTOCOLS_DIR}/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml ext-foreign-toplevel-list-v1)
endif()

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
    src/search/*.cpp src/search/*.h
    src/platform/GlobalShortcuts.cpp src/platform/KeyNames.cpp src/platform/*.h)
if(UNIX)
    list(APPEND CORE_SOURCES src/platform/PortalShortcuts.cpp)
endif()
# Platform backends: Windows*.cpp only on Windows; the Wayland and portal
# capture (DBus, wl libs) only elsewhere.
if(WIN32)
    list(FILTER CORE_SOURCES EXCLUDE REGEX "/src/capture/(Wayland|Portal)Capture\\.cpp$")
    list(APPEND CORE_SOURCES src/platform/WindowsShortcuts.cpp)
else()
    list(FILTER CORE_SOURCES EXCLUDE REGEX "/Windows[^/]*\\.(cpp|h)$")
endif()

add_library(selsearch_core STATIC ${CORE_SOURCES} ${PROTOCOL_SOURCES})
target_include_directories(selsearch_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PROTO_OUT}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tomlplusplus)
target_link_libraries(selsearch_core PUBLIC
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Concurrent Qt6::Qml)
if(WIN32)
    # dwmapi: window frames and cloaking; shell32/ole32: Start Menu icons.
    target_link_libraries(selsearch_core PUBLIC dwmapi shell32 ole32 user32 gdi32)
    target_compile_definitions(selsearch_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
    target_link_libraries(selsearch_core PUBLIC Qt6::DBus PkgConfig::WAYLAND_CLIENT)
endif()
target_compile_definitions(selsearch_core PUBLIC
    SELSEARCH_VERSION="${PROJECT_VERSION}"
    QT_NO_KEYWORDS)

# ICU: dictionary-based word breaks for Chinese/Japanese OCR results.
if(WIN32)
    # Windows 10 1903+ ships ICU (icu.dll) with its C API in the SDK's icu.h.
    # MinGW has no icu.h; the QTextBoundaryFinder fallback is used then.
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_LIBRARIES icu)
    check_cxx_source_compiles([=[
        #include <icu.h>
        int main() {
            UErrorCode status = U_ZERO_ERROR;
            UBreakIterator *it = ubrk_open(UBRK_WORD, "ja", nullptr, 0, &status);
            ubrk_close(it);
            return 0;
        }]=] SELSEARCH_HAVE_WINDOWS_ICU)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(SELSEARCH_HAVE_WINDOWS_ICU)
        target_link_libraries(selsearch_core PUBLIC icu)
        target_compile_definitions(selsearch_core PUBLIC SELSEARCH_HAVE_WINDOWS_ICU=1)
    endif()
else()
    pkg_check_modules(ICU IMPORTED_TARGET icu-uc icu-i18n)
    if(ICU_FOUND)
        target_link_libraries(selsearch_core PUBLIC PkgConfig::ICU)
        target_compile_definitions(selsearch_core PUBLIC SELSEARCH_HAVE_ICU=1)
    endif()
endif()

if(SELSEARCH_WITH_ONNX)
    find_package(onnxruntime QUIET)
    if(NOT onnxruntime_FOUND)
        find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h PATH_SUFFIXES onnxruntime onnxruntime/core/session)
        find_library(ONNXRUNTIME_LIBRARY onnxruntime)
        if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
            add_library(onnxruntime::onnxruntime UNKNOWN IMPORTED)
            if(WIN32)
                # The release zip has the DLL next to the import library.
                get_filename_component(ONNXRUNTIME_LIBDIR ${ONNXRUNTIME_LIBRARY} DIRECTORY)
                set(ONNXRUNTIME_DLL ${ONNXRUNTIME_LIBDIR}/onnxruntime.dll)
            endif()
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
        if(WIN32 AND ONNXRUNTIME_DLL AND EXISTS ${ONNXRUNTIME_DLL})
            # Next to our programs and tests (see CMAKE_RUNTIME_OUTPUT_DIRECTORY).
            file(COPY ${ONNXRUNTIME_DLL} DESTINATION ${CMAKE_BINARY_DIR}/bin)
        endif()
    else()
        message(WARNING "ONNX Runtime not found; PP-OCR engine disabled")
    endif()
endif()

if(SELSEARCH_WITH_TESSERACT AND PKG_CONFIG_FOUND)
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
if(NOT WIN32)
    list(FILTER DAEMON_SOURCES EXCLUDE REGEX "/Windows[^/]*\\.(cpp|h)$")
endif()
qt_add_executable(selsearchd ${DAEMON_SOURCES})
target_link_libraries(selsearchd PRIVATE selsearch_core
    Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Svg)
if(WIN32)
    # A GUI program (no console window); the tray icon needs QtWidgets.
    set_target_properties(selsearchd PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(selsearchd PRIVATE Qt6::Widgets)
else()
    target_link_libraries(selsearchd PRIVATE Qt6::WaylandClient LayerShellQt::Interface)
endif()

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
if(WIN32)
    target_compile_definitions(selsearch PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(selsearch PRIVATE shell32)
endif()

# ---------------------------------------------------------------------------
# selsearch-web: results panel, started on demand so idle RAM stays low.
if(SELSEARCH_WITH_WEBENGINE)
    find_package(Qt6 6.8 QUIET COMPONENTS WebEngineQuick)
    if(Qt6WebEngineQuick_FOUND)
        qt_add_executable(selsearch-web src/web/main.cpp src/app/WindowPlacement.cpp src/app/WindowPlacement.h)
        qt_add_qml_module(selsearch-web URI SelsearchWeb VERSION 1.0 QML_FILES qml/web/WebPanel.qml NO_RESOURCE_TARGET_PATH)
        target_include_directories(selsearch-web PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(selsearch-web PRIVATE Qt6::Quick Qt6::WebEngineQuick)
        if(WIN32)
            set_target_properties(selsearch-web PROPERTIES WIN32_EXECUTABLE TRUE)
            target_compile_definitions(selsearch-web PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
            target_link_libraries(selsearch-web PRIVATE user32)
        else()
            target_link_libraries(selsearch-web PRIVATE LayerShellQt::Interface)
        endif()
        install(TARGETS selsearch-web RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    else()
        message(WARNING "QtWebEngine not found; results open in the browser")
    endif()
endif()

install(TARGETS selsearchd selsearch RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WIN32)
    # Qt DLLs, QML and plugins are deployed with windeployqt; see
    # docs/BUILDING-windows.md and packaging/windows/.
    if(ONNXRUNTIME_DLL AND EXISTS ${ONNXRUNTIME_DLL})
        install(FILES ${ONNXRUNTIME_DLL} DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
else()
    configure_file(packaging/selsearch.service.in ${CMAKE_CURRENT_BINARY_DIR}/selsearch.service @ONLY)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/selsearch.service DESTINATION lib/systemd/user)
    install(FILES packaging/selsearch.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()

if(SELSEARCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
