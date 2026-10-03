# XR-OpenTTD: stages the runtime data for the APK assets (run as POST_BUILD of openttd).
#
#   SRC  = CMake build directory (lang/, baseset/, ai/, game/)
#   FREE = directory with the free base sets (opengfx/opensfx/openmsx *.tar)
#   OUT  = asset directory Gradle packs into the APK
#
# Result: OUT/xr_data/<files> and OUT/xr_data.lst (version line, then one relative path per line).
# xr_android_main.cpp copies these files to the device on the first start of each build.

file(REMOVE_RECURSE "${OUT}/xr_data")
file(MAKE_DIRECTORY "${OUT}/xr_data")
foreach(DIR lang baseset ai game)
    if(EXISTS "${SRC}/${DIR}")
        file(COPY "${SRC}/${DIR}" DESTINATION "${OUT}/xr_data")
    endif()
endforeach()

file(GLOB FREE_SETS "${FREE}/*.tar")
if(NOT FREE_SETS)
    message(WARNING "XR-OpenTTD: keine freien Basis-Sets in ${FREE} — das Spiel startet dann nur mit eigenen TTD-Dateien")
endif()
file(COPY ${FREE_SETS} DESTINATION "${OUT}/xr_data/baseset")

# General-MIDI-SoundFont für die Musik (FluidSynth), mit Lizenz.
file(GLOB SOUNDFONT_FILES "${SOUNDFONT}/*.sf2" "${SOUNDFONT}/LICENSE*")
if(SOUNDFONT_FILES)
    file(COPY ${SOUNDFONT_FILES} DESTINATION "${OUT}/xr_data/soundfont")
else()
    message(WARNING "XR-OpenTTD: keine SoundFont in ${SOUNDFONT} — das Spiel läuft dann ohne Musik")
endif()

file(GLOB_RECURSE FILES RELATIVE "${OUT}/xr_data" "${OUT}/xr_data/*")
list(SORT FILES)
string(TIMESTAMP VERSION "%Y%m%d%H%M%S")
list(JOIN FILES "\n" LISTING)
file(WRITE "${OUT}/xr_data.lst" "${VERSION}\n${LISTING}\n")
list(LENGTH FILES COUNT)
message(STATUS "XR-OpenTTD: ${COUNT} Dateien für die APK bereitgestellt")
