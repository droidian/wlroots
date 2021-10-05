#ifndef BACKEND_HWCOMPOSER_H
#define BACKEND_HWCOMPOSER_H

#include <wlr/backend/hwcomposer.h>
#include <wlr/backend/interface.h>

#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>

#ifdef HWC_DEVICE_API_VERSION_2_0
#include <hybris/hwc2/hwc2_compatibility_layer.h>
#endif

#define HWCOMPOSER_DEFAULT_REFRESH (60 * 1000) // 60 Hz

struct hwcomposer_impl;

struct wlr_hwcomposer_backend {
	struct wlr_backend backend;

	const struct hwcomposer_impl *impl;
	struct wlr_egl egl;
	struct wlr_renderer *renderer;
	struct wl_display *display;
	struct wl_list outputs;
	struct wl_list input_devices;
	struct wl_listener display_destroy;
	bool started;

	uint32_t hwc_version;
	bool hwc_vsync_enabled;

	int64_t idle_time; // nsec

	// This is the refresh rate of the main display. Every display is driven
	// by the VSYNC signal of the primary one, so this is going to be the
	// maximum refresh rate supported.
	int64_t hwc_device_refresh;

	// Per the Android docs, every display is driven by the VSYNC signal
	// of the internal one, so it makes sense to keep this into the backend.
	int64_t hwc_vsync_last_timestamp;
	// TODO: Also store 'vsyncPeriodNanos' if vsync2_4 is supported
};

struct wlr_hwcomposer_output {
	struct wlr_output wlr_output;

	struct wlr_hwcomposer_backend *hwc_backend;
	struct wl_list link;

	struct ANativeWindow *egl_window;
	void *egl_display;
	void *egl_surface;

	struct wlr_egl egl;

	bool hwc_is_primary;
	uint64_t hwc_display_id;
	int hwc_left;
	int hwc_top;
	int hwc_width;
	int hwc_height;
	int64_t hwc_refresh;

	struct wl_event_source *vsync_timer;
	int frame_delay; // ms
	int vsync_timer_fd;
	struct wl_event_source *vsync_event;
};

struct hwcomposer_impl {
	void (*register_callbacks)(struct wlr_hwcomposer_backend *hwc_backend);
	void (*present)(void *user_data, struct ANativeWindow *window, struct ANativeWindowBuffer *buffer);
	bool (*vsync_control)(struct wlr_hwcomposer_output *output, bool enable);
	bool (*set_power_mode)(struct wlr_hwcomposer_output *output, bool enable);
	struct wlr_hwcomposer_output *(*add_output)(struct wlr_hwcomposer_backend *hwc_backend, int display);
	void (*destroy_output)(struct wlr_hwcomposer_output *output);
	void (*close)(struct wlr_hwcomposer_backend *hwc_backend);
};

void hwcomposer_init(struct wlr_hwcomposer_backend *hwc_backend);
struct wlr_hwcomposer_backend *hwcomposer_api_init(hw_device_t *hwc_device);
#ifdef HWC_DEVICE_API_VERSION_2_0
struct wlr_hwcomposer_backend *hwcomposer2_api_init(hw_device_t *hwc_device);
#endif

#endif
