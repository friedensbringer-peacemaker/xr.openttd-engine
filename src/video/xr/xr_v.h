/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file xr_v.h OpenXR video driver for the Meta Quest (XR-OpenTTD).
 *
 * The game draws unchanged into a 32bpp buffer. The driver shows that buffer as a (curved)
 * screen in the room via an OpenXR composition layer, and turns the controller ray into the
 * mouse. Architecture and controls follow XR-Settlers2.5 (see its VideoOpenXR driver).
 */

#ifndef VIDEO_XR_H
#define VIDEO_XR_H

#include "../video_driver.hpp"

#include <memory>

/** The OpenXR video driver. */
class VideoDriver_XR : public VideoDriver {
public:
	VideoDriver_XR();
	~VideoDriver_XR() override;

	std::optional<std::string_view> Start(const StringList &param) override;
	void Stop() override;
	void MakeDirty(int left, int top, int width, int height) override;
	void MainLoop() override;
	bool ChangeResolution(int w, int h) override;
	bool ToggleFullscreen(bool fullscreen) override;
	bool AfterBlitterChange() override;
	bool ClaimMousePointer() override { return true; }
	/** The headset paces every frame via xrWaitFrame; the option must not switch that off. */
	void ToggleVsync(bool) override { _video_vsync = true; }
	bool HasEfficient8Bpp() const override { return false; }
	std::string_view GetName() const override { return "xr"; }
	std::string_view GetInfoString() const override { return "xr (OpenXR, Meta Quest)"; }

protected:
	Dimension GetScreenSize() const override;
	void InputLoop() override;
	bool LockVideoBuffer() override;
	void UnlockVideoBuffer() override;
	void Paint() override;
	void CheckPaletteAnim() override;
	bool PollEvent() override;

private:
	struct State;
	std::unique_ptr<State> xr;

	std::optional<std::string_view> InitXr();
	void ShutdownXr();
	void AllocateBackingStore(int w, int h);
	void PollXrEvents();
	void ProcessControllers();
	void ReleaseInput();
	bool Recenter(bool reset_size);
	void UpdatePassthrough(int percent);
	void RenderFrame();
};

/** Factory for the OpenXR video driver. */
class FVideoDriver_XR : public DriverFactoryBase {
public:
	FVideoDriver_XR() : DriverFactoryBase(Driver::DT_VIDEO, 20, "xr", "OpenXR Video Driver (Meta Quest)") {}
	std::unique_ptr<Driver> CreateInstance() const override { return std::make_unique<VideoDriver_XR>(); }
};

#endif /* VIDEO_XR_H */
