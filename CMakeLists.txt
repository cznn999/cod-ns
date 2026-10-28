cmake_minimum_required(VERSION 3.20)
project(cod1-port C)

# ccache, when installed: the generated code is large and mostly unchanged between builds.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# The recompiled code is generated from the user's own copy of the game: the
# single-player executable and the three game DLLs it loads.
set(COD_DIR "${CMAKE_SOURCE_DIR}/gamedata" CACHE PATH "Game directory (CoDSP.exe, gamex86.dll, cgamex86.dll, uix86.dll)")
# cod: Call of Duty (CoDSP.exe 1.3); uo: United Offensive (CoDUOSP.exe and uo_*.dll).
set(COD_GAME "cod" CACHE STRING "Game to recompile: cod or uo")
set_property(CACHE COD_GAME PROPERTY STRINGS cod uo)
if(COD_GAME STREQUAL "uo")
    set(COD_GAME_FILES CoDUOSP.exe uo_gamex86.dll uo_cgamex86.dll uo_uix86.dll)
    # Packed after CoDUOSP.exe's image (0x00400000-0x0492A000), below the thunks at
    # 0x05200000 (runtime/include/game.h).
    set(COD_MODULES
        "${COD_DIR}/CoDUOSP.exe"
        "${COD_DIR}/uo_gamex86.dll@04930000"
        "${COD_DIR}/uo_cgamex86.dll@04C60000"
        "${COD_DIR}/uo_uix86.dll@04FC0000")
    add_compile_definitions(COD_GAME_UO)
elseif(COD_GAME STREQUAL "cod")
    set(COD_GAME_FILES CoDSP.exe gamex86.dll cgamex86.dll uix86.dll)
    # Packed after CoDSP.exe's image (0x00400000-0x01864000), below the thunks at
    # 0x02000000 (runtime/include/game.h).
    set(COD_MODULES
        "${COD_DIR}/CoDSP.exe"
        "${COD_DIR}/gamex86.dll@01870000"
        "${COD_DIR}/cgamex86.dll@01B60000"
        "${COD_DIR}/uix86.dll@01DE0000")
else()
    message(FATAL_ERROR "COD_GAME must be cod or uo")
endif()
foreach(f ${COD_GAME_FILES})
    if(NOT EXISTS "${COD_DIR}/${f}")
        message(FATAL_ERROR "${f} not found in ${COD_DIR}; pass -DCOD_DIR=/path/to/the/game")
    endif()
endforeach()

# Version of the port, separate for each game (shown in hbmenu as major.minor+patch,
# e.g. 0.2.1 as 0.21).
if(COD_GAME STREQUAL "uo")
    set(COD_PORT_VERSION "0.1.0")
else()
    set(COD_PORT_VERSION "0.3.0")
endif()
if(NOT COD_PORT_VERSION MATCHES "^([0-9]+)\\.([0-9])\\.([0-9])$")
    message(FATAL_ERROR "COD_PORT_VERSION must be major.minor.patch with one-digit minor and patch")
endif()
math(EXPR COD_VERSION_MAJOR "${CMAKE_MATCH_1}")
if(COD_VERSION_MAJOR LESS 10)
    set(COD_VERSION_MAJOR "0${COD_VERSION_MAJOR}")
endif()
set(COD_APP_VER "${COD_VERSION_MAJOR}.${CMAKE_MATCH_2}${CMAKE_MATCH_3}")
set(COD_SHORT_VER "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}${CMAKE_MATCH_3}")

if(DEFINED VITA_ELF_CREATE)
    # PlayStation Vita (vitasdk toolchain file).
    set(VITA TRUE)
    include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
    set(VITA_TITLEID "CODS00001" CACHE STRING "Vita title id")
    # ATTRIBUTE2=12: the extended memory mode (about 109 MiB more main memory); the
    # guest space (192 MiB), the host side and vitaGL do not fit without it.
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
    set(VITA_VERSION "${COD_APP_VER}")
    add_compile_definitions(__vita__)
    add_compile_options(-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-short-enums
                        -ffunction-sections -fdata-sections -fno-pic -fno-pie)
    add_link_options(-Wl,-q -Wl,--gc-sections -static -fno-short-enums -Wl,--no-warn-mismatch -Wl,-z,noexecstack)
endif()

if(NINTENDO_SWITCH)
    # Nintendo Switch (devkitPro: -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake).
    # The toolchain file adds -march=armv8-a+crc+crypto -mtune=cortex-a57 and -D__SWITCH__.
    set(SWITCH TRUE)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
file(GLOB RECOMP_TOOLS "${CMAKE_SOURCE_DIR}/tools/recomp/*.py")
list(TRANSFORM COD_GAME_FILES PREPEND "${COD_DIR}/" OUTPUT_VARIABLE COD_GAME_PATHS)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_TOOLS} ${COD_GAME_PATHS})
# Diagnostic entry probes (hex guest addresses), e.g. -DCOD_PROBES="452190 4521B6".
set(COD_PROBES "" CACHE STRING "Guest functions to instrument with rt_probe")

message(STATUS "Recompiling ${COD_DIR}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "COD_PROBES=${COD_PROBES}"
            "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/recomp/gen.py" "${GEN_DIR}" ${COD_MODULES}
    RESULT_VARIABLE GEN_RESULT
    OUTPUT_VARIABLE GEN_OUTPUT)
if(NOT GEN_RESULT EQUAL 0)
    message(FATAL_ERROR "recompiler failed:\n${GEN_OUTPUT}")
endif()
message(STATUS "${GEN_OUTPUT}")

file(STRINGS "${GEN_DIR}/files.txt" GEN_FILES)
list(TRANSFORM GEN_FILES PREPEND "${GEN_DIR}/")

add_library(cod_recomp STATIC ${GEN_FILES})
target_include_directories(cod_recomp PUBLIC runtime/include "${GEN_DIR}")
# Guest code relies on x86 semantics: wrapping signed arithmetic, type punning
# and signed char.
set(COD_RECOMP_OPT "-O2" CACHE STRING "Optimization flag of the generated code")
target_compile_options(cod_recomp PRIVATE -Wall -Wextra ${COD_RECOMP_OPT} -fwrapv -fno-strict-aliasing -fsigned-char
                       -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)
# On by default on the Vita: the Cortex-A9's double precision costs far more there.
if(VITA)
    set(COD_X87_FLOAT_DEFAULT ON)
else()
    set(COD_X87_FLOAT_DEFAULT OFF)
endif()
option(COD_X87_FLOAT "Single-precision x87 registers in generated code (faster on the Vita, inexact)" ${COD_X87_FLOAT_DEFAULT})
if(COD_X87_FLOAT)
    target_compile_definitions(cod_recomp PRIVATE COD_X87_FLOAT)
endif()
option(COD_X87_CHECK "Check statically derived x87 stack effects at run time" OFF)
if(COD_X87_CHECK)
    target_compile_definitions(cod_recomp PRIVATE COD_X87_CHECK)
endif()

file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS runtime/src/*.c)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/src/*.c)
if(VITA)
    list(FILTER SHIM_SOURCES EXCLUDE REGEX "platform_(sdl|switch)\\.c$|(gl|gamma)_switch\\.c$")
elseif(SWITCH)
    list(FILTER SHIM_SOURCES EXCLUDE REGEX "platform_(sdl|vita)\\.c$")
else()
    list(FILTER SHIM_SOURCES EXCLUDE REGEX "platform_(vita|switch)\\.c$|(gl|gamma)_switch\\.c$")
endif()

add_executable(cod1 ${RUNTIME_SOURCES} ${SHIM_SOURCES})
target_include_directories(cod1 PRIVATE runtime/include shim/include "${GEN_DIR}")
target_compile_options(cod1 PRIVATE -Wall -Wextra -fwrapv -fno-strict-aliasing -fsigned-char)
target_compile_definitions(cod1 PRIVATE COD_PORT_VERSION="${COD_PORT_VERSION}" COD_PORT_VERSION="${COD_PORT_VERSION}")
option(COD_PROFILE "Build with the sampling profiler" OFF)
if(COD_PROFILE)
    target_compile_definitions(cod_recomp PUBLIC COD_PROFILE)
    target_compile_definitions(cod1 PRIVATE COD_PROFILE)
endif()

if(VITA)
    target_include_directories(cod1 PRIVATE "$ENV{VITASDK}/arm-vita-eabi/include/SDL2")
    # The private vitaGL build (tools/build_vitagl_vita.sh, or the one of a sibling port).
    set(COD_VITAGL_DIR "${CMAKE_SOURCE_DIR}/deps/vita" CACHE PATH "Prefix of the private vitaGL build")
    if(EXISTS "${COD_VITAGL_DIR}/lib/libvitaGL.a")
        message(STATUS "Using the private vitaGL in ${COD_VITAGL_DIR}")
        target_include_directories(cod1 BEFORE PRIVATE "${COD_VITAGL_DIR}/include")
        set(COD_VITAGL_LIB "${COD_VITAGL_DIR}/lib/libvitaGL.a")
    else()
        message(STATUS "Private vitaGL not built: using the SDK's (tools/build_vitagl_vita.sh)")
        set(COD_VITAGL_LIB vitaGL)
    endif()
    target_link_libraries(cod1 PRIVATE cod_recomp
        SDL2 ${COD_VITAGL_LIB} vitashark SceShaccCgExt mathneon taihen_stub
        SceShaccCg_stub SceKernelDmacMgr_stub SceCtrl_stub SceTouch_stub SceHid_stub SceMotion_stub
        SceAudio_stub SceAudioIn_stub SceDisplay_stub SceGxm_stub SceCommonDialog_stub SceSysmodule_stub
        ScePower_stub SceAppMgr_stub SceAppUtil_stub SceIme_stub SceLibKernel_stub SceProcessmgr_stub
        SceIofilemgr_stub png z stdc++ pthread m c)
    # LiveArea pictures (pic0.png is also the boot picture) from the game's own art.
    set(COD_SCE_SYS "${CMAKE_BINARY_DIR}/sce_sys")
    file(COPY "${CMAKE_SOURCE_DIR}/platform/vita/sce_sys/" DESTINATION "${COD_SCE_SYS}")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/make_vita_livearea.py" "${COD_DIR}" "${COD_SCE_SYS}"
        RESULT_VARIABLE COD_LIVEAREA_RESULT)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/make_vita_livearea.py")
    if(NOT COD_LIVEAREA_RESULT EQUAL 0)
        message(FATAL_ERROR "tools/make_vita_livearea.py failed (it needs Pillow: pip install Pillow)")
    endif()
    vita_create_self(eboot.bin cod1 UNSAFE)
    vita_create_vpk(cod1.vpk ${VITA_TITLEID} eboot.bin
        VERSION ${VITA_VERSION}
        NAME "Call of Duty"
        FILE ${COD_SCE_SYS} sce_sys)
elseif(SWITCH)
    # SDL2 (switch-sdl2) for the window, the GL context (EGL) and audio; Mesa's
    # desktop OpenGL (switch-mesa: EGL, glapi, drm_nouveau) for the renderer.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    find_package(OpenGL REQUIRED)
    target_link_libraries(cod1 PRIVATE cod_recomp PkgConfig::SDL2 OpenGL::GL m)
    # Mesa (libEGL.a) is C++ and the executable is linked as C: libstdc++ has to
    # come after Mesa on the link line. A library listed here would be merged
    # with the one SDL2's pkg-config already puts before Mesa, so it goes at the
    # very end, with the platform's own libraries (-lnx -lm).
    set(CMAKE_C_STANDARD_LIBRARIES "-lstdc++ ${CMAKE_C_STANDARD_LIBRARIES}")
    set(COD_SWITCH_ICON "${CMAKE_SOURCE_DIR}/platform/switch/icon.jpg")
    if(COD_GAME STREQUAL "uo")
        set(COD_SWITCH_NAME "Call of Duty UO NX")
        # cod1uo.elf and cod1uo.nro (the NRO is named after the executable).
        set_target_properties(cod1 PROPERTIES OUTPUT_NAME cod1uo)
        if(EXISTS "${CMAKE_SOURCE_DIR}/platform/switch/icon_uo.jpg")
            set(COD_SWITCH_ICON "${CMAKE_SOURCE_DIR}/platform/switch/icon_uo.jpg")
        endif()
    else()
        set(COD_SWITCH_NAME "Call of Duty NX")
    endif()
    nx_generate_nacp(cod1.nacp NAME "${COD_SWITCH_NAME}" AUTHOR "cod-vita / Switch port" VERSION "${COD_SHORT_VER}")
    if(EXISTS "${COD_SWITCH_ICON}")
        nx_create_nro(cod1 NACP cod1.nacp ICON "${COD_SWITCH_ICON}")
    else()
        nx_create_nro(cod1 NACP cod1.nacp)
    endif()
else()
    find_package(SDL2 REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(cod1 PRIVATE cod_recomp SDL2::SDL2 OpenGL::GL m)
    if(APPLE)
        target_link_libraries(cod1 PRIVATE "-framework CoreFoundation")
        # The legacy (compatibility) OpenGL API the game uses is deprecated on macOS.
        target_compile_definitions(cod1 PRIVATE GL_SILENCE_DEPRECATION)
    else()
        target_link_libraries(cod1 PRIVATE pthread)
    endif()
endif()
