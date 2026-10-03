/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file xr_v.cpp OpenXR video driver for the Meta Quest (XR-OpenTTD). */

#include "../../stdafx.h"
#include "../../openttd.h"
#include "../../gfx_func.h"
#include "../../palette_func.h"
#include "../../blitter/factory.hpp"
#include "../../core/geometry_func.hpp"
#include "../../window_func.h"
#include "../../window_gui.h"
#include "../../viewport_func.h"
#include "../../zoom_func.h"
#include "../../tilehighlight_func.h"
#include "../../framerate_type.h"
#include "../../gui.h"
#include "../../debug.h"
#include "../../driver.h"
#include "../../command_func.h"
#include "../../misc_cmd.h"
#include "../../textbuf_gui.h"
#include "../../strings_func.h"
#include "../../network/network.h"
#include "../../saveload/saveload.h"
#include "../../settings_type.h"
#include "xr_v.h"
#include "table/strings.h"
#include "xr_config.h"
#include "xr_autotest.h"
#include "xr_settings_gui.h"
#include "xr_gl.h"
#include "xr_input.h"
#include "xr_panel.h"

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <SDL.h>
#include <SDL_system.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "../../safeguards.h"

/* The OpenXR {XR_TYPE_X} initialisation idiom leaves the remaining fields zero on purpose. */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

static FVideoDriver_XR iFVideoDriver_XR;

/** Screen sizes offered in the game options; the screen in the room keeps its physical size. */
static const Dimension _xr_resolutions[] = {{1600, 900}, {1920, 1080}, {2560, 1440}};

namespace {

unsigned long NowMs()
{
	using namespace std::chrono;
	return static_cast<unsigned long>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

XrPosef IdentityPose()
{
	XrPosef pose{};
	pose.orientation.w = 1.f;
	return pose;
}

void CopyName(char *dst, size_t size, std::string_view src)
{
	const size_t n = std::min(size - 1, src.size());
	std::copy_n(src.data(), n, dst);
	dst[n] = '\0';
}

/** Log a failed OpenXR call; returns true on success. */
bool XrOk(XrInstance instance, XrResult result, std::string_view what)
{
	if (XR_SUCCEEDED(result)) return true;
	char buffer[XR_MAX_RESULT_STRING_SIZE] = {};
	if (instance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(instance, result, buffer))) {
		Debug(driver, 0, "xr: {} failed: {}", what, static_cast<int>(result));
	} else {
		Debug(driver, 0, "xr: {} failed: {}", what, buffer);
	}
	return false;
}

/** Controller direction: the aim pose looks along -Z. */
XrVector3f AimDirection(const XrQuaternionf &q)
{
	return {-2.f * (q.x * q.z + q.w * q.y), -2.f * (q.y * q.z - q.w * q.x), -(1.f - 2.f * (q.x * q.x + q.y * q.y))};
}

} // namespace

struct VideoDriver_XR::State {
	/* Game frame (32bpp, BGRA bytes). */
	std::vector<uint32_t> pixels;
	int width = 0, height = 0;
	Rect dirty{};
	Palette local_palette{};
	bool buffer_locked = false;
	std::thread::id main_thread; ///< OpenXR and GL are only used from the thread that started the driver.

	/* EGL / GL */
	EGLDisplay display = EGL_NO_DISPLAY;
	EGLConfig config = nullptr;
	EGLContext context = EGL_NO_CONTEXT;
	EGLSurface pbuffer = EGL_NO_SURFACE;
	GLuint game_texture = 0;
	int texture_width = 0, texture_height = 0;
	xrgl::ScreenCopy copy;
	xrgl::ColorDraw color;

	/* OpenXR */
	jobject activity = nullptr;
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
	bool running = false;
	bool system_exit = false; ///< The system closes the app (Quest menu, session end): save and quit.
	XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
	bool cylinder_supported = false;
	bool layer_settings_supported = false; ///< XR_FB_composition_layer_settings: supersampling for sharper text.
	bool perf_settings_supported = false; ///< XR_EXT_performance_settings: sustained high CPU level for the simulation.

	/* Passthrough (XR_FB_passthrough): the room as background behind the screen. */
	bool passthrough_supported = false, passthrough_running = false;
	int passthrough_applied = -1;
	XrPassthroughFB passthrough = XR_NULL_HANDLE;
	XrPassthroughLayerFB passthrough_layer = XR_NULL_HANDLE;
	PFN_xrCreatePassthroughFB create_passthrough = nullptr;
	PFN_xrDestroyPassthroughFB destroy_passthrough = nullptr;
	PFN_xrPassthroughStartFB start_passthrough = nullptr;
	PFN_xrPassthroughPauseFB pause_passthrough = nullptr;
	PFN_xrCreatePassthroughLayerFB create_passthrough_layer = nullptr;
	PFN_xrDestroyPassthroughLayerFB destroy_passthrough_layer = nullptr;
	PFN_xrPassthroughLayerResumeFB resume_passthrough_layer = nullptr;
	PFN_xrPassthroughLayerPauseFB pause_passthrough_layer = nullptr;
	PFN_xrPassthroughLayerSetStyleFB set_passthrough_style = nullptr;

	/* Performance line in the log every 10 s. */
	std::chrono::steady_clock::time_point perf_since{};
	int perf_frames = 0, perf_copies = 0;
	double perf_wait_ms = 0;
	size_t perf_upload_bytes = 0;
	int64_t swapchain_format = 0;
	bool frame_waited = false, should_render = false;
	XrTime display_time = 0;

	struct Swapchain {
		XrSwapchain handle = XR_NULL_HANDLE;
		int width = 0, height = 0;
		std::vector<XrSwapchainImageOpenGLESKHR> images;
		GLuint fbo = 0;
	};
	Swapchain panel, eyes[2];
	bool panel_has_image = false, panel_needs_copy = true;

	/* Actions */
	XrActionSet action_set = XR_NULL_HANDLE;
	XrAction aim = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, squeeze = XR_NULL_HANDLE, stick = XR_NULL_HANDLE,
		stick_click = XR_NULL_HANDLE, button_a = XR_NULL_HANDLE, button_b = XR_NULL_HANDLE, button_x = XR_NULL_HANDLE,
		button_y = XR_NULL_HANDLE, menu = XR_NULL_HANDLE, haptic = XR_NULL_HANDLE;
	XrPath hands[2] = {XR_NULL_PATH, XR_NULL_PATH};
	XrSpace aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

	/* Screen placement; the same pose drives the compositor layer and the ray hit test. */
	XrVector3f origin{};
	float yaw = 0.f, panel_z = -1.8f, panel_width = 2.f, panel_height = 0.f;
	bool start_pose_applied = false;
	unsigned recenter_seen = 0, reset_seen = 0, size_seen = 0;
	XrTime recenter_change_time = 0;
	unsigned long panel_save_at = 0;
	vrPanel::Pose Pose() const { return {{origin.x, origin.y, origin.z}, yaw, panel_z, 0.f, panel_height}; }
	XrVector3f ToWorld(float x, float y, float z) const
	{
		const auto p = this->Pose().toWorld({x, y, z});
		return {p.x, p.y, p.z};
	}

	/* Pointer and buttons */
	bool focused = false, armed = false;
	int pointer_hand = 1;
	bool pointer_valid = false, pointer_on_panel = false;
	XrVector3f ray_origin{}, ray_end{};
	float smooth_u = .5f, smooth_v = .5f;
	int last_x = 0, last_y = 0; ///< Last pointer position in screen pixels (for the locked cursor).
	bool filter_valid = false;
	unsigned long last_input = 0, click_lock_until = 0, wheel_repeat_at = 0;
	bool left_down = false, right_down = false;
	bool prev_trigger = false, prev_grip = false, prev_a = false, prev_b = false, prev_menu = false, prev_reset = false;
	int menu_nav_direction = 0;         ///< Stick direction last used for menu focus (-1 up, 1 down).
	unsigned long menu_nav_repeat_at = 0;
	unsigned haptic_seen = 0;           ///< Haptic requests of the menus already played.
	bool menu_open = false;             ///< A VR menu or the game options are open: the ray stays visible.
	vrInput::PanAccumulator pan;
	vrInput::ZoomRepeat zoom;
	vrInput::StickTurbo turbo;

	float Arc(const XrConfig &cfg) const { return this->cylinder_supported ? vrPanel::angle(cfg.curvature) : 0.f; }
	float Aspect() const { return this->width > 0 ? static_cast<float>(this->height) / this->width : 9.f / 16.f; }

	bool GetBool(XrAction action, int hand) const
	{
		XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
		info.action = action;
		info.subactionPath = this->hands[hand];
		XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
		return XR_SUCCEEDED(xrGetActionStateBoolean(this->session, &info, &state)) && state.isActive && state.currentState;
	}

	XrVector2f GetStick(int hand) const
	{
		XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
		info.action = this->stick;
		info.subactionPath = this->hands[hand];
		XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
		if (XR_FAILED(xrGetActionStateVector2f(this->session, &info, &state)) || !state.isActive) return {0.f, 0.f};
		return state.currentState;
	}

	void Pulse(int hand, float amplitude) const
	{
		if (!GetXrConfig().haptics) return;
		XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
		info.action = this->haptic;
		info.subactionPath = this->hands[hand];
		XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
		vibration.duration = 18000000; // 18 ms
		vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
		vibration.amplitude = amplitude;
		xrApplyHapticFeedback(this->session, &info, reinterpret_cast<const XrHapticBaseHeader *>(&vibration));
	}
};

VideoDriver_XR::VideoDriver_XR() : VideoDriver(true), xr(std::make_unique<State>()) {}

VideoDriver_XR::~VideoDriver_XR() = default;

std::optional<std::string_view> VideoDriver_XR::Start(const StringList &param)
{
	if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 32) return "xr: only the 32bpp blitters are supported";

	if (SDL_InitSubSystem(SDL_INIT_EVENTS) != 0) return "xr: SDL events unavailable";
	this->xr->main_thread = std::this_thread::get_id();

	XrConfigLoad();

	_resolutions.assign(std::begin(_xr_resolutions), std::end(_xr_resolutions));
	if (std::find(_resolutions.begin(), _resolutions.end(), _cur_resolution) == _resolutions.end()) {
		_cur_resolution = {1920, 1080};
	}
	_video_vsync = true; // The frame pacing comes from xrWaitFrame.

	auto error = this->InitXr();
	if (error) {
		this->ShutdownXr();
		return error;
	}

	this->AllocateBackingStore(_cur_resolution.width, _cur_resolution.height);
	MarkWholeScreenDirty();

	this->is_game_threaded = !GetDriverParamBool(param, "no_threads") && !GetDriverParamBool(param, "no_thread");
	return std::nullopt;
}

void VideoDriver_XR::Stop()
{
	XrConfigSave();
	this->ShutdownXr();
	SDL_QuitSubSystem(SDL_INIT_EVENTS);
}

std::optional<std::string_view> VideoDriver_XR::InitXr()
{
	State &s = *this->xr;

	/* The Khronos loader needs the Java VM and the activity on Android. */
	JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
	jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
	if (env == nullptr || activity == nullptr) return "xr: no Android activity";
	JavaVM *vm = nullptr;
	if (env->GetJavaVM(&vm) != JNI_OK) return "xr: no Java VM";
	s.activity = env->NewGlobalRef(activity);
	env->DeleteLocalRef(activity);

	PFN_xrVoidFunction fn = nullptr;
	if (!XrOk(XR_NULL_HANDLE, xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &fn), "xrGetInstanceProcAddr(xrInitializeLoaderKHR)")) return "xr: loader";
	XrLoaderInitInfoAndroidKHR loader_init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
	loader_init.applicationVM = vm;
	loader_init.applicationContext = s.activity;
	if (!XrOk(XR_NULL_HANDLE, reinterpret_cast<PFN_xrInitializeLoaderKHR>(fn)(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR *>(&loader_init)), "xrInitializeLoaderKHR")) return "xr: loader";

	/* EGL context for OpenXR; drawing happens only into swapchain framebuffers. */
	s.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (s.display == EGL_NO_DISPLAY || !eglInitialize(s.display, nullptr, nullptr)) return "xr: no EGL display";
	const EGLint config_attribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
	EGLint count = 0;
	if (!eglChooseConfig(s.display, config_attribs, &s.config, 1, &count) || count < 1) return "xr: no EGL config";
	const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
	s.context = eglCreateContext(s.display, s.config, EGL_NO_CONTEXT, context_attribs);
	if (s.context == EGL_NO_CONTEXT) return "xr: no GLES 3 context";
	const EGLint pbuffer_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
	s.pbuffer = eglCreatePbufferSurface(s.display, s.config, pbuffer_attribs);
	if (s.pbuffer == EGL_NO_SURFACE || !eglMakeCurrent(s.display, s.pbuffer, s.pbuffer, s.context)) return "xr: EGL context not current";
	if (!s.copy.Init() || !s.color.Init()) return "xr: shaders";

	/* Instance with the optional cylinder layer (curved screen) and supersampling. */
	bool android_instance = false;
	uint32_t ext_count = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
	std::vector<XrExtensionProperties> available(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
	xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, available.data());
	for (const auto &ext : available) {
		const std::string_view name = ext.extensionName;
		if (name == XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME) s.cylinder_supported = true;
		if (name == XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME) s.layer_settings_supported = true;
		if (name == XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME) android_instance = true;
		if (name == XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME) s.perf_settings_supported = true;
		if (name == XR_FB_PASSTHROUGH_EXTENSION_NAME) s.passthrough_supported = true;
	}
	std::vector<const char *> extensions{XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
	if (s.cylinder_supported) extensions.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
	if (s.layer_settings_supported) extensions.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
	if (android_instance) extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
	if (s.perf_settings_supported) extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
	if (s.passthrough_supported) extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);

	/* Required by the spec on Android: the runtime gets the VM and the activity. */
	XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
	android_info.applicationVM = vm;
	android_info.applicationActivity = s.activity;
	XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
	if (android_instance) instance_info.next = &android_info;
	CopyName(instance_info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "XR-OpenTTD");
	CopyName(instance_info.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "OpenTTD");
	instance_info.applicationInfo.applicationVersion = 1;
	instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	instance_info.enabledExtensionNames = extensions.data();
	if (!XrOk(XR_NULL_HANDLE, xrCreateInstance(&instance_info, &s.instance), "xrCreateInstance")) return "xr: no OpenXR instance";

	XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
	system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!XrOk(s.instance, xrGetSystem(s.instance, &system_info, &s.system), "xrGetSystem")) return "xr: no headset";

	/* Passthrough only when the device supports it and all functions load; otherwise the option is greyed out. */
	if (s.passthrough_supported) {
		XrSystemPassthroughPropertiesFB passthrough_properties{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB};
		XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
		properties.next = &passthrough_properties;
		s.passthrough_supported = XR_SUCCEEDED(xrGetSystemProperties(s.instance, s.system, &properties)) && passthrough_properties.supportsPassthrough;
		auto load = [&](const char *name, auto &function) {
			PFN_xrVoidFunction pointer = nullptr;
			if (XR_FAILED(xrGetInstanceProcAddr(s.instance, name, &pointer)) || pointer == nullptr) return false;
			function = reinterpret_cast<std::decay_t<decltype(function)>>(pointer);
			return true;
		};
		s.passthrough_supported = s.passthrough_supported &&
			load("xrCreatePassthroughFB", s.create_passthrough) && load("xrDestroyPassthroughFB", s.destroy_passthrough) &&
			load("xrPassthroughStartFB", s.start_passthrough) && load("xrPassthroughPauseFB", s.pause_passthrough) &&
			load("xrCreatePassthroughLayerFB", s.create_passthrough_layer) && load("xrDestroyPassthroughLayerFB", s.destroy_passthrough_layer) &&
			load("xrPassthroughLayerResumeFB", s.resume_passthrough_layer) && load("xrPassthroughLayerPauseFB", s.pause_passthrough_layer) &&
			load("xrPassthroughLayerSetStyleFB", s.set_passthrough_style);
	}
	GetXrConfig().passthrough_available = s.passthrough_supported;

	/* Required by the runtime before creating the session. */
	if (!XrOk(s.instance, xrGetInstanceProcAddr(s.instance, "xrGetOpenGLESGraphicsRequirementsKHR", &fn), "xrGetOpenGLESGraphicsRequirementsKHR")) return "xr: graphics requirements";
	XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
	reinterpret_cast<PFN_xrGetOpenGLESGraphicsRequirementsKHR>(fn)(s.instance, s.system, &requirements);

	XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
	binding.display = s.display;
	binding.config = s.config;
	binding.context = s.context;
	XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
	session_info.next = &binding;
	session_info.systemId = s.system;
	if (!XrOk(s.instance, xrCreateSession(s.instance, &session_info, &s.session), "xrCreateSession")) return "xr: no session";

	XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	space_info.poseInReferenceSpace = IdentityPose();
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (!XrOk(s.instance, xrCreateReferenceSpace(s.session, &space_info, &s.local), "local space")) return "xr: spaces";
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (!XrOk(s.instance, xrCreateReferenceSpace(s.session, &space_info, &s.view), "view space")) return "xr: spaces";

	/* sRGB swapchains keep the colours of the 2D game unchanged. */
	uint32_t format_count = 0;
	xrEnumerateSwapchainFormats(s.session, 0, &format_count, nullptr);
	std::vector<int64_t> formats(format_count);
	xrEnumerateSwapchainFormats(s.session, format_count, &format_count, formats.data());
	s.swapchain_format = std::find(formats.begin(), formats.end(), GL_SRGB8_ALPHA8) != formats.end() ? GL_SRGB8_ALPHA8 : GL_RGBA8;

	uint32_t view_count = 0;
	xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
	std::vector<XrViewConfigurationView> views(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, views.data());
	if (view_count != 2) return "xr: no stereo view";

	auto create_swapchain = [&](State::Swapchain &sc, int w, int h) {
		XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		info.format = s.swapchain_format;
		info.sampleCount = 1;
		info.width = w;
		info.height = h;
		info.faceCount = 1;
		info.arraySize = 1;
		info.mipCount = 1;
		if (!XrOk(s.instance, xrCreateSwapchain(s.session, &info, &sc.handle), "xrCreateSwapchain")) return false;
		sc.width = w;
		sc.height = h;
		uint32_t image_count = 0;
		xrEnumerateSwapchainImages(sc.handle, 0, &image_count, nullptr);
		sc.images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
		xrEnumerateSwapchainImages(sc.handle, image_count, &image_count, reinterpret_cast<XrSwapchainImageBaseHeader *>(sc.images.data()));
		glGenFramebuffers(1, &sc.fbo);
		return true;
	};
	for (int i = 0; i < 2; ++i) {
		if (!create_swapchain(s.eyes[i], views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight)) return "xr: eye swapchain";
	}

	/* Actions: pointer, buttons, sticks and haptics on both hands. */
	xrStringToPath(s.instance, "/user/hand/left", &s.hands[0]);
	xrStringToPath(s.instance, "/user/hand/right", &s.hands[1]);
	XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
	CopyName(set_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "openttd");
	CopyName(set_info.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "OpenTTD");
	if (!XrOk(s.instance, xrCreateActionSet(s.instance, &set_info, &s.action_set), "xrCreateActionSet")) return "xr: actions";
	auto create_action = [&](XrAction &action, std::string_view name, XrActionType type) {
		XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
		info.actionType = type;
		CopyName(info.actionName, XR_MAX_ACTION_NAME_SIZE, name);
		CopyName(info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, name);
		info.countSubactionPaths = 2;
		info.subactionPaths = s.hands;
		return XrOk(s.instance, xrCreateAction(s.action_set, &info, &action), name);
	};
	if (!create_action(s.aim, "aim", XR_ACTION_TYPE_POSE_INPUT) ||
			!create_action(s.trigger, "trigger", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
			!create_action(s.squeeze, "squeeze", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
			!create_action(s.stick, "stick", XR_ACTION_TYPE_VECTOR2F_INPUT) ||
			!create_action(s.stick_click, "stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
			!create_action(s.button_a, "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
			!create_action(s.button_b, "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
			!create_action(s.button_x, "button_x", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
			!create_action(s.button_y, "button_y", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
			!create_action(s.menu, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT) ||
			!create_action(s.haptic, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT)) {
		return "xr: actions";
	}

	auto suggest = [&](std::string_view profile, std::initializer_list<std::pair<XrAction, std::string_view>> bindings) {
		std::vector<XrActionSuggestedBinding> suggested;
		for (const auto &[action, path] : bindings) {
			XrActionSuggestedBinding b{action, XR_NULL_PATH};
			if (XR_SUCCEEDED(xrStringToPath(s.instance, std::string(path).c_str(), &b.binding))) suggested.push_back(b);
		}
		XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
		if (XR_FAILED(xrStringToPath(s.instance, std::string(profile).c_str(), &info.interactionProfile))) return;
		info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
		info.suggestedBindings = suggested.data();
		XrOk(s.instance, xrSuggestInteractionProfileBindings(s.instance, &info), profile);
	};
	suggest("/interaction_profiles/oculus/touch_controller", {
		{s.aim, "/user/hand/left/input/aim/pose"}, {s.aim, "/user/hand/right/input/aim/pose"},
		{s.trigger, "/user/hand/left/input/trigger/value"}, {s.trigger, "/user/hand/right/input/trigger/value"},
		{s.squeeze, "/user/hand/left/input/squeeze/value"}, {s.squeeze, "/user/hand/right/input/squeeze/value"},
		{s.stick, "/user/hand/left/input/thumbstick"}, {s.stick, "/user/hand/right/input/thumbstick"},
		{s.stick_click, "/user/hand/left/input/thumbstick/click"}, {s.stick_click, "/user/hand/right/input/thumbstick/click"},
		{s.button_a, "/user/hand/right/input/a/click"}, {s.button_b, "/user/hand/right/input/b/click"},
		{s.button_x, "/user/hand/left/input/x/click"}, {s.button_y, "/user/hand/left/input/y/click"},
		{s.menu, "/user/hand/left/input/menu/click"},
		{s.haptic, "/user/hand/left/output/haptic"}, {s.haptic, "/user/hand/right/output/haptic"},
	});
	suggest("/interaction_profiles/khr/simple_controller", {
		{s.aim, "/user/hand/left/input/aim/pose"}, {s.aim, "/user/hand/right/input/aim/pose"},
		{s.trigger, "/user/hand/left/input/select/click"}, {s.trigger, "/user/hand/right/input/select/click"},
		{s.menu, "/user/hand/left/input/menu/click"},
		{s.haptic, "/user/hand/left/output/haptic"}, {s.haptic, "/user/hand/right/output/haptic"},
	});
	for (int hand = 0; hand < 2; ++hand) {
		XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
		info.action = s.aim;
		info.subactionPath = s.hands[hand];
		info.poseInActionSpace = IdentityPose();
		if (!XrOk(s.instance, xrCreateActionSpace(s.session, &info, &s.aim_space[hand]), "aim space")) return "xr: aim space";
	}
	XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets = &s.action_set;
	if (!XrOk(s.instance, xrAttachSessionActionSets(s.session, &attach), "xrAttachSessionActionSets")) return "xr: actions";

	const XrConfig &cfg = GetXrConfig();
	s.panel_width = cfg.panel_width_mm / 1000.f;
	s.panel_z = -cfg.distance_mm / 1000.f;
	s.pointer_hand = cfg.pointer_hand;
	Debug(driver, 1, "xr: OpenXR ready (cylinder layer: {}, sRGB: {})", s.cylinder_supported, s.swapchain_format == GL_SRGB8_ALPHA8);
	return std::nullopt;
}

void VideoDriver_XR::ShutdownXr()
{
	State &s = *this->xr;
	if (s.display != EGL_NO_DISPLAY && s.context != EGL_NO_CONTEXT) {
		eglMakeCurrent(s.display, s.pbuffer, s.pbuffer, s.context);
		for (State::Swapchain *sc : {&s.panel, &s.eyes[0], &s.eyes[1]}) {
			if (sc->fbo != 0) glDeleteFramebuffers(1, &sc->fbo);
			sc->fbo = 0;
		}
		if (s.game_texture != 0) glDeleteTextures(1, &s.game_texture);
		s.game_texture = 0;
		s.copy.Destroy();
		s.color.Destroy();
	}
	for (State::Swapchain *sc : {&s.panel, &s.eyes[0], &s.eyes[1]}) {
		if (sc->handle != XR_NULL_HANDLE) xrDestroySwapchain(sc->handle);
		sc->handle = XR_NULL_HANDLE;
	}
	if (s.passthrough_layer != XR_NULL_HANDLE) s.destroy_passthrough_layer(s.passthrough_layer);
	if (s.passthrough != XR_NULL_HANDLE) s.destroy_passthrough(s.passthrough);
	s.passthrough_layer = XR_NULL_HANDLE;
	s.passthrough = XR_NULL_HANDLE;
	s.passthrough_running = false;
	if (s.action_set != XR_NULL_HANDLE) xrDestroyActionSet(s.action_set);
	s.action_set = XR_NULL_HANDLE;
	if (s.session != XR_NULL_HANDLE) xrDestroySession(s.session);
	s.session = XR_NULL_HANDLE;
	if (s.instance != XR_NULL_HANDLE) xrDestroyInstance(s.instance);
	s.instance = XR_NULL_HANDLE;
	if (s.display != EGL_NO_DISPLAY) {
		eglMakeCurrent(s.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		if (s.pbuffer != EGL_NO_SURFACE) eglDestroySurface(s.display, s.pbuffer);
		if (s.context != EGL_NO_CONTEXT) eglDestroyContext(s.display, s.context);
		eglTerminate(s.display);
	}
	s.display = EGL_NO_DISPLAY;
	s.context = EGL_NO_CONTEXT;
	s.pbuffer = EGL_NO_SURFACE;
	if (s.activity != nullptr) {
		JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
		if (env != nullptr) env->DeleteGlobalRef(s.activity);
		s.activity = nullptr;
	}
	s.running = false;
}

void VideoDriver_XR::AllocateBackingStore(int w, int h)
{
	State &s = *this->xr;
	s.pixels.assign(static_cast<size_t>(w) * h, 0);
	s.width = w;
	s.height = h;
	s.dirty = {};

	_screen.width = w;
	_screen.height = h;
	_screen.pitch = w;
	_screen.dst_ptr = s.pixels.data();

	CopyPalette(s.local_palette, true);
	BlitterFactory::GetCurrentBlitter()->PostResize();
	GameSizeChanged();
}

void VideoDriver_XR::MakeDirty(int left, int top, int width, int height)
{
	Rect r = {left, top, left + width, top + height};
	this->xr->dirty = BoundingRect(this->xr->dirty, r);
}

bool VideoDriver_XR::ChangeResolution(int w, int h)
{
	this->AllocateBackingStore(w, h);
	return true;
}

bool VideoDriver_XR::ToggleFullscreen(bool)
{
	return true; // The screen in the room has no window mode.
}

bool VideoDriver_XR::AfterBlitterChange()
{
	if (BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 32) return false;
	this->AllocateBackingStore(this->xr->width, this->xr->height);
	return true;
}

Dimension VideoDriver_XR::GetScreenSize() const
{
	return {2560, 1440};
}

void VideoDriver_XR::CheckPaletteAnim()
{
	if (!CopyPalette(this->xr->local_palette)) return;
	this->MakeDirty(0, 0, _screen.width, _screen.height);
}

bool VideoDriver_XR::PollEvent()
{
	SDL_Event ev;
	if (!SDL_PollEvent(&ev)) return false;
	if (ev.type == SDL_QUIT || ev.type == SDL_APP_TERMINATING) {
		/* The system closes the app: save (if enabled) and quit, done with the game state locked. */
		this->xr->system_exit = true;
	}
	return true;
}

void VideoDriver_XR::PollXrEvents()
{
	State &s = *this->xr;
	XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
	while (s.instance != XR_NULL_HANDLE && xrPollEvent(s.instance, &event) == XR_SUCCESS) {
		switch (event.type) {
			case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
				const auto &changed = *reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
				s.session_state = changed.state;
				Debug(driver, 1, "xr: session state {}", static_cast<int>(changed.state));
				switch (changed.state) {
					case XR_SESSION_STATE_READY: {
						XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
						begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
						s.running = XrOk(s.instance, xrBeginSession(s.session, &begin), "xrBeginSession");
						if (s.running && s.perf_settings_supported) {
							/* Without this the Quest 3 stays on a low CPU clock; large games simulate slowly. */
							PFN_xrVoidFunction fn = nullptr;
							if (XR_SUCCEEDED(xrGetInstanceProcAddr(s.instance, "xrPerfSettingsSetPerformanceLevelEXT", &fn)) && fn != nullptr) {
								XrOk(s.instance, reinterpret_cast<PFN_xrPerfSettingsSetPerformanceLevelEXT>(fn)(s.session,
									XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT), "CPU performance level");
							}
						}
						/* Fresh performance window, otherwise the first line after putting the headset on counts old frames. */
						s.perf_since = std::chrono::steady_clock::now();
						s.perf_frames = s.perf_copies = 0;
						s.perf_wait_ms = 0;
						s.perf_upload_bytes = 0;
						break;
					}
					case XR_SESSION_STATE_STOPPING:
						xrEndSession(s.session);
						s.running = false;
						s.frame_waited = false;
						break;
					case XR_SESSION_STATE_EXITING:
					case XR_SESSION_STATE_LOSS_PENDING:
						s.running = false;
						s.frame_waited = false;
						s.system_exit = true;
						break;
					default:
						break;
				}
				break;
			}
			case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
				/* Long press on the Meta button: recenter the screen in front of the user. */
				const auto &change = *reinterpret_cast<const XrEventDataReferenceSpaceChangePending *>(&event);
				if (change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) s.recenter_change_time = change.changeTime;
				break;
			}
			case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
				s.system_exit = true;
				break;
			default:
				break;
		}
		event = {XR_TYPE_EVENT_DATA_BUFFER};
	}
}

bool VideoDriver_XR::LockVideoBuffer()
{
	State &s = *this->xr;
	if (s.buffer_locked) return false;
	s.buffer_locked = true;

	/* The crash handler may lock the buffer from the game thread for a screenshot: no OpenXR calls there. */
	if (std::this_thread::get_id() != s.main_thread) {
		_screen.dst_ptr = s.pixels.data();
		return true;
	}

	this->PollXrEvents();
	if (s.running && !s.frame_waited) {
		/* Blocks until the headset wants the next frame; this paces drawing (vsync). */
		XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState state{XR_TYPE_FRAME_STATE};
		const auto wait_start = std::chrono::steady_clock::now();
		const XrResult wait_result = xrWaitFrame(s.session, &wait, &state);
		s.perf_wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_start).count();
		if (XR_SUCCEEDED(wait_result)) {
			s.frame_waited = true;
			s.display_time = state.predictedDisplayTime;
			s.should_render = state.shouldRender == XR_TRUE;
		} else {
			std::this_thread::sleep_for(std::chrono::milliseconds(5)); // No pacing without a frame.
		}
	}

	_screen.dst_ptr = s.pixels.data();
	return true;
}

void VideoDriver_XR::UnlockVideoBuffer()
{
	_screen.dst_ptr = nullptr;
	this->xr->buffer_locked = false;
}

void VideoDriver_XR::MainLoop()
{
	this->StartGameThread();
	while (!_exit_game) {
		this->Tick();
		/* Without a running session xrWaitFrame does not pace the loop. */
		if (!this->xr->running) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	this->StopGameThread();
}

bool VideoDriver_XR::Recenter(bool reset_size)
{
	State &s = *this->xr;
	XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
	if (s.display_time <= 0 || XR_FAILED(xrLocateSpace(s.view, s.local, s.display_time, &head)) ||
			!(head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
			!(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		return false;
	}
	const auto &q = head.pose.orientation;
	/* Only the heading is used: the screen stays upright. */
	s.yaw = std::atan2(2.f * (q.x * q.z + q.w * q.y), 1.f - 2.f * (q.x * q.x + q.y * q.y));
	s.origin = head.pose.position;
	if (reset_size) {
		s.panel_width = 2.f;
		s.panel_z = -1.8f;
		s.panel_save_at = NowMs() + 500;
	}
	s.filter_valid = false;
	return true;
}

/** Start/stop passthrough and apply its opacity (0 = off). Failures fall back to a black background. */
void VideoDriver_XR::UpdatePassthrough(int percent)
{
	State &s = *this->xr;
	if (!s.passthrough_supported) return;
	percent = std::clamp(percent, 0, 100);
	const bool wanted = percent > 0;
	if (wanted && !s.passthrough_running) {
		XrResult result = XR_SUCCESS;
		if (s.passthrough == XR_NULL_HANDLE) {
			XrPassthroughCreateInfoFB info{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
			result = s.create_passthrough(s.session, &info, &s.passthrough);
		}
		if (XR_SUCCEEDED(result) && s.passthrough_layer == XR_NULL_HANDLE) {
			XrPassthroughLayerCreateInfoFB info{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
			info.passthrough = s.passthrough;
			info.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
			result = s.create_passthrough_layer(s.session, &info, &s.passthrough_layer);
		}
		if (XR_SUCCEEDED(result)) result = s.start_passthrough(s.passthrough);
		if (XR_SUCCEEDED(result)) result = s.resume_passthrough_layer(s.passthrough_layer);
		s.passthrough_running = XR_SUCCEEDED(result);
		s.passthrough_applied = -1;
		XrOk(s.instance, result, "passthrough start");
	} else if (!wanted && s.passthrough_running) {
		s.pause_passthrough_layer(s.passthrough_layer);
		s.pause_passthrough(s.passthrough);
		s.passthrough_running = false;
	}
	if (s.passthrough_running && percent != s.passthrough_applied) {
		XrPassthroughStyleFB style{XR_TYPE_PASSTHROUGH_STYLE_FB};
		style.textureOpacityFactor = percent / 100.f;
		style.edgeColor = {0.f, 0.f, 0.f, 0.f};
		if (XrOk(s.instance, s.set_passthrough_style(s.passthrough_layer, &style), "passthrough style")) s.passthrough_applied = percent;
	}
}

void VideoDriver_XR::ReleaseInput()
{
	State &s = *this->xr;
	if (s.left_down) {
		s.left_down = false;
		_left_button_down = false;
		_left_button_clicked = false;
		HandleMouseEvents();
	}
	if (s.right_down) {
		s.right_down = false;
		_right_button_down = false;
		HandleMouseEvents();
	}
	if (_ctrl_pressed) {
		_ctrl_pressed = false;
		HandleCtrlChanged();
	}
	this->fast_forward_key_pressed = false;
	s.prev_trigger = s.prev_grip = s.prev_a = s.prev_b = s.prev_menu = s.prev_reset = false;
	s.armed = false; // A held button must be released before it acts again.
	s.filter_valid = false;
	s.pan = {};
	s.zoom = {};
	s.turbo = {};
}

/** The game was paused by the driver because the headset lost input focus. */
static bool _xr_paused_by_focus = false;

/** Headset taken off or system menu opened: pause a single-player game. */
static void XrPauseForFocusLoss()
{
	if (_game_mode != GM_NORMAL || _networking || _pause_mode.Test(PauseMode::Normal)) return;
	Command<CMD_PAUSE>::Post(PauseMode::Normal, true);
	_xr_paused_by_focus = true;
}

/** Back in the game: ask before continuing, so no time passes unnoticed. */
static void XrAskResume()
{
	if (!_xr_paused_by_focus) return;
	_xr_paused_by_focus = false;
	if (_game_mode != GM_NORMAL || !_pause_mode.Test(PauseMode::Normal)) return;
	ShowQuery(GetEncodedString(STR_XR_RESUME_CAPTION), GetEncodedString(STR_XR_RESUME_TEXT), GetMainWindow(),
		[](Window *, bool confirmed) {
			if (confirmed && _pause_mode.Test(PauseMode::Normal)) Command<CMD_PAUSE>::Post(PauseMode::Normal, false);
		});
}

/** Close the front-most window that the player may close, or cancel the current build tool. */
static void XrBack()
{
	if (_thd.place_mode != HT_NONE) {
		ResetObjectToPlace();
		return;
	}
	for (Window *w : Window::IterateFromFront()) {
		switch (w->window_class) {
			case WC_MAIN_WINDOW:
			case WC_MAIN_TOOLBAR:
			case WC_STATUS_BAR:
			case WC_TOOLTIPS:
			case WC_SELECT_GAME:
			case WC_BOOTSTRAP:
			case WC_MODAL_PROGRESS:
			case WC_NEWS_WINDOW:
				continue;
			default:
				break;
		}
		if (w->flags.Test(WindowFlag::Sticky)) continue;
		w->Close();
		return;
	}
}

void VideoDriver_XR::ProcessControllers()
{
	State &s = *this->xr;
	XrConfig &cfg = GetXrConfig();
	s.pointer_valid = false;
	s.pointer_on_panel = false;

	if (s.system_exit) {
		/* Closed by the system: no confirmation dialog, but keep the progress like "autosave on exit". */
		s.system_exit = false;
		if (_game_mode == GM_NORMAL && !_networking && _settings_client.gui.autosave_on_exit) DoExitSave();
		_exit_game = true;
		return;
	}

	/* A VR menu or the game options are open: the ray stays visible even when switched off. */
	s.menu_open = FindWindowByClass(WC_GAME_OPTIONS) != nullptr;

	/* Settings from the VR menu that take effect immediately. */
	s.panel_height = cfg.height_mm / 1000.f;
	if (cfg.size_request != s.size_seen) {
		s.size_seen = cfg.size_request;
		s.panel_width = std::clamp(cfg.panel_width_mm / 1000.f, 1.f, 3.2f);
		s.panel_z = -std::clamp(cfg.distance_mm / 1000.f, .7f, 3.5f);
		s.filter_valid = false;
	}
	const int cursor_shift = cfg.cursor_size == 1 ? 1 : cfg.cursor_size == 3 ? -1 : 0;
	const bool cursor_hidden = cfg.cursor_size == 0;
	if (cursor_shift != _cursor_zoom_shift || cursor_hidden != _cursor_hidden) {
		_cursor_zoom_shift = cursor_shift;
		_cursor_hidden = cursor_hidden;
		UpdateCursorSize();
	}

	const bool focused = s.running && s.session_state == XR_SESSION_STATE_FOCUSED;
	if (focused != s.focused) {
		if (focused) {
			XrAskResume();
		} else {
			this->ReleaseInput();
			XrPauseForFocusLoss();
		}
		s.focused = focused;
	}
	if (!focused || s.pointer_hand != cfg.pointer_hand) {
		/* A held button must not turn into a click with the other hand. */
		if (focused) this->ReleaseInput();
		s.pointer_hand = cfg.pointer_hand;
		_cursor.in_window = false;
		return;
	}

	XrActiveActionSet active{s.action_set, XR_NULL_PATH};
	XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	if (XR_FAILED(xrSyncActions(s.session, &sync))) {
		this->ReleaseInput();
		return;
	}

	const unsigned long now = NowMs();
	const float dt = s.last_input == 0 ? 0.f : std::min(0.05f, (now - s.last_input) / 1000.f);
	s.last_input = now;
	const int hand = s.pointer_hand, other = 1 - hand;

	/* Menus ask for haptic feedback (e.g. the double pulse of the armed "Quit"): one pulse per frame. */
	if (cfg.haptic_request != s.haptic_seen) {
		s.haptic_seen++;
		s.Pulse(hand, .25f);
	}

	if (!s.armed) {
		for (XrAction action : {s.trigger, s.squeeze, s.button_a, s.button_b, s.button_x, s.button_y, s.menu, s.stick_click}) {
			if (s.GetBool(action, 0) || s.GetBool(action, 1)) return;
		}
		s.armed = true;
	}

	/* --- Screen placement (other hand) ------------------------------------------------ */
	const bool placing = s.GetBool(s.squeeze, other);
	const XrVector2f other_stick = s.GetStick(other);
	const bool reset = s.GetBool(s.stick_click, other);
	if (reset && !s.prev_reset) this->Recenter(true);
	s.prev_reset = reset;
	if (placing) {
		/* Grip of the other hand unlocks the screen: stick left/right = width, forward/back = distance. */
		const float width_axis = vrInput::stickAxis(other_stick.x), distance_axis = vrInput::stickAxis(other_stick.y);
		if (width_axis != 0.f || distance_axis != 0.f) {
			s.panel_width = std::clamp(s.panel_width + width_axis * dt, 1.f, 3.2f);
			s.panel_z = std::clamp(s.panel_z - distance_axis * dt, -3.5f, -0.7f);
			const float distance = -s.panel_z;
			s.panel_width = std::clamp(s.panel_width, 0.55f * distance, 1.8f * distance);
			s.panel_save_at = now + 700;
		}
	}
	/* Stick forward/back of the other hand zooms the map (applied after the ray update below). */
	const int zoom_step = s.zoom.sample(cfg.invert_zoom ? -other_stick.y : other_stick.y, !placing, now);
	if (s.panel_save_at != 0 && now >= s.panel_save_at) {
		cfg.panel_width_mm = static_cast<int>(s.panel_width * 1000.f);
		cfg.distance_mm = static_cast<int>(-s.panel_z * 1000.f);
		XrConfigSave();
		s.panel_save_at = 0;
	}

	/* --- Ray → mouse position ------------------------------------------------------------ */
	const bool trigger = s.GetBool(s.trigger, hand);
	if (trigger && !s.prev_trigger) s.click_lock_until = now + 85;
	XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
	if (XR_SUCCEEDED(xrLocateSpace(s.aim_space[hand], s.local, s.display_time, &location)) &&
			(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
			(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		const XrVector3f o = location.pose.position;
		const XrVector3f dir = AimDirection(location.pose.orientation);
		s.pointer_valid = true;
		s.ray_origin = o;
		s.ray_end = {o.x + dir.x * 1.5f, o.y + dir.y * 1.5f, o.z + dir.z * 1.5f};

		const vrPanel::Pose pose = s.Pose();
		const auto lo = pose.toLocal({o.x, o.y, o.z});
		const auto ld = pose.directionToLocal({dir.x, dir.y, dir.z});
		const float arc = s.Arc(cfg);
		vrPanel::Hit hit;
		if (vrPanel::intersect(lo, ld, s.panel_width, s.panel_width * s.Aspect(), s.panel_z, arc, hit)) {
			s.pointer_on_panel = true;
			if (!s.filter_valid) {
				s.smooth_u = hit.u;
				s.smooth_v = hit.v;
			} else if (!cfg.click_stabilization || !trigger || now >= s.click_lock_until) {
				const float alpha = vrInput::smoothingAlpha(cfg.smoothing, dt);
				s.smooth_u += alpha * (hit.u - s.smooth_u);
				s.smooth_v += alpha * (hit.v - s.smooth_v);
			}
			s.filter_valid = true;

			/* Ray ends on the (smoothed) screen point, slightly in front of the surface. */
			float px = (s.smooth_u - .5f) * s.panel_width, pz = s.panel_z;
			if (arc > 0.f) {
				const float radius = s.panel_width / arc, theta = (s.smooth_u - .5f) * arc;
				px = radius * std::sin(theta);
				pz += radius * (1.f - std::cos(theta));
			}
			s.ray_end = s.ToWorld(px, (.5f - s.smooth_v) * s.panel_width * s.Aspect(), pz);
			s.ray_end = {s.ray_end.x - dir.x * .002f, s.ray_end.y - dir.y * .002f, s.ray_end.z - dir.z * .002f};

			const int x = std::clamp(static_cast<int>(s.smooth_u * s.width), 0, s.width - 1);
			const int y = std::clamp(static_cast<int>(s.smooth_v * s.height), 0, s.height - 1);
			_cursor.in_window = true;
			if (_cursor.fix_at) {
				/*
				 * Locked cursor (scroll modes "mouse locked", smallmap drag): the ray cannot be warped
				 * back, so pass on only the movement since the last frame, like the Cocoa driver.
				 */
				if (x != s.last_x || y != s.last_y) {
					_cursor.UpdateCursorPositionRelative(x - s.last_x, y - s.last_y);
					HandleMouseEvents();
				}
			} else if (x != _cursor.pos.x || y != _cursor.pos.y) {
				_cursor.UpdateCursorPosition(x, y);
				HandleMouseEvents();
			}
			s.last_x = x;
			s.last_y = y;
		}
	}
	if (!s.pointer_on_panel) {
		s.filter_valid = false;
		_cursor.in_window = false;
	}

	if (Window *main = GetMainWindow(); zoom_step != 0 && main != nullptr && main->viewport != nullptr && !XrQuickMenuOpen()) {
		if (s.pointer_on_panel && FindWindowFromPt(_cursor.pos.x, _cursor.pos.y) == main) {
			ZoomInOrOutToCursorWindow(zoom_step > 0, main);
		} else {
			DoZoomInOutWindow(zoom_step > 0 ? ZOOM_IN : ZOOM_OUT, main);
		}
	}

	/* --- Trigger = left click, grip = right click (pointer hand) -------------------------- */
	if (trigger != s.prev_trigger) {
		if (trigger && s.pointer_on_panel) {
			s.left_down = true;
			_left_button_down = true;
			HandleMouseEvents();
			s.Pulse(hand, .25f);
		} else if (!trigger && s.left_down) {
			s.left_down = false;
			_left_button_down = false;
			_left_button_clicked = false;
			HandleMouseEvents();
		}
		s.prev_trigger = trigger;
	}
	const bool grip = s.GetBool(s.squeeze, hand);
	if (grip != s.prev_grip) {
		if (grip && s.pointer_on_panel) {
			s.right_down = true;
			_right_button_down = true;
			_right_button_clicked = true;
			HandleMouseEvents();
			s.Pulse(hand, .2f);
		} else if (!grip && s.right_down) {
			s.right_down = false;
			_right_button_down = false;
			HandleMouseEvents();
		}
		s.prev_grip = grip;
	}

	/* --- Buttons: A confirm, B back, X Ctrl, Y fast forward, menu = VR quick menu ------- */
	const bool quick_menu = XrQuickMenuOpen();
	const bool a = s.GetBool(s.button_a, 1);
	if (a && !s.prev_a) {
		/* In the quick menu A presses the focused button, otherwise it is Enter. */
		if (quick_menu) {
			XrQuickMenuActivate();
		} else {
			HandleKeypress(WKC_RETURN, 0);
		}
		s.Pulse(1, .15f);
	}
	s.prev_a = a;
	const bool b = s.GetBool(s.button_b, 1);
	if (b && !s.prev_b) {
		XrBack();
		s.Pulse(1, .15f);
	}
	s.prev_b = b;
	const bool ctrl = s.GetBool(s.button_x, 0);
	if (ctrl != _ctrl_pressed) {
		_ctrl_pressed = ctrl;
		HandleCtrlChanged();
	}
	this->fast_forward_key_pressed = s.GetBool(s.button_y, 0);
	const bool menu = s.GetBool(s.menu, 0);
	if (menu && !s.prev_menu) {
		/* Opening/closing the menu releases every held game input; buttons re-arm after release. */
		this->ReleaseInput();
		ShowXrQuickMenu();
		return;
	}
	s.prev_menu = menu;

	/* --- Pointer-hand stick: in the quick menu move the focus, otherwise list or map ---- */
	const XrVector2f stick = s.GetStick(hand);
	if (quick_menu) {
		const int direction = stick.y > .6f ? -1 : stick.y < -.6f ? 1 : 0;
		if (direction != 0 && (direction != s.menu_nav_direction || now >= s.menu_nav_repeat_at)) {
			XrQuickMenuNavigate(direction);
			s.menu_nav_repeat_at = now + (direction != s.menu_nav_direction ? 450 : 200);
		}
		s.menu_nav_direction = direction;
		s.pan = {};
		return;
	}
	s.menu_nav_direction = 0;
	const bool boost = s.turbo.sample(stick.x, stick.y, now) || s.GetBool(s.stick_click, hand);
	Window *under = s.pointer_on_panel ? FindWindowFromPt(_cursor.pos.x, _cursor.pos.y) : nullptr;
	Window *main = GetMainWindow();
	/* Toolbar and status bar have nothing to scroll: there the stick keeps moving the map. */
	const bool scrollable = under != nullptr && under != main &&
		under->window_class != WC_MAIN_TOOLBAR && under->window_class != WC_STATUS_BAR;
	if (cfg.smart_scroll && scrollable && std::fabs(stick.y) > .25f) {
		if (now >= s.wheel_repeat_at) {
			_cursor.wheel += stick.y > 0 ? -1 : 1;
			_cursor.wheel_moved = true;
			HandleMouseEvents();
			s.wheel_repeat_at = now + 150;
		}
		s.pan = {};
	} else {
		int dx = 0, dy = 0;
		s.pan.sample(stick.x, -stick.y, dt, cfg.map_speed, boost, dx, dy);
		if ((dx != 0 || dy != 0) && main != nullptr && main->viewport != nullptr &&
				_game_mode != GM_MENU && _game_mode != GM_BOOTSTRAP) {
			main->viewport->CancelFollow(*main);
			main->viewport->dest_scrollpos_x += ScaleByZoom(dx, main->viewport->zoom);
			main->viewport->dest_scrollpos_y += ScaleByZoom(dy, main->viewport->zoom);
		}
	}
}

void VideoDriver_XR::InputLoop()
{
	XrAutotestStep();
	this->ProcessControllers();
}

void VideoDriver_XR::Paint()
{
	PerformanceMeasurer framerate(PFE_VIDEO);
	State &s = *this->xr;

	if (s.local_palette.count_dirty != 0) {
		Blitter *blitter = BlitterFactory::GetCurrentBlitter();
		if (blitter->UsePaletteAnimation() == Blitter::PaletteAnimation::Blitter) blitter->PaletteAnimate(s.local_palette);
		s.local_palette.count_dirty = 0;
	}

	this->RenderFrame();
}

void VideoDriver_XR::RenderFrame()
{
	State &s = *this->xr;
	if (!s.frame_waited || std::this_thread::get_id() != s.main_thread) return;
	s.frame_waited = false;

	XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
	if (!XrOk(s.instance, xrBeginFrame(s.session, &begin), "xrBeginFrame")) return;

	const XrConfig &cfg = GetXrConfig();

	/*
	 * Start and recenter: always in front of the current gaze (retried until the head pose is valid).
	 * The start keeps the stored size; the Meta button and "default size" also reset size and distance.
	 */
	const bool system_reset = s.recenter_change_time > 0 && s.display_time >= s.recenter_change_time;
	const bool reset = system_reset || cfg.reset_request != s.reset_seen;
	if (reset || !s.start_pose_applied || cfg.recenter_request != s.recenter_seen) {
		if (this->Recenter(reset)) {
			s.start_pose_applied = true;
			s.recenter_seen = cfg.recenter_request;
			s.reset_seen = cfg.reset_request;
			if (system_reset) s.recenter_change_time = 0;
		}
	}

	XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
	XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
	XrCompositionLayerSettingsFB layer_settings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
	XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	XrCompositionLayerProjectionView projection_views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
	std::vector<XrCompositionLayerBaseHeader *> layers;

	/* Passthrough below everything: the room as background (the screen itself stays opaque). */
	XrCompositionLayerPassthroughFB passthrough_layer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
	this->UpdatePassthrough(cfg.passthrough);
	if (s.passthrough_running && s.should_render) {
		passthrough_layer.layerHandle = s.passthrough_layer;
		passthrough_layer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&passthrough_layer));
	}

	if (s.should_render && s.width > 0 && s.start_pose_applied) {
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_BLEND);

		/* (Re)create the screen texture and swapchain in the game resolution. */
		if (s.texture_width != s.width || s.texture_height != s.height) {
			if (s.game_texture != 0) glDeleteTextures(1, &s.game_texture);
			glGenTextures(1, &s.game_texture);
			glBindTexture(GL_TEXTURE_2D, s.game_texture);
			/* Same encoding as the swapchain: sRGB in and out, or linear in and out as a fallback. */
			glTexStorage2D(GL_TEXTURE_2D, 1, s.swapchain_format == GL_SRGB8_ALPHA8 ? GL_SRGB8_ALPHA8 : GL_RGBA8, s.width, s.height);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glBindTexture(GL_TEXTURE_2D, 0);
			s.texture_width = s.width;
			s.texture_height = s.height;
			s.dirty = {0, 0, s.width, s.height};

			if (s.panel.handle != XR_NULL_HANDLE) xrDestroySwapchain(s.panel.handle);
			if (s.panel.fbo != 0) glDeleteFramebuffers(1, &s.panel.fbo);
			s.panel = {};
			s.panel_has_image = false;
			XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
			info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
			info.format = s.swapchain_format;
			info.sampleCount = 1;
			info.width = s.width;
			info.height = s.height;
			info.faceCount = 1;
			info.arraySize = 1;
			info.mipCount = 1;
			if (XrOk(s.instance, xrCreateSwapchain(s.session, &info, &s.panel.handle), "screen swapchain")) {
				s.panel.width = s.width;
				s.panel.height = s.height;
				uint32_t image_count = 0;
				xrEnumerateSwapchainImages(s.panel.handle, 0, &image_count, nullptr);
				s.panel.images.assign(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
				xrEnumerateSwapchainImages(s.panel.handle, image_count, &image_count, reinterpret_cast<XrSwapchainImageBaseHeader *>(s.panel.images.data()));
				glGenFramebuffers(1, &s.panel.fbo);
			}
		}

		/* Upload only the changed part of the game frame. */
		if (!IsEmptyRect(s.dirty)) {
			const Rect r = {std::max(0, s.dirty.left), std::max(0, s.dirty.top), std::min(s.width, s.dirty.right), std::min(s.height, s.dirty.bottom)};
			if (r.right > r.left && r.bottom > r.top) {
				glBindTexture(GL_TEXTURE_2D, s.game_texture);
				glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
				glPixelStorei(GL_UNPACK_ROW_LENGTH, s.width);
				glTexSubImage2D(GL_TEXTURE_2D, 0, r.left, r.top, r.right - r.left, r.bottom - r.top, GL_RGBA, GL_UNSIGNED_BYTE,
					s.pixels.data() + static_cast<size_t>(r.top) * s.width + r.left);
				glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
				s.perf_upload_bytes += static_cast<size_t>(r.right - r.left) * (r.bottom - r.top) * 4;
				glBindTexture(GL_TEXTURE_2D, 0);
			}
			s.dirty = {};
			s.panel_needs_copy = true;
		}

		/* Copy into the next swapchain image only when the picture changed. */
		static int applied_brightness = -1, applied_contrast = -1;
		if (applied_brightness != cfg.brightness || applied_contrast != cfg.contrast) {
			applied_brightness = cfg.brightness;
			applied_contrast = cfg.contrast;
			s.panel_needs_copy = true;
		}
		if (s.panel.handle != XR_NULL_HANDLE && s.panel_needs_copy) {
			uint32_t index = 0;
			XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = XR_INFINITE_DURATION;
			if (XR_SUCCEEDED(xrAcquireSwapchainImage(s.panel.handle, &acquire, &index))) {
				if (XR_SUCCEEDED(xrWaitSwapchainImage(s.panel.handle, &wait))) {
					glBindFramebuffer(GL_FRAMEBUFFER, s.panel.fbo);
					glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.panel.images[index].image, 0);
					glViewport(0, 0, s.panel.width, s.panel.height);
					s.copy.Draw(s.game_texture, cfg.brightness / 100.f, cfg.contrast / 100.f);
					glBindFramebuffer(GL_FRAMEBUFFER, 0);
					s.panel_has_image = true;
					s.panel_needs_copy = false;
					s.perf_copies++;
				}
				/* Always release an acquired image, otherwise every later acquire fails. */
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(s.panel.handle, &release);
			}
		}

		if (s.panel_has_image) {
			const auto o = s.Pose().orientation();
			const float arc = s.Arc(cfg);
			XrCompositionLayerBaseHeader *layer = nullptr;
			if (arc > 0.f) {
				cylinder.space = s.local;
				cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				cylinder.subImage.swapchain = s.panel.handle;
				cylinder.subImage.imageRect.extent = {s.panel.width, s.panel.height};
				cylinder.radius = s.panel_width / arc;
				cylinder.centralAngle = arc;
				cylinder.aspectRatio = static_cast<float>(s.width) / s.height;
				cylinder.pose.orientation = {o.x, o.y, o.z, o.w};
				cylinder.pose.position = s.ToWorld(0.f, 0.f, s.panel_z + cylinder.radius);
				layer = reinterpret_cast<XrCompositionLayerBaseHeader *>(&cylinder);
			} else {
				quad.space = s.local;
				quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				quad.subImage.swapchain = s.panel.handle;
				quad.subImage.imageRect.extent = {s.panel.width, s.panel.height};
				quad.pose.orientation = {o.x, o.y, o.z, o.w};
				quad.pose.position = s.ToWorld(0.f, 0.f, s.panel_z);
				quad.size = {s.panel_width, s.panel_width * s.Aspect()};
				layer = reinterpret_cast<XrCompositionLayerBaseHeader *>(&quad);
			}
			/* The compositor supersamples the screen: small text shimmers less when the head moves. */
			if (s.layer_settings_supported && cfg.sharpening != 0) {
				layer_settings.layerFlags = cfg.sharpening == 2 ? XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB : XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB;
				layer->next = &layer_settings;
			}
			layers.push_back(layer);
		}

		/* Pointer ray and target marker in a transparent stereo layer on top. */
		XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
		XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
		locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate.displayTime = s.display_time;
		locate.space = s.local;
		XrViewState view_state{XR_TYPE_VIEW_STATE};
		uint32_t located = 0;
		const bool views_valid = XR_SUCCEEDED(xrLocateViews(s.session, &locate, &view_state, 2, &located, views)) && located == 2 &&
			(view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
		if (s.pointer_valid && views_valid) {
			static const float colors[4][4] = {{1.f, 1.f, 1.f, 1.f}, {.7f, .7f, .68f, 1.f}, {.05f, .05f, .05f, 1.f}, {1.f, .65f, .1f, 1.f}};
			static const float widths[3] = {.001f, .003f, .006f};
			static const float markers[3] = {.004f, .007f, .012f};
			const float *color = colors[std::clamp(cfg.ray_color, 0, 3)];
			/* "Off" hides ray / marker during play; inside a VR menu both stay visible (medium size). */
			const bool show_ray = cfg.ray_width != 3 || s.menu_open;
			const bool show_marker = cfg.marker_size != 3 || s.menu_open;
			const int ray_index = cfg.ray_width == 3 ? 1 : std::clamp(cfg.ray_width, 0, 2);
			const int marker_index = cfg.marker_size == 3 ? 1 : std::clamp(cfg.marker_size, 0, 2);
			int eyes_ready = 0;
			for (int i = 0; i < 2; ++i) {
				State::Swapchain &eye = s.eyes[i];
				uint32_t index = 0;
				XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
				XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
				wait.timeout = XR_INFINITE_DURATION;
				if (XR_FAILED(xrAcquireSwapchainImage(eye.handle, &acquire, &index))) continue;
				if (XR_FAILED(xrWaitSwapchainImage(eye.handle, &wait))) {
					XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
					xrReleaseSwapchainImage(eye.handle, &release);
					continue;
				}
				glBindFramebuffer(GL_FRAMEBUFFER, eye.fbo);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eye.images[index].image, 0);
				glViewport(0, 0, eye.width, eye.height);
				glClearColor(0.f, 0.f, 0.f, 0.f);
				glClear(GL_COLOR_BUFFER_BIT);

				const XrView &v = views[i];
				const float q[4] = {v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w};
				const float p[3] = {v.pose.position.x, v.pose.position.y, v.pose.position.z};
				const xrgl::Mat4 mvp = xrgl::Multiply(xrgl::Projection(v.fov.angleLeft, v.fov.angleRight, v.fov.angleDown, v.fov.angleUp, .05f, 100.f), xrgl::View(q, p));

				/* Ribbon facing the eye: reliable thickness where GLES only has 1-pixel lines. */
				const XrVector3f &a = s.ray_origin, &e = s.ray_end;
				const float bx = e.x - a.x, by = e.y - a.y, bz = e.z - a.z;
				const float tx = p[0] - a.x, ty = p[1] - a.y, tz = p[2] - a.z;
				float sx = by * tz - bz * ty, sy = bz * tx - bx * tz, sz = bx * ty - by * tx;
				const float len = std::sqrt(sx * sx + sy * sy + sz * sz);
				if (len > 1e-6f && show_ray) {
					const float half = widths[ray_index] * .5f / len;
					sx *= half; sy *= half; sz *= half;
					const float ribbon[] = {a.x - sx, a.y - sy, a.z - sz, a.x + sx, a.y + sy, a.z + sz,
						e.x - sx, e.y - sy, e.z - sz, e.x + sx, e.y + sy, e.z + sz};
					s.color.Draw(mvp, ribbon, 4, GL_TRIANGLE_STRIP, color);
				}
				if (s.pointer_on_panel && show_marker) {
					/* Ring around the target point, facing the eye. */
					const float x = q[0], y = q[1], z = q[2], w = q[3];
					const float right[3] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)};
					const float up[3] = {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)};
					const float radius = markers[marker_index];
					float ring[66 * 3];
					for (int k = 0; k <= 32; ++k) {
						const float angle = k * 6.283185307f / 32.f, cx = std::cos(angle), cy = std::sin(angle);
						for (int j = 0; j < 2; ++j) {
							const float r = radius * (j == 0 ? 1.f : .6f);
							float *out = ring + (k * 2 + j) * 3;
							for (int c = 0; c < 3; ++c) out[c] = (&e.x)[c] + r * (right[c] * cx + up[c] * cy);
						}
					}
					s.color.Draw(mvp, ring, 66, GL_TRIANGLE_STRIP, color);
				}
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
				xrReleaseSwapchainImage(eye.handle, &release);

				projection_views[i].pose = v.pose;
				projection_views[i].fov = v.fov;
				projection_views[i].subImage.swapchain = eye.handle;
				projection_views[i].subImage.imageRect.extent = {eye.width, eye.height};
				++eyes_ready;
			}
			/* Submit the ray only with both eyes, otherwise xrEndFrame rejects the whole frame. */
			if (eyes_ready == 2) {
				projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
				projection.space = s.local;
				projection.viewCount = 2;
				projection.views = projection_views;
				layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader *>(&projection));
			}
		}
	}

	/* Performance line: headset frame rate, wait for the next frame, screen copies and upload volume. */
	s.perf_frames++;
	if (const auto now = std::chrono::steady_clock::now(); now - s.perf_since >= std::chrono::seconds(10)) {
		const double seconds = std::chrono::duration<double>(now - s.perf_since).count();
		Debug(driver, 0, "xr perf: {:.1f} fps, xrWaitFrame {:.1f} ms, {:.1f} screen copies/s, upload {:.1f} MB/s, screen {}x{}",
			s.perf_frames / seconds, s.perf_wait_ms / s.perf_frames, s.perf_copies / seconds,
			s.perf_upload_bytes / seconds / 1e6, s.width, s.height);
		s.perf_since = now;
		s.perf_frames = s.perf_copies = 0;
		s.perf_wait_ms = 0;
		s.perf_upload_bytes = 0;
	}

	XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
	end.displayTime = s.display_time;
	end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end.layerCount = static_cast<uint32_t>(layers.size());
	end.layers = layers.data();
	XrOk(s.instance, xrEndFrame(s.session, &end), "xrEndFrame");
}
