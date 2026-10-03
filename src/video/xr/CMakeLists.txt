add_files(
    xr_autotest.cpp
    xr_autotest.h
    xr_config.cpp
    xr_config.h
    xr_gl.cpp
    xr_gl.h
    xr_input.h
    xr_panel.h
    xr_settings_gui.cpp
    xr_settings_gui.h
    xr_v.cpp
    xr_v.h
    CONDITION XR_QUEST
)

if(XR_QUEST)
    target_sources(openttd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/xr_android_main.cpp)
endif()

# Desktop preview (Windows/Linux host): only the VR settings tab, to test the UI without a headset.
add_files(
    xr_config.cpp
    xr_config.h
    xr_settings_gui.cpp
    xr_settings_gui.h
    CONDITION XR_DESKTOP_PREVIEW
)
