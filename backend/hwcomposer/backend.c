#include "util/signal.h"
#include <stdlib.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/util/log.h>
#include <assert.h>
#include "backend/hwcomposer.h"

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
	struct wlr_hwcomposer_backend *backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;
	wlr_log(WLR_INFO, "Starting hwcomposer backend");

	struct wlr_hwcomposer_output *output;
	wl_list_for_each(output, &backend->outputs, link) {
		wl_event_source_timer_update(output->vsync_timer, output->frame_delay);
		wlr_output_update_enabled(&output->wlr_output, true);
		wlr_signal_emit_safe(&backend->backend.events.new_output,
			&output->wlr_output);
	}

	backend->started = true;
	return true;
}

static void backend_destroy(struct wlr_backend *wlr_backend) {
	struct wlr_hwcomposer_backend *backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;
	if (!wlr_backend) {
		return;
	}

	wl_list_remove(&backend->display_destroy.link);

	struct wlr_hwcomposer_output *output, *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &backend->outputs, link) {
		wlr_output_destroy(&output->wlr_output);
	}

	wlr_signal_emit_safe(&wlr_backend->events.destroy, backend);

	wlr_renderer_destroy(backend->renderer);
	wlr_egl_finish(&backend->egl);
	free(backend);
}

static struct wlr_renderer *backend_get_renderer(
		struct wlr_backend *wlr_backend) {
	struct wlr_hwcomposer_backend *backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;
	return backend->renderer;
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	.get_renderer = backend_get_renderer,
};

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_hwcomposer_backend *backend =
		wl_container_of(listener, backend, display_destroy);
	backend_destroy(&backend->backend);
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
	struct wlr_hwcomposer_backend *backend;
	hw_module_t *hwc_module = 0;
	hw_device_t *hwc_device = NULL;
#ifdef HWC_DEVICE_API_VERSION_2_0
	int hwc_version = HWC_DEVICE_API_VERSION_2_0;
#else
	int hwc_version = HWC_DEVICE_API_VERSION_1_3;
#endif // HWC_DEVICE_API_VERSION_2_0

	err = hw_get_module(HWC_HARDWARE_MODULE_ID, (const hw_module_t **) &hwc_module);
	assert(err == 0);

	wlr_log(WLR_INFO, "== hwcomposer module ==\n");
	wlr_log(WLR_INFO, " * Address: %p\n", hwc_module);
	wlr_log(WLR_INFO, " * Module API Version: %x\n", hwc_module->module_api_version);
	wlr_log(WLR_INFO, " * HAL API Version: %x\n", hwc_module->hal_api_version); /* should be zero */
	wlr_log(WLR_INFO, " * Identifier: %s\n", hwc_module->id);
	wlr_log(WLR_INFO, " * Name: %s\n", hwc_module->name);
	wlr_log(WLR_INFO, " * Author: %s\n", hwc_module->author);
	wlr_log(WLR_INFO, "== hwcomposer module ==\n");

	err = hwc_module->methods->open(hwc_module, HWC_HARDWARE_COMPOSER, &hwc_device);
	if (!err) {
		// If there is an error, use the default (hwc2). It seems that on some
		// hwc2 devices the open call fails.
		hwc_version = interpreted_version(hwc_device);
	}

#ifdef HWC_DEVICE_API_VERSION_2_0
	if (hwc_version == HWC_DEVICE_API_VERSION_2_0)
		backend = hwcomposer2_api_init(hwc_device);
	else
#endif // HWC_DEVICE_API_VERSION_2_0
		backend = hwcomposer_api_init(hwc_device);

	wlr_log(WLR_INFO, "HWC Version=%x\n", hwc_version);

	backend->hwc_version = hwc_version;
	backend->display = display;
	backend->is_blank = true; // reset by the set_power_mode call below

	backend->impl->set_power_mode(backend, true);

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

	backend->renderer = create_renderer_func(&backend->egl, EGL_PLATFORM_ANDROID_KHR,
		NULL, config_attribs, HAL_PIXEL_FORMAT_RGBA_8888);

	if (!backend->renderer) {
		wlr_log(WLR_ERROR, "Failed to create renderer");
		free(backend);
		return NULL;
	}

	backend->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &backend->display_destroy);

	return &backend->backend;
}

bool wlr_backend_is_hwcomposer(struct wlr_backend *backend) {
	return backend->impl == &backend_impl;
}
