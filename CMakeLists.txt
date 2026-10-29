cmake_minimum_required(VERSION 3.24)
project(casement_plugin_meshcentral VERSION 0.1.0 LANGUAGES CXX)

# The static C runtime with MSVC too, as MinGW links statically
# (cmake/casement.cmake): one self-contained exe, no runtime DLLs to ship.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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
    src/meshcentral/session.cpp
    src/meshcentral/tiles.cpp
    src/meshcentral/websocket.cpp)
target_include_directories(meshcentral PUBLIC src)
target_compile_definitions(meshcentral PUBLIC CASEMENT_PLUGIN_VERSION="${PROJECT_VERSION}")
target_link_libraries(meshcentral PUBLIC casement winhttp windowscodecs ole32 uuid bcrypt crypt32 gdi32 user32)

casement_plugin_executable(casement-meshcentral src/main.cpp)
target_link_libraries(casement-meshcentral PRIVATE meshcentral)

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

# Needs the MeshCentral server and agent that ci/e2e.sh starts.
casement_test(e2e_tests e2e tests/e2e/meshcentral_tests.cpp)
target_link_libraries(e2e_tests PRIVATE casement_sim meshcentral)
target_compile_definitions(e2e_tests PRIVATE PLUGIN_EXE=L"$<TARGET_FILE_NAME:casement-meshcentral>")
add_dependencies(e2e_tests casement-meshcentral)
set_tests_properties(e2e_tests PROPERTIES TIMEOUT 900)
