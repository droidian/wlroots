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

struct wlr_hwcomposer_backend {
	struct wlr_backend backend;
	struct wlr_egl egl;
	struct wlr_renderer *renderer;
	struct wlr_session *session;
	struct wl_display *display;
	struct wl_list outputs;
	struct wl_list input_devices;
	struct wl_listener display_destroy;
	struct wl_listener session_destroy;
	struct wl_listener session_signal;
	bool started;
	bool is_blank;

	hwc_composer_device_1_t *hwc_device_ptr;
	hwc_display_contents_1_t **hwc_contents;
	hwc_layer_1_t *fblayer;
	uint32_t hwc_version;
	int hwc_width;
	int hwc_height;
	int64_t hwc_refresh;
	bool hwc_vsync_enabled;

	int64_t idle_time; // nsec

	// FIXME: Store the last vsync event timestamp. This really
	// belongs to wlr_hwcomposer_output, but for simplicity's sake
	// until we support multiple outputs we'll keep it here.
	int64_t hwc_vsync_last_timestamp;
	// TODO: Also store 'vsyncPeriodNanos' if vsync2_4 is supported

#ifdef HWC_DEVICE_API_VERSION_2_0
	hwc2_compat_device_t* hwc2_device;
	hwc2_compat_display_t* hwc2_primary_display;
	hwc2_compat_layer_t* hwc2_primary_layer;
#endif
};

struct wlr_hwcomposer_output {
	struct wlr_output wlr_output;

	struct wlr_hwcomposer_backend *backend;
	struct wl_list link;

	struct ANativeWindow *egl_window;
	void *egl_display;
	void *egl_surface;
	struct wl_event_source *vsync_timer;
	int frame_delay; // ms
	int vsync_timer_fd;
	struct wl_event_source *vsync_event;
};

void hwcomposer_vsync_control(struct wlr_hwcomposer_backend *hwc, bool enable);
void hwcomposer_vsync_wait(struct wlr_hwcomposer_backend *hwc);
void hwcomposer_vsync_wake(struct wlr_hwcomposer_backend *hwc);
void hwcomposer_blank_toggle(struct wlr_hwcomposer_backend *hwc);
bool hwcomposer_api_init(struct wlr_hwcomposer_backend *hwc);
#ifdef HWC_DEVICE_API_VERSION_2_0
bool hwcomposer2_api_init(struct wlr_hwcomposer_backend *hwc);
void hwcomposer2_vsync_callback(HWC2EventListener* listener, int32_t sequence_id,
	hwc2_display_t display, int64_t timestamp);
void hwcomposer2_hotplug_callback(HWC2EventListener* listener, int32_t sequence_id,
	hwc2_display_t display, bool connected,
	bool primary_display);
void hwcomposer2_refresh_callback(HWC2EventListener* listener, int32_t sequence_id,
	hwc2_display_t display);
void hwcomposer2_close(struct wlr_hwcomposer_backend *hwc);
void hwcomposer2_present(void *user_data, struct ANativeWindow *window,
	struct ANativeWindowBuffer *buffer);
#endif

#endif
