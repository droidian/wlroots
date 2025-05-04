#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>
#include <wayland-util.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/util/log.h>
#include <assert.h>
#include <stdio.h>
#include <dlfcn.h>
#include <libudev.h>
#include "time.h"
#include "backend/hwcomposer.h"
#include <android-config.h>
#include <drm_fourcc.h>

void *android_dlopen(const char *filename, int flags);
void *android_dlsym(void *handle, const char *symbol);
int android_dlclose(void *handle);

static bool backend_start(struct wlr_backend *wlr_backend) {
	struct wlr_hwcomposer_backend *hwc_backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;
	wlr_log(WLR_INFO, "Starting hwcomposer backend");

	hwc_backend->started = true;

	// FIXME: Drop this
	struct wlr_hwcomposer_output *output;
	wl_list_for_each(output, &hwc_backend->outputs, link) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wl_event_source_timer_update(output->vsync_timer, output->frame_delay);
		wlr_output_state_set_enabled(&state, true);
		wlr_output_state_finish(&state);
		wl_signal_emit_mutable(&hwc_backend->backend.events.new_output,
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

	if (hwc_backend->udev)
		udev_unref(hwc_backend->udev);

	wl_signal_emit_mutable(&wlr_backend->events.destroy, hwc_backend);

	free(hwc_backend);
}

static uint32_t get_buffer_caps(struct wlr_backend *wlr_backend) {
	return WLR_BUFFER_CAP_DATA_PTR
		| WLR_BUFFER_CAP_SHM;
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	.get_buffer_caps = get_buffer_caps,
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

struct wlr_backend *wlr_hwcomposer_backend_create(struct wl_display *display) {
	int err;
	struct wlr_hwcomposer_backend *hwc_backend;
	hw_module_t *hwc_module = 0;
	hw_device_t *hwc_device = NULL;

	start_fake_surfaceflinger();

	int hwc_version = HWC_DEVICE_API_VERSION_2_0;

	hwc_backend = hwcomposer2_api_init(hwc_device);

	wlr_log(WLR_INFO, "HWC Version=%x\n", hwc_version);

	hwc_backend->hwc_version = hwc_version;
	hwc_backend->display = display;

	wlr_drm_format_set_add(&hwc_backend->shm_formats, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR);
	wlr_drm_format_set_add(&hwc_backend->shm_formats, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR);

	hwc_backend->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &hwc_backend->display_destroy);

	// Prepare global vsync variables
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	hwc_backend->hwc_vsync_last_timestamp = now.tv_sec * 1000000000 + now.tv_nsec;
	hwc_backend->hwc_vsync_enabled = false;

	// Create a udev instance for panel brightness control
	if (getenv("WLR_HWC_SYSFS_BACKLIGHT") != NULL)
		hwc_backend->udev = udev_new();
	else
		hwc_backend->udev = NULL;

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
