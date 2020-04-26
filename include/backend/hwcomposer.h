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

#ifdef HWC_DEVICE_API_VERSION_2_0
	hwc2_compat_device_t* hwc2Device;
	hwc2_compat_display_t* hwc2_primary_display;
	hwc2_compat_layer_t* hwc2_primary_layer;
#endif

	struct light_device_t *lightsDevice;
	int screenBrightness;
};

struct wlr_hwcomposer_output {
	struct wlr_output wlr_output;

	struct wlr_hwcomposer_backend *backend;
	struct wl_list link;

	struct ANativeWindow *egl_window;
	void *egl_display;
	void *egl_surface;
	struct wl_event_source *frame_timer;
	int frame_delay; // ms
};

bool hwcomposer_api_init(struct wlr_hwcomposer_backend *hwc);
#ifdef HWC_DEVICE_API_VERSION_2_0
bool hwcomposer2_api_init(struct wlr_hwcomposer_backend *hwc);
void hwc2_callback_vsync(HWC2EventListener* listener, int32_t sequenceId,
                         hwc2_display_t display, int64_t timestamp);
void hwc2_callback_hotplug(HWC2EventListener* listener, int32_t sequenceId,
                           hwc2_display_t display, bool connected,
                           bool primaryDisplay);
void hwc2_callback_refresh(HWC2EventListener* listener, int32_t sequenceId,
                           hwc2_display_t display);
void hwc_hwcomposer2_close(struct wlr_hwcomposer_backend *hwc);
void hwc_present_hwcomposer2(void *user_data, struct ANativeWindow *window,
								struct ANativeWindowBuffer *buffer);
#endif

#endif
