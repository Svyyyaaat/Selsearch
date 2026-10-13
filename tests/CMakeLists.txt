find_package(Qt6 REQUIRED COMPONENTS Test)
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tst_*.cpp)
foreach(src ${TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE selsearch_core Qt6::Test)
    target_compile_definitions(${name} PRIVATE SELSEARCH_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()

add_executable(selsearch-capture-dump tools/capture_dump.cpp)
target_link_libraries(selsearch-capture-dump PRIVATE selsearch_core)

# Virtual pointer driver for headless UI tests (wlroots compositors).
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/wlr-virtual-pointer-unstable-v1-client.h ${CMAKE_CURRENT_BINARY_DIR}/wlr-virtual-pointer-unstable-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER} client-header ${PROJECT_SOURCE_DIR}/protocols/wlr-virtual-pointer-unstable-v1.xml ${CMAKE_CURRENT_BINARY_DIR}/wlr-virtual-pointer-unstable-v1-client.h
    COMMAND ${WAYLAND_SCANNER} private-code ${PROJECT_SOURCE_DIR}/protocols/wlr-virtual-pointer-unstable-v1.xml ${CMAKE_CURRENT_BINARY_DIR}/wlr-virtual-pointer-unstable-v1-protocol.c
    DEPENDS ${PROJECT_SOURCE_DIR}/protocols/wlr-virtual-pointer-unstable-v1.xml VERBATIM)
add_executable(selsearch-vptr tools/vptr.c
    ${CMAKE_CURRENT_BINARY_DIR}/wlr-virtual-pointer-unstable-v1-client.h ${CMAKE_CURRENT_BINARY_DIR}/wlr-virtual-pointer-unstable-v1-protocol.c)
target_include_directories(selsearch-vptr PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(selsearch-vptr PRIVATE PkgConfig::WAYLAND_CLIENT m)
add_executable(selsearch-ocr-dump tools/ocr_dump.cpp)
target_link_libraries(selsearch-ocr-dump PRIVATE selsearch_core Qt6::Gui)
