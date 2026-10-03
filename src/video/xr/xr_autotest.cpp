/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file xr_autotest.cpp Unattended device test of the Meta Quest build.
 *
 * Runs only when the file "xr_autotest" exists in the app's files directory at start
 * (tools/quest-smoke-test.sh pushes it; the file is removed right away). It walks through
 * the VR tab, changes and restores VR settings, starts a new game, checks music and takes
 * raw screenshots, logging every step with the prefix "XR-AUTOTEST", then quits without saving.
 */

#include "../../stdafx.h"
#include "../../openttd.h"
#include "../../debug.h"
#include "../../gfx_func.h"
#include "../../window_func.h"
#include "../../window_gui.h"
#include "../../screenshot.h"
#include "../../genworld.h"
#include "../../progress.h"
#include "../../settings_type.h"
#include "../../base_media_base.h"
#include "../../base_media_music.h"
#include "../../music/music_driver.hpp"
#include "../../zoom_func.h"
#include "xr_config.h"
#include "xr_settings_gui.h"
#include "xr_autotest.h"

#include <chrono>

#include "../../safeguards.h"

static bool _xr_autotest = false;

void XrAutotestEnable()
{
	_xr_autotest = true;
}

static void Report(std::string_view step)
{
	const MusicSet *music = BaseMusic::GetUsedSet();
	MusicDriver *driver = MusicDriver::GetInstance();
	Debug(misc, 0, "XR-AUTOTEST {}: mode={} screen={}x{} gui_scale={} music_set={} music_driver={} song_playing={} music_on={} music_vol={} cursor={} width_mm={} height_mm={}",
		step, static_cast<int>(_game_mode), _screen.width, _screen.height, _gui_scale,
		music != nullptr ? music->name : "(none)", driver != nullptr ? driver->GetName() : "(none)",
		driver != nullptr && driver->IsSongPlaying(), _settings_client.music.playing, _settings_client.music.music_vol,
		GetXrConfig().cursor_size, GetXrConfig().panel_width_mm, GetXrConfig().height_mm);
}

static void Screenshot(std::string_view name)
{
	const bool ok = MakeScreenshot(SC_CRASHLOG, fmt::format("xr_autotest_{}", name));
	Debug(misc, 0, "XR-AUTOTEST screenshot {}: {}", name, ok ? "ok" : "FAILED");
}

void XrAutotestStep()
{
	if (!_xr_autotest) return;

	using clock = std::chrono::steady_clock;
	static clock::time_point last = clock::now();
	static int step = 0;
	static XrConfig saved;

	/*
	 * Each step waits the given seconds after the previous one (world generation may take longer
	 * on a slow start, so the waits are relative); modal progress is always awaited.
	 */
	static const double waits[] = {4, 2, 2, 2, 2, 2, 8, 2, 2, 2, 2};
	if (step >= static_cast<int>(std::size(waits)) || HasModalProgress()) return;
	if (std::chrono::duration<double>(clock::now() - last).count() < waits[step]) return;
	last = clock::now();

	switch (step) {
		case 0:
			/* The test must not change the player's openttd.cfg; quitting via _exit_game never saves the game. */
			_save_config = false;
			saved = GetXrConfig();
			Report("start");
			Screenshot("1_intro");
			break;
		case 1:
			ShowGameOptionsXrTab();
			Report("vr_tab_opened");
			break;
		case 2: {
			const Window *w = FindWindowById(WC_GAME_OPTIONS, WN_GAME_OPTIONS_GAME_OPTIONS);
			Debug(misc, 0, "XR-AUTOTEST vr_tab window={} lowered={} size={}x{}", w != nullptr, w != nullptr && w->IsWidgetLowered(WID_GO_TAB_XR),
				w != nullptr ? w->width : 0, w != nullptr ? w->height : 0);
			for (WidgetID id = WID_GO_XR_FIRST; id < WID_XRS_HELP; ++id) {
				if (auto text = XrSettingsWidgetString(id); text.has_value()) Debug(misc, 0, "XR-AUTOTEST widget {}: {}", id, *text);
			}
			Screenshot("2_vr_tab");
			break;
		}
		case 3:
			/* Change settings through the same path as the dropdowns: large arrow, wide screen, raised. */
			XrSettingsDropdownSelect(WID_XRS_CURSOR, 3);       // large arrow
			XrSettingsDropdownSelect(WID_XRS_PANEL_WIDTH, 4);  // 2.8 m
			XrSettingsDropdownSelect(WID_XRS_HEIGHT, 3);       // +20 cm
			if (Window *w = FindWindowById(WC_GAME_OPTIONS, WN_GAME_OPTIONS_GAME_OPTIONS); w != nullptr) w->SetDirty();
			Report("settings_changed");
			break;
		case 4: {
			Screenshot("3_settings_changed");
			/* Restore the stored values; a new size request makes the driver apply them at once. */
			const unsigned request = GetXrConfig().size_request;
			GetXrConfig() = saved;
			GetXrConfig().size_request = request + 1;
			XrConfigSave();
			Report("settings_restored");
			break;
		}
		case 5:
			CloseWindowByClass(WC_GAME_OPTIONS);
			Report("new_game");
			StartNewGameWithoutGUI(12345);
			break;
		case 6:
			Report("in_game");
			Screenshot("4_game");
			break;
		case 7:
			ShowGameOptionsXrTab();
			break;
		case 8:
			Screenshot("5_game_vr_tab");
			CloseWindowByClass(WC_GAME_OPTIONS);
			break;
		case 9:
			Report("end");
			break;
		case 10:
			Debug(misc, 0, "XR-AUTOTEST DONE");
			_exit_game = true;
			break;
	}
	++step;
}
