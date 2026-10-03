/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file xr_android_main.cpp Entry point of the Meta Quest build (XR-OpenTTD).
 *
 * SDLActivity loads libmain.so and calls SDL_main on its own thread. Before OpenTTD starts,
 * the game data packed in the APK (language files, AI/GS libraries, free base sets) is copied
 * to the app's external files directory, which becomes the working and personal directory.
 * Players can add their own original TTD files to baseset/ there via adb.
 */

#include "../../stdafx.h"
#include "../../openttd.h"
#include "../../core/random_func.hpp"
#include "../../string_func.h"
#include "xr_config.h"
#include "xr_autotest.h"

#include <SDL.h>
#include <SDL_system.h>
#include <android/log.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <signal.h>
#include <unistd.h>

#include "../../safeguards.h"

/** Forward stdout/stderr (OpenTTD's debug output) to logcat, tag "OpenTTD". */
static void RedirectOutputToLogcat()
{
	static int pipe_fds[2];
	setvbuf(stdout, nullptr, _IOLBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
	if (pipe(pipe_fds) != 0) return;
	dup2(pipe_fds[1], STDOUT_FILENO);
	dup2(pipe_fds[1], STDERR_FILENO);
	std::thread([] {
		std::string line;
		char buffer[512];
		ssize_t n;
		while ((n = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
			for (ssize_t i = 0; i < n; ++i) {
				if (buffer[i] == '\n') {
					__android_log_write(ANDROID_LOG_INFO, "OpenTTD", line.c_str());
					line.clear();
				} else {
					line += buffer[i];
				}
			}
		}
	}).detach();
	/* OpenTTD ends fatal errors with exit(): give the reader thread time to forward the message. */
	atexit([] {
		fflush(stdout);
		fflush(stderr);
		usleep(300000);
	});
}

/** Read a whole file from the APK assets (SDL resolves relative paths to assets on Android). */
static bool ReadAsset(const std::string &name, std::vector<char> &data)
{
	SDL_RWops *rw = SDL_RWFromFile(name.c_str(), "rb");
	if (rw == nullptr) {
		__android_log_print(ANDROID_LOG_ERROR, "OpenTTD", "Asset %s: %s", name.c_str(), SDL_GetError());
		return false;
	}
	const Sint64 size = SDL_RWsize(rw);
	data.resize(size > 0 ? static_cast<size_t>(size) : 0);
	/* Compressed APK assets are delivered in chunks: read until the whole file is there. */
	size_t done = 0;
	while (size >= 0 && done < data.size()) {
		const size_t n = SDL_RWread(rw, data.data() + done, 1, data.size() - done);
		if (n == 0) break;
		done += n;
	}
	SDL_RWclose(rw);
	if (size < 0 || done != data.size()) {
		__android_log_print(ANDROID_LOG_ERROR, "OpenTTD", "Asset %s: read %zu of %lld bytes", name.c_str(), done, static_cast<long long>(size));
		return false;
	}
	return true;
}

/** Size of an APK asset, or -1 if it is missing. */
static std::uintmax_t AssetSize(const std::string &name)
{
	SDL_RWops *rw = SDL_RWFromFile(name.c_str(), "rb");
	if (rw == nullptr) return static_cast<std::uintmax_t>(-1);
	const Sint64 size = SDL_RWsize(rw);
	SDL_RWclose(rw);
	return size < 0 ? static_cast<std::uintmax_t>(-1) : static_cast<std::uintmax_t>(size);
}

/**
 * Copy the game data from the APK once per build. The asset "xr_data.lst" lists a version
 * line and then every file below "xr_data/".
 */
static void ExtractGameData(const std::filesystem::path &base)
{
	std::vector<char> list;
	if (!ReadAsset("xr_data.lst", list)) {
		__android_log_write(ANDROID_LOG_ERROR, "OpenTTD", "xr_data.lst missing in the APK");
		return;
	}
	std::string text(list.begin(), list.end());
	const std::string version = text.substr(0, text.find('\n'));

	const std::filesystem::path stamp = base / ".xr_data_version";
	std::string installed;
	std::getline(std::ifstream(stamp) >> std::ws, installed);
	if (installed == version) return;

	__android_log_print(ANDROID_LOG_INFO, "OpenTTD", "Extracting game data %s", version.c_str());
	size_t pos = text.find('\n');
	std::vector<char> data;
	int failed = 0;
	while (pos != std::string::npos && pos + 1 < text.size()) {
		const size_t next = text.find('\n', pos + 1);
		std::string rel = text.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1);
		pos = next;
		while (!rel.empty() && (rel.back() == '\r' || rel.back() == ' ')) rel.pop_back();
		if (rel.empty()) continue;
		const std::filesystem::path target = base / rel;
		/* Base set archives and the SoundFont never change within a release: keep an intact copy. */
		const bool large_static = rel.starts_with("soundfont/") || rel.ends_with(".tar");
		std::error_code size_ec;
		if (large_static && std::filesystem::file_size(target, size_ec) == AssetSize("xr_data/" + rel) && !size_ec) continue;
		if (!ReadAsset("xr_data/" + rel, data)) {
			++failed;
			continue;
		}
		std::error_code ec;
		std::filesystem::create_directories(target.parent_path(), ec);
		std::ofstream out(target, std::ios::binary | std::ios::trunc);
		out.write(data.data(), static_cast<std::streamsize>(data.size()));
		if (!out) {
			__android_log_print(ANDROID_LOG_ERROR, "OpenTTD", "Cannot write %s", target.c_str());
			++failed;
		}
	}
	/* Only a complete copy is marked as installed; otherwise the next start tries again. */
	if (failed == 0) {
		std::ofstream(stamp, std::ios::trunc) << version << '\n';
	} else {
		__android_log_print(ANDROID_LOG_ERROR, "OpenTTD", "%d game data files could not be copied", failed);
	}
}

/** First start: settings that make the game readable and usable with a controller ray. */
static void WriteDefaultConfig(const std::filesystem::path &file)
{
	if (std::filesystem::exists(file)) return;
	std::ofstream out(file);
	out << "[misc]\n"
		"graphicsset = OpenGFX\n"
		"soundset = OpenSFX\n"
		"musicset = OpenMSX\n"
		"resolution = 1920,1080\n"
		"gui_scale = 200\n"
		"video_vsync = true\n"
		"\n"
		"[gui]\n"
		"osk_activation = immediately\n"
		"autosave_on_exit = true\n"
		"hover_delay_ms = 1000\n";
}

extern "C" int SDL_main(int, char *[])
{
	RedirectOutputToLogcat();

	const char *external = SDL_AndroidGetExternalStoragePath();
	if (external == nullptr) {
		__android_log_write(ANDROID_LOG_ERROR, "OpenTTD", "No external storage path");
		return 1;
	}
	const std::filesystem::path base = external;
	std::error_code ec;
	std::filesystem::create_directories(base, ec);

	ExtractGameData(base);
	const std::filesystem::path config = base / "openttd.cfg";
	WriteDefaultConfig(config);
	XrConfigSetPath((base / "xr.cfg").string());

	/* Unattended device test (tools/quest-smoke-test.sh): runs once per pushed flag file. */
	if (std::filesystem::remove(base / "xr_autotest", ec)) {
		__android_log_write(ANDROID_LOG_INFO, "OpenTTD", "XR-AUTOTEST enabled");
		XrAutotestEnable();
	}

	/* Working directory = data, personal directory = <base>/openttd (saves, screenshots, downloads). */
	setenv("HOME", base.c_str(), 1);
	setenv("XDG_DATA_HOME", base.c_str(), 1);
	setenv("XDG_CONFIG_HOME", base.c_str(), 1);
	if (chdir(base.c_str()) != 0) __android_log_write(ANDROID_LOG_ERROR, "OpenTTD", "chdir failed");

	SetRandomSeed(static_cast<uint32_t>(time(nullptr)));
	signal(SIGPIPE, SIG_IGN);

	std::string config_arg = config.string();
	/* Music: FluidSynth renders the MIDI songs (OpenMSX) with the bundled General MIDI SoundFont. */
	const std::filesystem::path soundfont = base / "soundfont" / "GeneralUser-GS.sf2";
	std::string music_arg = std::filesystem::exists(soundfont) ? "fluidsynth:soundfont=" + soundfont.string() : "null";
	std::vector<std::string_view> params = {"openttd", "-c", config_arg, "-b", "32bpp-anim", "-v", "xr", "-s", "sdl", "-m", music_arg, "-d", "driver=1,misc=1"};
	const int result = openttd_main(params);

	/* SDL2 keeps the process alive after SDL_main returns; a restart must not reuse old globals. */
	_exit(result);
}
