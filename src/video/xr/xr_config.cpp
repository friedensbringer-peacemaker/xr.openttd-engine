/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file xr_config.cpp Loading and saving of the VR settings (simple key=value file). */

#include "../../stdafx.h"
#include "xr_config.h"
#include "xr_pointer_offset.h"

#include <algorithm>
#include <charconv>
#include <fstream>

#include "../../safeguards.h"

static XrConfig _xr_config;
static std::string _xr_config_path;

XrConfig &GetXrConfig()
{
	return _xr_config;
}

void XrConfigSetPath(const std::string &path)
{
	_xr_config_path = path;
}

/** One stored value: name, pointer, minimum, maximum. */
struct XrConfigItem {
	const char *name;
	int *value;
	bool *flag;
	int min;
	int max;
};

/** All stored values of _xr_config. Booleans are stored as 0/1. */
static const XrConfigItem _xr_config_items[] = {
	{"pointer_hand", &_xr_config.pointer_hand, nullptr, 0, 1},
	{"smoothing", &_xr_config.smoothing, nullptr, 0, 2},
	{"click_stabilization", nullptr, &_xr_config.click_stabilization, 0, 1},
	{"haptics", nullptr, &_xr_config.haptics, 0, 1},
	{"ray_width", &_xr_config.ray_width, nullptr, 0, 3},
	{"ray_color", &_xr_config.ray_color, nullptr, 0, 3},
	{"marker_size", &_xr_config.marker_size, nullptr, 0, 3},
	{"cursor_size", &_xr_config.cursor_size, nullptr, 0, 3},
	{"pointer_offset_x", &_xr_config.pointer_offset_x, nullptr, -ptroff::kMaxSteps, ptroff::kMaxSteps},
	{"pointer_offset_y", &_xr_config.pointer_offset_y, nullptr, -ptroff::kMaxSteps, ptroff::kMaxSteps},
	{"panel_width_mm", &_xr_config.panel_width_mm, nullptr, 1000, 3200},
	{"distance_mm", &_xr_config.distance_mm, nullptr, 700, 3500},
	{"height_mm", &_xr_config.height_mm, nullptr, -600, 600},
	{"curvature", &_xr_config.curvature, nullptr, 0, 2},
	{"brightness", &_xr_config.brightness, nullptr, 50, 150},
	{"contrast", &_xr_config.contrast, nullptr, 50, 150},
	{"sharpening", &_xr_config.sharpening, nullptr, 0, 2},
	{"passthrough", &_xr_config.passthrough, nullptr, 0, 100},
	{"map_speed", &_xr_config.map_speed, nullptr, 0, 2},
	{"invert_zoom", nullptr, &_xr_config.invert_zoom, 0, 1},
	{"smart_scroll", nullptr, &_xr_config.smart_scroll, 0, 1},
};

void XrConfigLoad()
{
	if (_xr_config_path.empty()) return;
	std::ifstream in(_xr_config_path);
	std::string line;
	while (std::getline(in, line)) {
		const auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		const std::string key = line.substr(0, eq);
		const std::string text = line.substr(eq + 1);
		int value = 0;
		if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) continue;
		for (const XrConfigItem &item : _xr_config_items) {
			if (key != item.name) continue;
			value = std::clamp(value, item.min, item.max);
			if (item.value != nullptr) *item.value = value;
			if (item.flag != nullptr) *item.flag = value != 0;
		}
	}
}

void XrConfigSave()
{
	if (_xr_config_path.empty()) return;
	std::ofstream out(_xr_config_path, std::ios::trunc);
	out << "# XR-OpenTTD: VR-Einstellungen (werden im Spiel über das VR-Menü geändert)\n";
	for (const XrConfigItem &item : _xr_config_items) {
		out << item.name << '=' << (item.value != nullptr ? *item.value : (*item.flag ? 1 : 0)) << '\n';
	}
}
