#ifndef BACKEND_HWCOMPOSER_H
#define BACKEND_HWCOMPOSER_H

#include <wlr/backend/hwcomposer.h>
#include <wlr/backend/interface.h>

#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>

#define HWCOMPOSER_DEFAULT_REFRESH (60 * 1000) // 60 Hz

struct wlr_hwcomposer_backend {
	struct wlr_backend backend;
	struct wlr_egl egl;
	struct wlr_renderer *renderer;
	struct wl_display *display;
	struct wl_list outputs;
	struct wl_list input_devices;
	struct wl_listener display_destroy;
	bool started;

	gralloc_module_t *gralloc;
	alloc_device_t *alloc;

	hwc_composer_device_1_t *hwcDevicePtr;
	hwc_display_contents_1_t **hwcContents;
	hwc_layer_1_t *fblayer;
	uint32_t hwcVersion;
	int hwcWidth;
	int hwcHeight;

	struct light_device_t *lightsDevice;
	int screenBrightness;
};

struct wlr_hwcomposer_output {
	struct wlr_output wlr_output;

	struct wlr_hwcomposer_backend *backend;
	struct wl_list link;

	void *egl_display;
	void *egl_surface;
	struct wl_event_source *frame_timer;
	int frame_delay; // ms
};

struct wlr_hwcomposer_input_device {
	struct wlr_input_device wlr_input_device;

	struct wlr_hwcomposer_backend *backend;
};

bool hwcomposer_api_init(struct wlr_hwcomposer_backend *hwc);

#endif
