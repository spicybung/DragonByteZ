cmake_minimum_required(VERSION 3.16)
project(DragonByteZ VERSION 0.7.0 LANGUAGES CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(dragonbytez_core
    src/analysis.cpp
    src/compression.cpp
    src/gba_bios.cpp
    src/png.cpp
    src/rom.cpp
    src/sprite_analysis.cpp
)
target_include_directories(dragonbytez_core PUBLIC include)
if(MSVC)
    target_compile_options(
        dragonbytez_core PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(
        dragonbytez_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(dragonbytez_cli src/main.cpp)
set_target_properties(dragonbytez_cli PROPERTIES OUTPUT_NAME "dragonbytez-cli")
target_link_libraries(dragonbytez_cli PRIVATE dragonbytez_core)
if(MSVC)
    target_compile_options(
        dragonbytez_cli PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(
        dragonbytez_cli PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(
    viogsf STATIC
    third_party/viogsf/vbam/apu/Blip_Buffer.cpp
    third_party/viogsf/vbam/apu/Effects_Buffer.cpp
    third_party/viogsf/vbam/apu/Gb_Apu.cpp
    third_party/viogsf/vbam/apu/Gb_Oscs.cpp
    third_party/viogsf/vbam/apu/Multi_Buffer.cpp
    third_party/viogsf/vbam/gba/bios.cpp
    third_party/viogsf/vbam/gba/GBA-arm.cpp
    third_party/viogsf/vbam/gba/GBA-thumb.cpp
    third_party/viogsf/vbam/gba/GBA.cpp
    third_party/viogsf/vbam/gba/Sound.cpp
)
target_include_directories(
    viogsf PUBLIC third_party/viogsf)
if(MSVC)
    target_compile_options(viogsf PRIVATE /W0)
else()
    target_compile_options(viogsf PRIVATE -w)
endif()

target_sources(dragonbytez_core PRIVATE src/gsf_player.cpp src/log1_runtime.cpp)
target_include_directories(
    dragonbytez_core PRIVATE third_party/viogsf)
target_link_libraries(dragonbytez_core PRIVATE viogsf)

if(WIN32)

    add_executable(
        DragonByteZ WIN32
        src/gui_main.cpp
        resources/DragonByteZ.rc
    )
    target_link_libraries(
        DragonByteZ
        PRIVATE
            dragonbytez_core
            comctl32
            comdlg32
            gdiplus
            winmm
            ole32
            shell32
            uxtheme
    )
    target_compile_definitions(
        DragonByteZ PRIVATE UNICODE _UNICODE NOMINMAX)
    if(MSVC)
        target_compile_options(
            DragonByteZ PRIVATE /W4 /WX /permissive- /utf-8)
        target_link_options(DragonByteZ PRIVATE /MANIFEST:NO)
    else()
        target_compile_options(
            DragonByteZ PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(dragonbytez_tests tests/tests.cpp)
    target_link_libraries(dragonbytez_tests PRIVATE dragonbytez_core)
    if(MSVC)
        target_compile_options(
            dragonbytez_tests PRIVATE /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(
            dragonbytez_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME dragonbytez_tests COMMAND dragonbytez_tests)
endif()
