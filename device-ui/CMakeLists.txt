cmake_minimum_required(VERSION 3.16)
project(cp0-sync-ui VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CP0_UI_BUILD_TESTS "Build device-ui unit tests" ON)
option(CP0_UI_BUILD_APP "Build cp0-sync-ui LVGL executable" ON)
option(CP0_UI_TARGET_DEVICE "Target CardputerZero Linux fbdev/evdev instead of SDL2 simulator" OFF)

find_package(SQLite3 REQUIRED)

add_library(cp0_ui_core STATIC
    src/theme.cpp
    src/ipc_client.cpp
)
target_include_directories(cp0_ui_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(TARGET SQLite3::SQLite3)
    target_link_libraries(cp0_ui_core PUBLIC SQLite3::SQLite3)
else()
    target_link_libraries(cp0_ui_core PUBLIC SQLite::SQLite3)
endif()
target_compile_options(cp0_ui_core PRIVATE -Wall -Wextra -Wpedantic)

if(CP0_UI_BUILD_APP)
    message(STATUS "CP0_UI_BUILD_APP=ON: app target will be configured in Task 6")
endif()

if(CP0_UI_BUILD_TESTS)
    enable_testing()
    add_executable(test_ipc_and_theme
        tests/test_ipc_and_theme.cpp
    )
    target_link_libraries(test_ipc_and_theme PRIVATE cp0_ui_core)
    target_compile_options(test_ipc_and_theme PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME test_ipc_and_theme COMMAND test_ipc_and_theme)
endif()
