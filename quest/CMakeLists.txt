cmake_minimum_required(VERSION 3.18)
project(wc2quest C CXX)

# Wing Commander II for Meta Quest: SDL2 port of the wc2-re reconstruction plus an
# OpenXR host (src/quest).  Built by Gradle (quest/android) for arm64-v8a.

get_filename_component(WC2_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(QUEST_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(SDL2_DIR_SRC "${QUEST_DIR}/third_party/SDL2-2.32.10")
set(OPENXR_INCLUDE "${QUEST_DIR}/third_party/openxr/include")
set(OPENXR_LOADER "${QUEST_DIR}/third_party/openxr/lib/arm64-v8a/libopenxr_loader.so")

# --- SDL2 (shared, libSDL2.so as SDLActivity expects) ---------------------
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
add_subdirectory("${SDL2_DIR_SRC}" sdl2 EXCLUDE_FROM_ALL)

add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES IMPORTED_LOCATION "${OPENXR_LOADER}")

# --- Game sources (same lists as the Makefile's modern target) -------------
set(GAMEPLAY_C
    auto barracks brains cdrom cmpgn cockpt dib disk eventmgr geom gr globals
    hudmsg killbrd logic main mathfp mathutil mono music nav personnel pload
    screen screens ship smart spc strdos sysinput system text winmain)
set(SRCS_C "")
foreach(name ${GAMEPLAY_C})
    list(APPEND SRCS_C "${WC2_ROOT}/src/${name}.c")
endforeach()
list(APPEND SRCS_C
    ${WC2_ROOT}/src/sdl/compat.c ${WC2_ROOT}/src/sdl/input.c
    ${WC2_ROOT}/src/sdl/resources.c ${WC2_ROOT}/src/sdl/registry.c
    ${WC2_ROOT}/src/sdl/thread.c ${WC2_ROOT}/src/sdl/timer.c
    ${WC2_ROOT}/src/sdl/video_state.c
    ${WC2_ROOT}/src/sdl/audio.c ${WC2_ROOT}/src/sdl/cutscene.c
    ${WC2_ROOT}/src/sdl/difficulty.c ${WC2_ROOT}/src/sdl/events.c
    ${WC2_ROOT}/src/sdl/gl_renderer.c ${WC2_ROOT}/src/sdl/joystick.c
    ${WC2_ROOT}/src/sdl/music.c ${WC2_ROOT}/src/sdl/video.c
    ${WC2_ROOT}/src/sdl/launcher.c
    ${WC2_ROOT}/src/quest/quest_xr.c ${WC2_ROOT}/src/quest/quest_input.c
    ${WC2_ROOT}/src/quest/quest_stereo.c
    ${QUEST_DIR}/third_party/minilzo/minilzo.c)

set(SRCS_GAME_CXX ${WC2_ROOT}/src/debug.cpp ${WC2_ROOT}/src/dibtime.cpp
    ${WC2_ROOT}/src/pilot.cpp)
set(SRCS_SOUND_AS_CXX ${WC2_ROOT}/src/sound.c)
set(IX_NAMES ixlog streamer thread dsp dsps mixer dspv system sound sample
    lzo1x)
set(SRCS_IX "")
foreach(name ${IX_NAMES})
    list(APPEND SRCS_IX "${WC2_ROOT}/src/ix/${name}.cpp")
endforeach()
set(SRCS_HOST_CXX ${WC2_ROOT}/src/sdl/originfx.cpp ${WC2_ROOT}/src/sdl/wave.cpp
    ${WC2_ROOT}/third_party/ymfm/ymfm_adpcm.cpp
    ${WC2_ROOT}/third_party/ymfm/ymfm_opl.cpp
    ${WC2_ROOT}/third_party/ymfm/ymfm_pcm.cpp)

add_library(main SHARED ${SRCS_C} ${SRCS_GAME_CXX} ${SRCS_SOUND_AS_CXX}
    ${SRCS_IX} ${SRCS_HOST_CXX})

set_source_files_properties(${SRCS_SOUND_AS_CXX} PROPERTIES LANGUAGE CXX
    COMPILE_OPTIONS "-xc++;-Dix_system_new_sound=SdlNewWaveSound")
# Same per-file define as the Makefile's music.o rule.
set_source_files_properties(${WC2_ROOT}/src/music.c PROPERTIES
    COMPILE_OPTIONS "-DSdlUsingOriginFxSoundEffects=SdlHandlesGameSoundEffects")
set_source_files_properties(${SRCS_IX} ${SRCS_SOUND_AS_CXX} PROPERTIES
    INCLUDE_DIRECTORIES "${WC2_ROOT}/src/ix")
set_source_files_properties(${SRCS_HOST_CXX} PROPERTIES
    INCLUDE_DIRECTORIES "${WC2_ROOT}/third_party/ymfm")

target_compile_definitions(main PRIVATE SDL_PORT=1 WC2_QUEST=1)
target_include_directories(main PRIVATE
    ${WC2_ROOT}/include
    ${QUEST_DIR}/third_party/minilzo
    ${OPENXR_INCLUDE}
    ${SDL2_DIR_SRC}/include)

# x86/MSVC char is signed; the reconstructed code relies on it.
set(COMMON_FLAGS -fsigned-char -O2 -g -ffunction-sections -fdata-sections
    -Wno-return-type -Wno-unknown-warning-option
    -Wno-incompatible-pointer-types -Wno-int-conversion
    -Wno-implicit-function-declaration -Wno-pointer-to-int-cast
    -Wno-int-to-pointer-cast -Wno-format -Wno-parentheses
    -Wno-dangling-else -Wno-shift-op-parentheses -Wno-empty-body
    -Wno-deprecated-non-prototype -Wno-tautological-constant-out-of-range-compare)
target_compile_options(main PRIVATE ${COMMON_FLAGS}
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++14 -Wno-c++11-narrowing -Wno-register>)
# pilot.cpp needs construction cleanup (C++ EH), see docs/COMPILER.md.
set_source_files_properties(${WC2_ROOT}/src/pilot.cpp PROPERTIES
    COMPILE_OPTIONS "-fexceptions")

target_link_libraries(main PRIVATE SDL2 openxr_loader EGL GLESv3 android log z
    -Wl,--gc-sections)
