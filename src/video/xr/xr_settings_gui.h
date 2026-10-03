/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file xr_settings_gui.h The "VR" tab of the game options window (Meta Quest build).
 *
 * The tab lives in the normal game options window, in the same look as the other tabs.
 * Its widgets use numbers from #WID_GO_XR_FIRST on, so OpenTTD's own widget lists stay untouched;
 * the game options window forwards those widgets to the functions below.
 */

#ifndef VIDEO_XR_SETTINGS_GUI_H
#define VIDEO_XR_SETTINGS_GUI_H

#include "../../window_type.h"
#include "../../widget_type.h"

#include <optional>
#include <string>

static constexpr WidgetID WID_GO_TAB_XR = 999;   ///< Tab button "VR" in the game options.
static constexpr WidgetID WID_GO_XR_FIRST = 1000; ///< First widget of the VR tab.
static constexpr WidgetID WID_GO_XR_LAST = 1099;  ///< Last possible widget of the VR tab.

/** Widgets of the VR tab, numbered from #WID_GO_XR_FIRST. */
enum XrSettingsWidgets : WidgetID {
	WID_XRS_GUI_SCALE = WID_GO_XR_FIRST,
	WID_XRS_PANEL_WIDTH,
	WID_XRS_DISTANCE,
	WID_XRS_HEIGHT,
	WID_XRS_CURVATURE,
	WID_XRS_BRIGHTNESS,
	WID_XRS_CONTRAST,
	WID_XRS_SHARPENING,
	WID_XRS_PASSTHROUGH,
	WID_XRS_RECENTER,
	WID_XRS_RESET_SIZE,
	WID_XRS_POINTER_HAND,
	WID_XRS_CURSOR,
	WID_XRS_SMOOTHING,
	WID_XRS_STABILISATION,
	WID_XRS_HAPTICS,
	WID_XRS_RAY_WIDTH,
	WID_XRS_RAY_COLOUR,
	WID_XRS_MARKER,
	WID_XRS_MAP_SPEED,
	WID_XRS_ZOOM_DIRECTION,
	WID_XRS_SMART_SCROLL,
	WID_XRS_CLOSE,
	WID_XRS_HELP,
};

inline bool IsXrSettingsWidget(WidgetID widget) { return widget >= WID_GO_XR_FIRST && widget <= WID_GO_XR_LAST; }

std::unique_ptr<NWidgetBase> MakeXrSettingsPane();
std::optional<std::string> XrSettingsWidgetString(WidgetID widget);
void XrSettingsUpdateSize(WidgetID widget, Dimension &size, const Dimension &padding);
void XrSettingsDraw(const Rect &r, WidgetID widget);
void XrSettingsClick(Window *w, WidgetID widget);
void XrSettingsDropdownSelect(WidgetID widget, int index);

/* VR quick menu (left menu button), see xr_settings_gui.cpp. */
void ShowXrQuickMenu();
bool XrQuickMenuOpen();
void XrQuickMenuNavigate(int delta);
void XrQuickMenuActivate();

/** Open the game options on the VR tab (implemented in settings_gui.cpp). */
void ShowGameOptionsXrTab();

#endif /* VIDEO_XR_SETTINGS_GUI_H */
