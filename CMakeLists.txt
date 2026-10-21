cmake_minimum_required(VERSION 3.24)
project(casement_plugin_meshcentral VERSION 0.1.0 LANGUAGES CXX)

include(cmake/casement.cmake)
enable_testing()

# The plugin's code, shared by the program and its tests. Everything it
# needs besides nlohmann/json is part of Windows: WinHTTP for WebSockets and
# TLS, WIC for the screen tiles.
add_library(meshcentral STATIC
    src/meshcentral/clipboard.cpp
    src/meshcentral/control.cpp
    src/meshcentral/cursors.cpp
    src/meshcentral/desktop.cpp
    src/meshcentral/keys.cpp
    src/meshcentral/options.cpp
    src/meshcentral/tiles.cpp
    src/meshcentral/websocket.cpp)
target_include_directories(meshcentral PUBLIC src)
target_compile_definitions(meshcentral PUBLIC CASEMENT_PLUGIN_VERSION="${PROJECT_VERSION}")
target_link_libraries(meshcentral PUBLIC casement winhttp windowscodecs ole32 uuid bcrypt crypt32 gdi32 user32)

casement_test(unit_tests unit
    tests/unit/main.cpp
    tests/unit/clipboard_tests.cpp
    tests/unit/control_tests.cpp
    tests/unit/cursor_tests.cpp
    tests/unit/desktop_tests.cpp
    tests/unit/keys_tests.cpp
    tests/unit/options_tests.cpp
    tests/unit/tiles_tests.cpp)
target_link_libraries(unit_tests PRIVATE meshcentral)
