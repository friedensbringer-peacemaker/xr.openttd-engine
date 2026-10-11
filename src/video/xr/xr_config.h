/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file xr_config.h VR settings of the Meta Quest build (XR-OpenTTD), stored in xr.cfg in the personal directory. */

#ifndef VIDEO_XR_CONFIG_H
#define VIDEO_XR_CONFIG_H

#include <string>

/** All VR-only settings. Game settings (interface size, fonts, ...) stay in openttd.cfg. */
struct XrConfig {
	/* Pointer */
	int pointer_hand = 1;          ///< 0: left, 1: right.
	int smoothing = 1;             ///< 0: off, 1: light, 2: strong.
	bool click_stabilization = true; ///< Freeze the pointer ~85 ms after pressing the trigger.
	bool haptics = true;           ///< Short pulse on click.
	int ray_width = 1;             ///< 0: thin, 1: medium, 2: thick, 3: off (always shown while a VR menu is open).
	int ray_color = 0;             ///< 0: white, 1: grey, 2: dark, 3: amber.
	int marker_size = 1;           ///< 0: small, 1: medium, 2: large, 3: off.
	int cursor_size = 2;           ///< Mouse arrow of the game: 0: off, 1: small, 2: normal, 3: large.
	int pointer_offset_x = 0;      ///< Game mouse vs. laser in 0.25 % steps of the image width, -60..60 (+ = right), see xr_pointer_offset.h.
	int pointer_offset_y = 0;      ///< Same for the image height (+ = down).

	/* Screen */
	int panel_width_mm = 2000;     ///< Arc length of the screen.
	int distance_mm = 1800;        ///< Distance from the head at start.
	int height_mm = 0;             ///< Screen centre above (+) or below (-) eye height.
	int curvature = 1;             ///< 0: flat, 1: gentle (45 deg), 2: curved (75 deg).
	int brightness = 100;          ///< Percent, 50..150.
	int contrast = 100;            ///< Percent, 50..150.
	int sharpening = 1;            ///< Compositor filter of the screen: 0: off, 1: supersampling, 2: sharpening.
	int passthrough = 0;           ///< Room as background in percent (0 = off, 25/50/75/100).

	/* Controls */
	int map_speed = 1;             ///< Right stick map scrolling: 0: 1x, 1: 2x, 2: 4x.
	bool invert_zoom = false;      ///< Left stick up zooms out instead of in.
	bool smart_scroll = true;      ///< Right stick scrolls the list under the pointer instead of the map.

	/* Transient commands, not stored. */
	unsigned recenter_request = 0; ///< Incremented to recenter the screen in front of the head (size unchanged).
	unsigned reset_request = 0;    ///< Incremented to recenter with the default size and distance.
	unsigned size_request = 0;     ///< Incremented when width or distance were chosen in the menu.
	unsigned haptic_request = 0;   ///< Incremented by the menus: one short pulse on the pointer hand per step.
	bool passthrough_available = false; ///< Set by the driver: XR_FB_passthrough works on this device.
};

XrConfig &GetXrConfig();
void XrConfigSetPath(const std::string &path);
void XrConfigLoad();
void XrConfigSave();

/** Open or close the in-game VR settings window. */
void ShowXrSettings();

#endif /* VIDEO_XR_CONFIG_H */
