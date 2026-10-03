# XR-OpenTTD: Meta-Quest-Build (Android arm64, OpenXR).
#
# Wird vom Haupt-CMakeLists eingebunden, wenn XR_QUEST gesetzt ist (Gradle-Build in
# ../../android). Baut SDL2 (nur Ereignisse, Audio, Android-Glue) und liblzma aus
# third_party/ mit und bindet den vorgebauten Khronos-OpenXR-Loader ein.
#
# Erwartet: XR_THIRD_PARTY (Pfad zu XR-OpenTTD/third_party), HOST_BINARY_DIR (Host-Tools).

if(NOT ANDROID)
    message(FATAL_ERROR "XR_QUEST ist nur für den Android-Build gedacht")
endif()
if(NOT XR_THIRD_PARTY)
    message(FATAL_ERROR "XR_THIRD_PARTY fehlt")
endif()

# Auch der Gradle-Debug-Build muss auf der Quest flüssig laufen.
set(CMAKE_C_FLAGS_DEBUG "-O2 -g")
set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g")

# --- SDL2 (Android-Activity, Audio, Lebenszyklus-Ereignisse) ------------------
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE) # FluidSynth brings its own "uninstall" target
add_subdirectory(${XR_THIRD_PARTY}/SDL ${CMAKE_BINARY_DIR}/third_party/SDL EXCLUDE_FROM_ALL)
if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 ALIAS SDL2)
endif()
set(SDL2_FOUND TRUE)

# --- liblzma (Spielstände) ----------------------------------------------------
set(XZ_TOOL_XZ OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_XZDEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMADEC OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_LZMAINFO OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SCRIPTS OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SYMLINKS OFF CACHE BOOL "" FORCE)
set(XZ_TOOL_SYMLINKS_LZMA OFF CACHE BOOL "" FORCE)
set(XZ_DOC OFF CACHE BOOL "" FORCE)
set(XZ_NLS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING_SAVED ${BUILD_TESTING})
set(BUILD_TESTING OFF)
set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${XR_THIRD_PARTY}/xz ${CMAKE_BINARY_DIR}/third_party/xz EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})
set(BUILD_TESTING ${BUILD_TESTING_SAVED})
# xz only exports its headers on install; for the in-tree build add them to the interface.
set_property(TARGET liblzma APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${XR_THIRD_PARTY}/xz/src/liblzma/api")
if(NOT TARGET LibLZMA::LibLZMA)
    add_library(LibLZMA::LibLZMA ALIAS liblzma)
endif()
set(LIBLZMA_FOUND TRUE)

# --- FluidSynth (Musik: OpenMSX-MIDI → OpenTTD-Mixer, keine eigenen Audio-Backends) ----
foreach(XR_FS_OPTION enable-alsa enable-aufile enable-dbus enable-ipv6 enable-jack enable-ladspa
        enable-libsndfile enable-midishare enable-network enable-oss enable-sdl3 enable-pulseaudio
        enable-pipewire enable-readline enable-openmp enable-signalsmith enable-opensles enable-oboe
        enable-systemd enable-libinstpatch enable-portaudio)
    set(${XR_FS_OPTION} OFF CACHE BOOL "" FORCE)
endforeach()
set(osal "cpp11" CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${XR_THIRD_PARTY}/fluidsynth ${CMAKE_BINARY_DIR}/third_party/fluidsynth EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})
# OpenTTD's global -Wwrite-strings clashes with FluidSynth's -Werror=incompatible-pointer-types (C code).
target_compile_options(libfluidsynth-OBJ PRIVATE -Wno-write-strings)
set(Fluidsynth_FOUND TRUE)
set(Fluidsynth_LIBRARIES libfluidsynth)
set(Fluidsynth_INCLUDE_DIRS "${XR_THIRD_PARTY}/fluidsynth/include" "${CMAKE_BINARY_DIR}/third_party/fluidsynth/include")

# --- OpenXR (Khronos-Loader, arm64, aus XR-Settlers2.5 übernommen) -------------
add_library(xr_openxr_loader SHARED IMPORTED GLOBAL)
set_target_properties(xr_openxr_loader PROPERTIES
    IMPORTED_LOCATION "${XR_THIRD_PARTY}/openxr/lib/${ANDROID_ABI}/libopenxr_loader.so"
    INTERFACE_INCLUDE_DIRECTORIES "${XR_THIRD_PARTY}/openxr/include"
)

add_definitions(-DWITH_XR_QUEST)
