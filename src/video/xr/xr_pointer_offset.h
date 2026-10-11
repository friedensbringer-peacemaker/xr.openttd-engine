/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file xr_pointer_offset.h Pointer offset X/Y of the VR menu ("Zeiger waagerecht/senkrecht").
 *
 * The player shifts the game's mouse position against the laser until the game's arrow sits exactly under
 * the ray. Values in percent of the image width/height, raster 0.25 %, range +-15 %, positive = right/down.
 * Applied to the laser hit UV before it is mapped to OpenTTD mouse coordinates, clamped to the image.
 *
 * Project copy of xr-common `xrc/menu.h` namespace `xrc::ptroff` (origin XRShell `pointer_map.h`).
 * TODO xrc/menu.h: replace by the shared header once xr-common is tagged and vendored here.
 * Header-only and free of OpenTTD dependencies, so tools/tests/xr_logic_test.cpp can test it on the host.
 */

#ifndef VIDEO_XR_POINTER_OFFSET_H
#define VIDEO_XR_POINTER_OFFSET_H

#include <cmath>
#include <cstdlib>

namespace ptroff {

constexpr float kStep = 0.25f;  ///< Raster in percent.
constexpr float kMax = 15.0f;   ///< Largest offset in percent (both directions).
constexpr int kMaxSteps = 60;   ///< kMax / kStep: xr.cfg stores the offset as whole raster steps.

/** Round to the raster and clamp; NaN/Inf become 0. */
inline float clampPercent(float p)
{
	if (!std::isfinite(p)) return 0.0f;
	p = std::round(p / kStep) * kStep;
	if (p == 0.0f) p = 0.0f; // no "-0"
	return p < -kMax ? -kMax : p > kMax ? kMax : p;
}

/** One step with < (step = -1) or > (step = +1). */
inline float stepPercent(float p, int step) { return clampPercent(p + kStep * static_cast<float>(step)); }

/** Laser UV (0..1) -> shifted UV for the game, clamped to the image. */
inline float apply(float uv, float percent)
{
	const float v = uv + clampPercent(percent) / 100.0f;
	return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/** Stored raster steps (xr.cfg) <-> percent. */
inline int clampSteps(int steps) { return steps < -kMaxSteps ? -kMaxSteps : steps > kMaxSteps ? kMaxSteps : steps; }
inline float stepsToPercent(int steps) { return static_cast<float>(clampSteps(steps)) * kStep; }
inline int percentToSteps(float p) { return static_cast<int>(std::lround(clampPercent(p) / kStep)); }

/** Display: absolute value in hundredths of a percent (1.25 % -> 125) and the sign (-1, 0, +1). */
inline int displayHundredths(int steps) { return std::abs(clampSteps(steps)) * 25; }
inline int displaySign(int steps) { return clampSteps(steps) > 0 ? 1 : clampSteps(steps) < 0 ? -1 : 0; }

/**
 * Hold-to-repeat for the < > buttons (VR menu P2): while the trigger stays down on the same button, the first
 * repeat comes after 0.45 s, then one every 0.12 s. Buttons are identified by a non-negative number.
 */
struct HoldRepeat {
	static constexpr unsigned kFirstMs = 450;
	static constexpr unsigned kNextMs = 120;
	int button = -1;    ///< Held button, -1 = none.
	unsigned wait = 0;  ///< Milliseconds until the next repeat.

	void start(int b) { this->button = b; this->wait = kFirstMs; }
	void stop() { this->button = -1; }
	bool active() const { return this->button >= 0; }

	/** Advance by delta_ms; held = still pressed on the button. Returns the number of repeat steps now due. */
	int advance(bool held, unsigned delta_ms)
	{
		if (this->button < 0) return 0;
		if (!held) {
			this->stop();
			return 0;
		}
		int steps = 0;
		while (delta_ms >= this->wait) {
			delta_ms -= this->wait;
			this->wait = kNextMs;
			++steps;
		}
		this->wait -= delta_ms;
		return steps;
	}
};

} // namespace ptroff

#endif /* VIDEO_XR_POINTER_OFFSET_H */
