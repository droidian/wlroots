#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "util/signal.h"
#include <stdlib.h>
#include <wayland-util.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/util/log.h>
#include <assert.h>
#include <stdio.h>
#include <dlfcn.h>
#include "time.h"
#include "backend/hwcomposer.h"
#include <android-config.h>

void *android_dlopen(const char *filename, int flags);
void *android_dlsym(void *handle, const char *symbol);
int android_dlclose(void *handle);

inline static uint32_t interpreted_version(hw_device_t *hwc_device)
{
	uint32_t version = hwc_device->version;

	if ((version & 0xffff0000) == 0) {
		// Assume header version is always 1
		uint32_t header_version = 1;

		// Legacy version encoding
		version = (version << 16) | header_version;
	}
	return version;
}

static bool backend_start(struct wlr_backend *wlr_backend) {
	struct wlr_hwcomposer_backend *hwc_backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;
	wlr_log(WLR_INFO, "Starting hwcomposer backend");

	hwc_backend->started = true;

	// FIXME: Drop this
	struct wlr_hwcomposer_output *output;
	wl_list_for_each(output, &hwc_backend->outputs, link) {
		wl_event_source_timer_update(output->vsync_timer, output->frame_delay);
		wlr_output_update_enabled(&output->wlr_output, true);
		wlr_signal_emit_safe(&hwc_backend->backend.events.new_output,
			&output->wlr_output);
	}

	return true;
}

static void backend_destroy(struct wlr_backend *wlr_backend) {
	struct wlr_hwcomposer_backend *hwc_backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;
	if (!wlr_backend) {
		return;
	}

	wl_list_remove(&hwc_backend->display_destroy.link);

	struct wlr_hwcomposer_output *output, *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &hwc_backend->outputs, link) {
		wlr_output_destroy(&output->wlr_output);
	}

	wlr_signal_emit_safe(&wlr_backend->events.destroy, hwc_backend);

	wlr_renderer_destroy(hwc_backend->renderer);
	wlr_egl_finish(&hwc_backend->egl);
	free(hwc_backend);
}

static struct wlr_renderer *backend_get_renderer(
		struct wlr_backend *wlr_backend) {
	struct wlr_hwcomposer_backend *hwc_backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;
	return hwc_backend->renderer;
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	.get_renderer = backend_get_renderer,
};

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_hwcomposer_backend *hwc_backend =
		wl_container_of(listener, hwc_backend, display_destroy);
	backend_destroy(&hwc_backend->backend);
}

static void start_fake_surfaceflinger(void) {
	void* libminisf;
	void (*startMiniSurfaceFlinger)(void) = NULL;

	// Adapted from https://github.com/mer-hybris/qt5-qpa-hwcomposer-plugin/blob/master/hwcomposer/hwcomposer_backend.cpp#L88

	// A reason for calling this method here is to initialize the binder
	// thread pool such that services started from for example the
	// hwcomposer plugin don't get stuck.

	libminisf = android_dlopen("libminisf.so", RTLD_LAZY);

	if (libminisf) {
		startMiniSurfaceFlinger = (void(*)(void))android_dlsym(libminisf, "startMiniSurfaceFlinger");
	}

	if (startMiniSurfaceFlinger) {
		wlr_log(WLR_INFO, "starting mini surface flinger");
		startMiniSurfaceFlinger();
	} else {
		wlr_log(WLR_INFO, "libminisf is incompatible or missing. Can not possibly start the fake SurfaceFlinger service.");
	}
}

void hwcomposer_init(struct wlr_hwcomposer_backend *hwc_backend) {
	wlr_log(WLR_INFO, "Creating hwcomposer backend");
	wlr_backend_init(&hwc_backend->backend, &backend_impl);
	wl_list_init(&hwc_backend->outputs);

	// Get idle time from the environment, if specified
	char *idle_time_env = getenv("WLR_HWC_IDLE_TIME");
	if (idle_time_env) {
		char *end;
		int idle_time = (int)strtol(idle_time_env, &end, 10);

		hwc_backend->idle_time = (*end || idle_time < 2) ? 2 * 1000000 : idle_time * 1000000;
	} else {
		// Default to 2
		hwc_backend->idle_time = 2 * 1000000;
	}
}

struct wlr_backend *wlr_hwcomposer_backend_create(struct wl_display *display,
		wlr_renderer_create_func_t create_renderer_func) {
	int err;
	struct wlr_hwcomposer_backend *hwc_backend;
	hw_module_t *hwc_module = 0;
	hw_device_t *hwc_device = NULL;

	start_fake_surfaceflinger();

#ifdef HWC_DEVICE_API_VERSION_2_0
	int hwc_version = HWC_DEVICE_API_VERSION_2_0;
#else
	int hwc_version = HWC_DEVICE_API_VERSION_1_3;
#endif // HWC_DEVICE_API_VERSION_2_0

	// Allow skipping version check via WLR_HWC_SKIP_VERSION_CHECK env variable
	if (getenv("WLR_HWC_SKIP_VERSION_CHECK") == NULL) {
		err = hw_get_module(HWC_HARDWARE_MODULE_ID, (const hw_module_t **) &hwc_module);

		if (err == 0) {
			wlr_log(WLR_INFO, "== hwcomposer module ==\n");
			wlr_log(WLR_INFO, " * Address: %p\n", hwc_module);
			wlr_log(WLR_INFO, " * Module API Version: %x\n", hwc_module->module_api_version);
			wlr_log(WLR_INFO, " * HAL API Version: %x\n", hwc_module->hal_api_version); /* should be zero */
			wlr_log(WLR_INFO, " * Identifier: %s\n", hwc_module->id);
			wlr_log(WLR_INFO, " * Name: %s\n", hwc_module->name);
			wlr_log(WLR_INFO, " * Author: %s\n", hwc_module->author);
			wlr_log(WLR_INFO, "== hwcomposer module ==\n");

			err = hwc_module->methods->open(hwc_module, HWC_HARDWARE_COMPOSER, &hwc_device);
		}

		if (!err) {
#ifdef HWC_DEVICE_API_VERSION_2_0
			// If there is an error, use the default (hwc2). It seems that on some
			// hwc2 devices the open call fails.
			wlr_log(WLR_ERROR, "Unable to determine hwc version. Fallbacking to hwc2");
			hwc_version = interpreted_version(hwc_device);
#else
			// We can't use hwc2_compat_layer, bail out
			wlr_log(WLR_ERROR, "Unable to determine hwc version.");
			return NULL;
#endif // HWC_DEVICE_API_VERSION_2_0
		}
	}

#ifdef HWC_DEVICE_API_VERSION_2_0
	if (hwc_version == HWC_DEVICE_API_VERSION_2_0)
		hwc_backend = hwcomposer2_api_init(hwc_device);
	else
#endif // HWC_DEVICE_API_VERSION_2_0
		hwc_backend = hwcomposer_api_init(hwc_device);

	wlr_log(WLR_INFO, "HWC Version=%x\n", hwc_version);

	hwc_backend->hwc_version = hwc_version;
	hwc_backend->display = display;

	static EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24,
		EGL_STENCIL_SIZE, 8,
		EGL_NONE
	};

	if (!create_renderer_func) {
		create_renderer_func = wlr_renderer_autocreate;
	}

	hwc_backend->renderer = create_renderer_func(&hwc_backend->egl, EGL_PLATFORM_ANDROID_KHR,
		NULL, config_attribs, HAL_PIXEL_FORMAT_RGBA_8888);

	if (!hwc_backend->renderer) {
		wlr_log(WLR_ERROR, "Failed to create renderer");
		free(hwc_backend);
		return NULL;
	}

	hwc_backend->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &hwc_backend->display_destroy);

	hwc_backend->egl.display = eglGetDisplay(NULL);

	// Prepare global vsync variables
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	hwc_backend->hwc_vsync_last_timestamp = now.tv_sec * 1000000000 + now.tv_nsec;
	hwc_backend->hwc_vsync_enabled = false;

	// Register hwc callbacks
	hwc_backend->impl->register_callbacks(hwc_backend);

	return &hwc_backend->backend;
}

void wlr_hwcomposer_backend_handle_hotplug(struct wlr_backend *wlr_backend,
	uint64_t display, bool connected, bool primary_display) {

	struct wlr_hwcomposer_backend *hwc_backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;
	struct wlr_hwcomposer_output *output, *tmp_output;

	if (connected) {
		wlr_hwcomposer_add_output((struct wlr_backend *)hwc_backend, display,
			primary_display);
	} else {
		wl_list_for_each_reverse_safe(output, tmp_output, &hwc_backend->outputs, link) {
			if (output->hwc_display_id == display) {
				wlr_hwcomposer_output_schedule_destroy(&output->wlr_output);
				break;
			}
		}
	}
}

bool wlr_backend_is_hwcomposer(struct wlr_backend *backend) {
	return backend->impl == &backend_impl;
}
