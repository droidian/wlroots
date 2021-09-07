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
	bool is_blank;

	uint32_t hwc_version;
	int hwc_width;
	int hwc_height;
	int64_t hwc_refresh;
	bool hwc_vsync_enabled;

	int64_t idle_time; // nsec

	// Per the Android docs, every display is driven by the VSYNC signal
	// of the internal one, so it makes sense to keep this into the backend.
	bool vsync_frame_pending;
	int vsync_timer_fd;
	struct wl_event_source *vsync_event;
	int64_t hwc_vsync_last_timestamp;
	// TODO: Also store 'vsyncPeriodNanos' if vsync2_4 is supported
};

struct wlr_hwcomposer_output {
	struct wlr_output wlr_output;

	struct wlr_hwcomposer_backend *backend;
	struct wl_list link;

	struct ANativeWindow *egl_window;
	void *egl_display;
	void *egl_surface;

	bool needs_frame;

	struct wl_event_source *vsync_timer;
	int frame_delay; // ms
};

struct hwcomposer_impl {
	void (*present)(void *user_data, struct ANativeWindow *window, struct ANativeWindowBuffer *buffer);
	void (*vsync_control)(struct wlr_hwcomposer_backend *hwc_backend, bool enable);
	void (*set_power_mode)(struct wlr_hwcomposer_backend *hwc_backend, bool enable);
	void (*close)(struct wlr_hwcomposer_backend *hwc_backend);
};

void hwcomposer_init(struct wlr_hwcomposer_backend *hwc_backend);
void hwcomposer_schedule_frame(struct wlr_hwcomposer_backend *hwc_backend);
struct wlr_hwcomposer_backend *hwcomposer_api_init(hw_device_t *hwc_device);
#ifdef HWC_DEVICE_API_VERSION_2_0
struct wlr_hwcomposer_backend *hwcomposer2_api_init(hw_device_t *hwc_device);
#endif

#endif
