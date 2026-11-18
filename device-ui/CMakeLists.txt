cmake_minimum_required(VERSION 3.16)
project(cp0-sync-ui VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CP0_UI_BUILD_TESTS "Build device-ui unit tests" ON)
option(CP0_UI_BUILD_APP "Build cp0-sync-ui LVGL executable" ON)
option(CP0_UI_TARGET_DEVICE "Target CardputerZero Linux fbdev/evdev instead of SDL2 simulator" OFF)

find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

# Optional LVGL v9 discovery (never download over network via FetchContent)
set(CP0_LVGL_TARGET "")
find_package(lvgl QUIET)
if(TARGET lvgl::lvgl)
    set(CP0_LVGL_TARGET lvgl::lvgl)
elseif(TARGET lvgl)
    set(CP0_LVGL_TARGET lvgl)
elseif(PkgConfig_FOUND)
    pkg_check_modules(LVGL QUIET IMPORTED_TARGET lvgl)
    if(TARGET PkgConfig::LVGL)
        set(CP0_LVGL_TARGET PkgConfig::LVGL)
    endif()
endif()

add_library(cp0_ui_core STATIC
    src/theme.cpp
    src/ipc_client.cpp
    src/ui_controller.cpp
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
    set(CP0_FREETYPE_TARGET "")
    find_package(Freetype QUIET)
    if(TARGET Freetype::Freetype)
        set(CP0_FREETYPE_TARGET Freetype::Freetype)
    elseif(PkgConfig_FOUND)
        pkg_check_modules(FREETYPE QUIET IMPORTED_TARGET freetype2)
        if(TARGET PkgConfig::FREETYPE)
            set(CP0_FREETYPE_TARGET PkgConfig::FREETYPE)
        endif()
    endif()

    add_executable(cp0-sync-ui
        src/app_ui.cpp
        src/main.cpp
    )
    target_link_libraries(cp0-sync-ui PRIVATE
        cp0_ui_core
        Threads::Threads
    )
    target_compile_options(cp0-sync-ui PRIVATE -Wall -Wextra -Wpedantic)

    if(CP0_FREETYPE_TARGET)
        target_compile_definitions(cp0-sync-ui PRIVATE CP0_HAS_FREETYPE=1)
        target_link_libraries(cp0-sync-ui PRIVATE ${CP0_FREETYPE_TARGET})
        message(STATUS "FreeType2 enabled via ${CP0_FREETYPE_TARGET}")
    else()
        message(STATUS "FreeType2 not found: using built-in 5x7 bitmap fallback")
    endif()

    if(CP0_LVGL_TARGET)
        target_compile_definitions(cp0-sync-ui PRIVATE CP0_HAS_LVGL=1)
        target_link_libraries(cp0-sync-ui PRIVATE ${CP0_LVGL_TARGET})
        message(STATUS "LVGL detected via ${CP0_LVGL_TARGET}")
    else()
        message(STATUS "LVGL not installed: using built-in 320x170 ShadcnCanvas renderer")
    endif()

    if(CP0_UI_TARGET_DEVICE)
        if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
            message(FATAL_ERROR "CP0_UI_TARGET_DEVICE=ON requires Linux (fbdev/evdev); got CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}")
        endif()
        target_compile_definitions(cp0-sync-ui PRIVATE CP0_UI_TARGET_DEVICE=1)
        message(STATUS "Target backend: Linux fbdev + evdev")
    else()
        set(CP0_SDL2_TARGET "")
        if(PkgConfig_FOUND)
            pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
            if(TARGET PkgConfig::SDL2)
                set(CP0_SDL2_TARGET PkgConfig::SDL2)
            endif()
        endif()
        if(NOT CP0_SDL2_TARGET)
            find_package(SDL2 REQUIRED)
            if(TARGET SDL2::SDL2)
                set(CP0_SDL2_TARGET SDL2::SDL2)
            endif()
        endif()
        if(CP0_SDL2_TARGET)
            target_link_libraries(cp0-sync-ui PRIVATE ${CP0_SDL2_TARGET})
        else()
            target_include_directories(cp0-sync-ui PRIVATE ${SDL2_INCLUDE_DIRS})
            target_link_libraries(cp0-sync-ui PRIVATE ${SDL2_LIBRARIES})
        endif()
        message(STATUS "Target backend: SDL2 320x170 simulator")
    endif()

    install(TARGETS cp0-sync-ui RUNTIME DESTINATION bin)
endif()

if(CP0_UI_BUILD_TESTS)
    enable_testing()
    add_executable(test_ipc_and_theme
        tests/test_ipc_and_theme.cpp
    )
    target_link_libraries(test_ipc_and_theme PRIVATE
        cp0_ui_core
        Threads::Threads
    )
    target_compile_options(test_ipc_and_theme PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME test_ipc_and_theme COMMAND test_ipc_and_theme)

    add_executable(test_ui_navigation
        tests/test_ui_navigation.cpp
    )
    target_link_libraries(test_ui_navigation PRIVATE
        cp0_ui_core
        Threads::Threads
    )
    target_compile_options(test_ui_navigation PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME test_ui_navigation COMMAND test_ui_navigation)
endif()
