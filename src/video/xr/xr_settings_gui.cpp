/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file xr_settings_gui.cpp The "VR" tab of the game options window (Meta Quest build). */

#include "../../stdafx.h"
#include "../../window_gui.h"
#include "../../window_func.h"
#include "../../gfx_func.h"
#include "../../strings_func.h"
#include "../../zoom_func.h"
#include "../../dropdown_func.h"
#include "../../dropdown_type.h"
#include "xr_config.h"
#include "xr_settings_gui.h"
#include "../../openttd.h"
#include "../../command_func.h"
#include "../../misc_cmd.h"
#include "../../network/network.h"
#include "../../saveload/saveload.h"
#include "../../settings_type.h"
#include "../../fios.h"
#include "../../palette_func.h"

#include "table/strings.h"

#include "../../safeguards.h"


static const int _xr_gui_scales[] = {150, 175, 200, 250, 300};
static const int _xr_percent_steps[] = {70, 80, 90, 100, 110, 120, 130};
static const int _xr_widths_mm[] = {1200, 1600, 2000, 2400, 2800, 3200};
static const int _xr_distances_mm[] = {1000, 1400, 1800, 2200, 2600, 3000};
static const int _xr_heights_mm[] = {-400, -200, 0, 200, 400};

/** Label shown in front of the value of a dropdown setting. */
static StringID XrLabel(WidgetID widget)
{
	switch (widget) {
		case WID_XRS_GUI_SCALE: return STR_XR_GUI_SCALE;
		case WID_XRS_PANEL_WIDTH: return STR_XR_PANEL_WIDTH;
		case WID_XRS_DISTANCE: return STR_XR_DISTANCE;
		case WID_XRS_HEIGHT: return STR_XR_HEIGHT;
		case WID_XRS_CURVATURE: return STR_XR_CURVATURE;
		case WID_XRS_BRIGHTNESS: return STR_XR_BRIGHTNESS;
		case WID_XRS_CONTRAST: return STR_XR_CONTRAST;
		case WID_XRS_SHARPENING: return STR_XR_SHARPENING;
		case WID_XRS_PASSTHROUGH: return STR_XR_PASSTHROUGH;
		case WID_XRS_POINTER_HAND: return STR_XR_POINTER_HAND;
		case WID_XRS_CURSOR: return STR_XR_CURSOR;
		case WID_XRS_SMOOTHING: return STR_XR_SMOOTHING;
		case WID_XRS_STABILISATION: return STR_XR_STABILISATION;
		case WID_XRS_HAPTICS: return STR_XR_HAPTICS;
		case WID_XRS_RAY_WIDTH: return STR_XR_RAY_WIDTH;
		case WID_XRS_RAY_COLOUR: return STR_XR_RAY_COLOUR;
		case WID_XRS_MARKER: return STR_XR_MARKER;
		case WID_XRS_MAP_SPEED: return STR_XR_MAP_SPEED;
		case WID_XRS_ZOOM_DIRECTION: return STR_XR_ZOOM_DIRECTION;
		case WID_XRS_SMART_SCROLL: return STR_XR_SMART_SCROLL;
		default: return INVALID_STRING_ID;
	}
}

/** Choices of a dropdown setting; numbers are formatted in the current language. */
static std::vector<std::string> XrChoices(WidgetID widget)
{
	auto strings = [](std::initializer_list<StringID> ids) {
		std::vector<std::string> result;
		for (StringID id : ids) result.push_back(GetString(id));
		return result;
	};
	auto numbers = [](std::span<const int> values, auto format) {
		std::vector<std::string> result;
		for (int v : values) result.push_back(format(v));
		return result;
	};
	auto percent = [](int v) { return GetString(STR_XR_PERCENT, v); };
	auto metres = [](int mm) { return GetString(STR_XR_METRES, mm / 100, 1); };
	auto centimetres = [](int mm) { return GetString(STR_XR_CENTIMETRES, mm / 10); };
	switch (widget) {
		case WID_XRS_GUI_SCALE: return numbers(_xr_gui_scales, percent);
		case WID_XRS_PANEL_WIDTH: return numbers(_xr_widths_mm, metres);
		case WID_XRS_DISTANCE: return numbers(_xr_distances_mm, metres);
		case WID_XRS_HEIGHT: return numbers(_xr_heights_mm, centimetres);
		case WID_XRS_CURVATURE: return strings({STR_XR_FLAT, STR_XR_GENTLE, STR_XR_CURVED});
		case WID_XRS_BRIGHTNESS:
		case WID_XRS_CONTRAST: return numbers(_xr_percent_steps, percent);
		case WID_XRS_POINTER_HAND: return strings({STR_XR_LEFT, STR_XR_RIGHT});
		case WID_XRS_SHARPENING: return strings({STR_XR_OFF, STR_XR_SUPERSAMPLING, STR_XR_SHARPEN});
		case WID_XRS_CURSOR: return strings({STR_XR_OFF, STR_XR_SMALL, STR_XR_NORMAL, STR_XR_LARGE});
		case WID_XRS_SMOOTHING: return strings({STR_XR_OFF, STR_XR_LIGHT, STR_XR_STRONG});
		case WID_XRS_STABILISATION:
		case WID_XRS_HAPTICS:
		case WID_XRS_SMART_SCROLL: return strings({STR_XR_OFF, STR_XR_ON});
		case WID_XRS_RAY_WIDTH: return strings({STR_XR_THIN, STR_XR_MEDIUM, STR_XR_THICK, STR_XR_OFF});
		case WID_XRS_PASSTHROUGH:
			if (!GetXrConfig().passthrough_available) return {GetString(STR_XR_UNAVAILABLE)};
			return {GetString(STR_XR_OFF), percent(25), percent(50), percent(75), percent(100)};
		case WID_XRS_RAY_COLOUR: return strings({STR_XR_WHITE, STR_XR_GREY, STR_XR_DARK, STR_XR_AMBER});
		case WID_XRS_MARKER: return strings({STR_XR_SMALL, STR_XR_MEDIUM, STR_XR_LARGE, STR_XR_OFF});
		case WID_XRS_MAP_SPEED: return {GetString(STR_XR_TIMES, 1), GetString(STR_XR_TIMES, 2), GetString(STR_XR_TIMES, 4)};
		case WID_XRS_ZOOM_DIRECTION: return strings({STR_XR_ZOOM_NORMAL, STR_XR_ZOOM_INVERTED});
		default: return {};
	}
}

static DropDownList XrDropDownList(WidgetID widget)
{
	DropDownList list;
	int i = 0;
	for (std::string &choice : XrChoices(widget)) list.push_back(MakeDropDownListStringItem(std::move(choice), i++));
	return list;
}

/** Index of the nearest entry in a list of values. */
static int NearestIndex(std::span<const int> values, int value)
{
	int best = 0;
	for (int i = 1; i < static_cast<int>(values.size()); ++i) {
		if (std::abs(values[i] - value) < std::abs(values[best] - value)) best = i;
	}
	return best;
}

/** Currently selected choice of a dropdown setting. */
static int XrCurrent(WidgetID widget)
{
	const XrConfig &c = GetXrConfig();
	switch (widget) {
		case WID_XRS_GUI_SCALE: return NearestIndex(_xr_gui_scales, _gui_scale);
		case WID_XRS_PANEL_WIDTH: return NearestIndex(_xr_widths_mm, c.panel_width_mm);
		case WID_XRS_DISTANCE: return NearestIndex(_xr_distances_mm, c.distance_mm);
		case WID_XRS_HEIGHT: return NearestIndex(_xr_heights_mm, c.height_mm);
		case WID_XRS_CURVATURE: return c.curvature;
		case WID_XRS_BRIGHTNESS: return NearestIndex(_xr_percent_steps, c.brightness);
		case WID_XRS_CONTRAST: return NearestIndex(_xr_percent_steps, c.contrast);
		case WID_XRS_SHARPENING: return c.sharpening;
		case WID_XRS_PASSTHROUGH: return c.passthrough_available ? c.passthrough / 25 : 0;
		case WID_XRS_POINTER_HAND: return c.pointer_hand;
		case WID_XRS_CURSOR: return c.cursor_size;
		case WID_XRS_SMOOTHING: return c.smoothing;
		case WID_XRS_STABILISATION: return c.click_stabilization ? 1 : 0;
		case WID_XRS_HAPTICS: return c.haptics ? 1 : 0;
		case WID_XRS_SMART_SCROLL: return c.smart_scroll ? 1 : 0;
		case WID_XRS_RAY_WIDTH: return c.ray_width;
		case WID_XRS_RAY_COLOUR: return c.ray_color;
		case WID_XRS_MARKER: return c.marker_size;
		case WID_XRS_MAP_SPEED: return c.map_speed;
		case WID_XRS_ZOOM_DIRECTION: return c.invert_zoom ? 1 : 0;
		default: return 0;
	}
}

/** Apply a dropdown choice; VR values are saved right away. */
static void XrApply(WidgetID widget, int index)
{
	XrConfig &c = GetXrConfig();
	switch (widget) {
		case WID_XRS_GUI_SCALE:
			/* Same as the interface size slider in the game options (stored in openttd.cfg). */
			_gui_scale_cfg = _xr_gui_scales[index];
			if (AdjustGUIZoom(false)) ReInitAllWindows(true);
			return;
		case WID_XRS_PANEL_WIDTH: c.panel_width_mm = _xr_widths_mm[index]; c.size_request++; break;
		case WID_XRS_DISTANCE: c.distance_mm = _xr_distances_mm[index]; c.size_request++; break;
		case WID_XRS_HEIGHT: c.height_mm = _xr_heights_mm[index]; break;
		case WID_XRS_CURVATURE: c.curvature = index; break;
		case WID_XRS_BRIGHTNESS: c.brightness = _xr_percent_steps[index]; break;
		case WID_XRS_CONTRAST: c.contrast = _xr_percent_steps[index]; break;
		case WID_XRS_SHARPENING: c.sharpening = index; break;
		case WID_XRS_PASSTHROUGH:
			if (!c.passthrough_available) return;
			c.passthrough = index * 25;
			break;
		case WID_XRS_POINTER_HAND: c.pointer_hand = index; break;
		case WID_XRS_CURSOR: c.cursor_size = index; break;
		case WID_XRS_SMOOTHING: c.smoothing = index; break;
		case WID_XRS_STABILISATION: c.click_stabilization = index != 0; break;
		case WID_XRS_HAPTICS: c.haptics = index != 0; break;
		case WID_XRS_SMART_SCROLL: c.smart_scroll = index != 0; break;
		case WID_XRS_RAY_WIDTH: c.ray_width = index; break;
		case WID_XRS_RAY_COLOUR: c.ray_color = index; break;
		case WID_XRS_MARKER: c.marker_size = index; break;
		case WID_XRS_MAP_SPEED: c.map_speed = index; break;
		case WID_XRS_ZOOM_DIRECTION: c.invert_zoom = index != 0; break;
		default: return;
	}
	XrConfigSave();
}

/* Same look as the other tabs of the game options (settings_gui.cpp). */
static constexpr Colours XR_BACKGROUND = COLOUR_MAUVE;
static constexpr Colours XR_BUTTON = COLOUR_YELLOW;
static constexpr TextColour XR_FRAME = TC_ORANGE;

std::optional<std::string> XrSettingsWidgetString(WidgetID widget)
{
	/* Each setting is one wide button "Label: value" – large target for the ray, nothing to misalign. */
	const std::vector<std::string> choices = XrChoices(widget);
	if (choices.empty()) return std::nullopt;
	return GetString(XrLabel(widget)) + ": " + choices[std::clamp(XrCurrent(widget), 0, static_cast<int>(choices.size()) - 1)];
}

void XrSettingsUpdateSize(WidgetID widget, Dimension &size, const Dimension &padding)
{
	if (widget == WID_XRS_HELP) {
		size.width = std::max<uint>(size.width, ScaleGUITrad(300));
		size.height = std::max<uint>(size.height, GetStringHeight(STR_XR_CONTROLS_HELP, size.width));
		return;
	}
	/*
	 * Plain buttons (recenter, default size) have no label/choices: their size comes from the widget text.
	 * GetString(INVALID_STRING_ID) is a fatal error in OpenTTD — it crashed every opening of the game options.
	 */
	const StringID label_id = XrLabel(widget);
	if (label_id == INVALID_STRING_ID) return;
	/* Wide enough for the label with the longest choice, plus the dropdown arrow. */
	const std::string label = GetString(label_id) + ": ";
	for (const std::string &choice : XrChoices(widget)) {
		const uint width = GetStringBoundingBox(label + choice).width + padding.width + GetCharacterHeight(FS_NORMAL) * 2;
		size.width = std::max(size.width, width);
	}
}

void XrSettingsDraw(const Rect &r, WidgetID widget)
{
	if (widget == WID_XRS_HELP) DrawStringMultiLine(r, STR_XR_CONTROLS_HELP, TC_WHITE);
}

void XrSettingsClick(Window *w, WidgetID widget)
{
	switch (widget) {
		case WID_XRS_RECENTER:
			GetXrConfig().recenter_request++;
			return;
		case WID_XRS_RESET_SIZE:
			GetXrConfig().reset_request++;
			return;
		case WID_XRS_CLOSE:
			w->Close();
			return;
		default: break;
	}
	DropDownList list = XrDropDownList(widget);
	if (!list.empty()) ShowDropDownList(w, std::move(list), XrCurrent(widget), widget);
}

void XrSettingsDropdownSelect(WidgetID widget, int index)
{
	XrApply(widget, index);
}

/** One setting: a full-width dropdown button showing "Label: value". */
#define XR_ROW(widget) NWidget(WWT_DROPDOWN, XR_BUTTON, widget), SetFill(1, 0), SetResize(1, 0)

static constexpr std::initializer_list<NWidgetPart> _nested_xr_settings_pane = {
	NWidget(NWID_HORIZONTAL), SetPadding(WidgetDimensions::unscaled.sparse_resize), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
		/* Left column: screen and controls. */
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_wide, 0),
			NWidget(WWT_FRAME, XR_BACKGROUND), SetStringTip(STR_XR_FRAME_SCREEN), SetTextStyle(XR_FRAME), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				XR_ROW(WID_XRS_GUI_SCALE),
				XR_ROW(WID_XRS_PANEL_WIDTH),
				XR_ROW(WID_XRS_DISTANCE),
				XR_ROW(WID_XRS_HEIGHT),
				XR_ROW(WID_XRS_CURVATURE),
				XR_ROW(WID_XRS_BRIGHTNESS),
				XR_ROW(WID_XRS_CONTRAST),
				XR_ROW(WID_XRS_SHARPENING),
				XR_ROW(WID_XRS_PASSTHROUGH),
				NWidget(WWT_PUSHTXTBTN, XR_BUTTON, WID_XRS_RECENTER), SetStringTip(STR_XR_RECENTER), SetFill(1, 0), SetResize(1, 0),
				NWidget(WWT_PUSHTXTBTN, XR_BUTTON, WID_XRS_RESET_SIZE), SetStringTip(STR_XR_RESET_SIZE), SetFill(1, 0), SetResize(1, 0),
			EndContainer(),
			NWidget(WWT_FRAME, XR_BACKGROUND), SetStringTip(STR_XR_FRAME_CONTROLS), SetTextStyle(XR_FRAME), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				XR_ROW(WID_XRS_MAP_SPEED),
				XR_ROW(WID_XRS_ZOOM_DIRECTION),
				XR_ROW(WID_XRS_SMART_SCROLL),
			EndContainer(),
			NWidget(NWID_SPACER), SetFill(1, 1), SetResize(1, 1),
		EndContainer(),
		/* Right column: pointer and controller help. */
		NWidget(NWID_VERTICAL), SetPIP(0, WidgetDimensions::unscaled.vsep_wide, 0),
			NWidget(WWT_FRAME, XR_BACKGROUND), SetStringTip(STR_XR_FRAME_POINTER), SetTextStyle(XR_FRAME), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
				XR_ROW(WID_XRS_CURSOR),
				XR_ROW(WID_XRS_POINTER_HAND),
				XR_ROW(WID_XRS_SMOOTHING),
				XR_ROW(WID_XRS_STABILISATION),
				XR_ROW(WID_XRS_HAPTICS),
				XR_ROW(WID_XRS_RAY_WIDTH),
				XR_ROW(WID_XRS_RAY_COLOUR),
				XR_ROW(WID_XRS_MARKER),
			EndContainer(),
			NWidget(WWT_FRAME, XR_BACKGROUND), SetStringTip(STR_XR_FRAME_BUTTONS), SetTextStyle(XR_FRAME),
				NWidget(WWT_EMPTY, INVALID_COLOUR, WID_XRS_HELP), SetFill(1, 0), SetResize(1, 0),
			EndContainer(),
			NWidget(NWID_SPACER), SetFill(1, 1), SetResize(1, 1),
		EndContainer(),
	EndContainer(),
	/* Every VR page ends with "Close". */
	NWidget(NWID_HORIZONTAL), SetPadding(0, WidgetDimensions::unscaled.sparse_resize.right, WidgetDimensions::unscaled.sparse_resize.bottom, WidgetDimensions::unscaled.sparse_resize.left),
		NWidget(WWT_PUSHTXTBTN, XR_BUTTON, WID_XRS_CLOSE), SetStringTip(STR_XR_CLOSE), SetMinimalTextLines(2, 0), SetFill(1, 0), SetResize(1, 0),
	EndContainer(),
};

#undef XR_ROW

std::unique_ptr<NWidgetBase> MakeXrSettingsPane()
{
	return MakeNWidgets(_nested_xr_settings_pane, std::make_unique<NWidgetVertical>());
}

/** Menu button / settings menu: open the game options on the VR tab, or close them again. */
void ShowXrSettings()
{
	if (Window *w = FindWindowById(WC_GAME_OPTIONS, WN_GAME_OPTIONS_GAME_OPTIONS); w != nullptr && w->IsWidgetLowered(WID_GO_TAB_XR)) {
		w->Close();
		return;
	}
	ShowGameOptionsXrTab();
}

/*
 * ---------------------------------------------------------------------------------------------
 * VR quick menu (left menu button): large buttons in the game look, operable by ray or by
 * stick + A, pauses a single-player game while open. Same window class as the game options
 * with its own window number, so OpenTTD's window class list stays unchanged.
 * ---------------------------------------------------------------------------------------------
 */

static const WindowNumber XR_QUICK_MENU_NUMBER = 0x5852; ///< "XR"

/** Widgets of the quick menu, in focus order. */
enum XrQuickMenuWidgets : WidgetID {
	WID_XRQ_CONTINUE,
	WID_XRQ_RECENTER,
	WID_XRQ_RAY,
	WID_XRQ_PASSTHROUGH,
	WID_XRQ_SAVE,
	WID_XRQ_SETTINGS,
	WID_XRQ_QUIT,
	WID_XRQ_COUNT,
};

struct XrQuickMenuWindow : Window {
	int focus = WID_XRQ_CONTINUE;      ///< Button selected with the stick.
	uint quit_armed_ms = 0;            ///< Remaining time of the armed "Quit" button (0 = not armed).
	bool paused_by_menu = false;       ///< The menu paused the game and resumes it when closing.

	XrQuickMenuWindow(WindowDesc &desc) : Window(desc)
	{
		this->InitNested(XR_QUICK_MENU_NUMBER);
		if (_game_mode == GM_NORMAL && !_networking && !_pause_mode.Test(PauseMode::Normal)) {
			Command<CMD_PAUSE>::Post(PauseMode::Normal, true);
			this->paused_by_menu = true;
		}
		this->OnInvalidateData();
	}

	void Close(int data = 0) override
	{
		if (this->paused_by_menu && _pause_mode.Test(PauseMode::Normal)) Command<CMD_PAUSE>::Post(PauseMode::Normal, false);
		this->Window::Close(data);
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->SetWidgetDisabledState(WID_XRQ_PASSTHROUGH, !GetXrConfig().passthrough_available);
		this->SetWidgetDisabledState(WID_XRQ_SAVE, _game_mode != GM_NORMAL || _networking);
		this->SetWidgetLoweredState(WID_XRQ_QUIT, this->quit_armed_ms > 0);
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		const XrConfig &c = GetXrConfig();
		switch (widget) {
			case WID_XRQ_RAY: return GetString(c.ray_width == 3 ? STR_XR_RAY_OFF : STR_XR_RAY_ON);
			case WID_XRQ_PASSTHROUGH:
				if (!c.passthrough_available) return GetString(STR_XR_PASSTHROUGH) + ": " + GetString(STR_XR_UNAVAILABLE);
				return GetString(c.passthrough > 0 ? STR_XR_PASSTHROUGH_ON : STR_XR_PASSTHROUGH_OFF);
			case WID_XRQ_QUIT: return GetString(this->quit_armed_ms > 0 ? STR_XR_QUIT_ARMED : STR_XR_QUIT);
			default: return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		/* Large targets for the ray; wide enough for the longest state text. */
		size.width = std::max<uint>(size.width, ScaleGUITrad(240));
		if (widget == WID_XRQ_QUIT) size.width = std::max(size.width, GetStringBoundingBox(STR_XR_QUIT_ARMED).width + padding.width);
	}

	void OnPaint() override
	{
		this->DrawWidgets();
		/* Visible focus for stick operation. */
		if (const NWidgetBase *nw = this->GetWidget<NWidgetBase>(this->focus); nw != nullptr) {
			DrawRectOutline(nw->GetCurrentRect().Expand(WidgetDimensions::scaled.bevel.left), PC_WHITE, std::max(2, ScaleGUITrad(2)));
		}
	}

	void OnRealtimeTick(uint delta_ms) override
	{
		if (this->quit_armed_ms == 0) return;
		this->quit_armed_ms = delta_ms >= this->quit_armed_ms ? 0 : this->quit_armed_ms - delta_ms;
		if (this->quit_armed_ms == 0) {
			this->OnInvalidateData();
			this->SetDirty();
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget < 0 || widget >= WID_XRQ_COUNT || this->IsWidgetDisabled(widget)) return;
		this->focus = widget;
		XrConfig &c = GetXrConfig();
		switch (widget) {
			case WID_XRQ_CONTINUE:
				this->Close();
				return;
			case WID_XRQ_RECENTER:
				c.recenter_request++;
				break;
			case WID_XRQ_RAY:
				c.ray_width = c.ray_width == 3 ? 1 : 3;
				XrConfigSave();
				break;
			case WID_XRQ_PASSTHROUGH:
				c.passthrough = c.passthrough > 0 ? 0 : 100;
				XrConfigSave();
				break;
			case WID_XRQ_SAVE:
				ShowSaveLoadDialog(FT_SAVEGAME, SLO_SAVE);
				break;
			case WID_XRQ_SETTINGS:
				ShowGameOptionsXrTab();
				break;
			case WID_XRQ_QUIT:
				if (this->quit_armed_ms == 0) {
					/* First click: arm visibly (red, lowered, "Sure?") with a double pulse; falls back after 3 s. */
					this->quit_armed_ms = 3000;
					c.haptic_request += 2;
				} else {
					/* Second click within 3 s: save settings (and the game, like "autosave on exit"), then quit. */
					XrConfigSave();
					if (_game_mode == GM_NORMAL && !_networking && _settings_client.gui.autosave_on_exit) DoExitSave();
					_exit_game = true;
				}
				break;
		}
		this->OnInvalidateData();
		this->SetDirty();
	}

	void Navigate(int delta)
	{
		/* Skip disabled buttons, wrap around. */
		for (int i = 0; i < WID_XRQ_COUNT; ++i) {
			this->focus = (this->focus + delta + WID_XRQ_COUNT) % WID_XRQ_COUNT;
			if (!this->IsWidgetDisabled(this->focus)) break;
		}
		this->SetDirty();
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_xr_quick_menu_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, XR_BACKGROUND),
		NWidget(WWT_CAPTION, XR_BACKGROUND), SetStringTip(STR_XR_QUICK_CAPTION),
	EndContainer(),
	NWidget(WWT_PANEL, XR_BACKGROUND),
		NWidget(NWID_VERTICAL, NWidContainerFlag::EqualSize), SetPadding(WidgetDimensions::unscaled.sparse), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
			NWidget(WWT_PUSHTXTBTN, XR_BUTTON, WID_XRQ_CONTINUE), SetStringTip(STR_XR_CONTINUE), SetMinimalTextLines(2, 0), SetFill(1, 0),
			NWidget(WWT_PUSHTXTBTN, XR_BUTTON, WID_XRQ_RECENTER), SetStringTip(STR_XR_RECENTER), SetMinimalTextLines(2, 0), SetFill(1, 0),
			NWidget(WWT_PUSHTXTBTN, XR_BUTTON, WID_XRQ_RAY), SetMinimalTextLines(2, 0), SetFill(1, 0),
			NWidget(WWT_PUSHTXTBTN, XR_BUTTON, WID_XRQ_PASSTHROUGH), SetMinimalTextLines(2, 0), SetFill(1, 0),
			NWidget(WWT_PUSHTXTBTN, XR_BUTTON, WID_XRQ_SAVE), SetStringTip(STR_XR_SAVE_GAME), SetMinimalTextLines(2, 0), SetFill(1, 0),
			NWidget(WWT_PUSHTXTBTN, XR_BUTTON, WID_XRQ_SETTINGS), SetStringTip(STR_SETTINGS_MENU_XR_SETTINGS), SetMinimalTextLines(2, 0), SetFill(1, 0),
			NWidget(WWT_TEXTBTN, COLOUR_RED, WID_XRQ_QUIT), SetMinimalTextLines(2, 0), SetFill(1, 0),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _xr_quick_menu_desc(
	WDP_CENTER, {}, 0, 0,
	WC_GAME_OPTIONS, WC_NONE,
	{},
	_nested_xr_quick_menu_widgets
);

static XrQuickMenuWindow *GetXrQuickMenu()
{
	return dynamic_cast<XrQuickMenuWindow *>(FindWindowById(WC_GAME_OPTIONS, XR_QUICK_MENU_NUMBER));
}

void ShowXrQuickMenu()
{
	if (XrQuickMenuWindow *w = GetXrQuickMenu(); w != nullptr) {
		w->Close();
		return;
	}
	new XrQuickMenuWindow(_xr_quick_menu_desc);
}

bool XrQuickMenuOpen()
{
	return GetXrQuickMenu() != nullptr;
}

void XrQuickMenuNavigate(int delta)
{
	if (XrQuickMenuWindow *w = GetXrQuickMenu(); w != nullptr) w->Navigate(delta);
}

void XrQuickMenuActivate()
{
	if (XrQuickMenuWindow *w = GetXrQuickMenu(); w != nullptr) w->OnClick({}, w->focus, 1);
}
