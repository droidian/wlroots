#include <android-config.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <stddef.h>
#include <malloc.h>
#include <sys/cdefs.h> // for __BEGIN_DECLS/__END_DECLS found in sync.h
#include <sync/sync.h>

#include <hybris/hwcomposerwindow/hwcomposer.h>

#include <wlr/util/log.h>

#include "backend/hwcomposer.h"

#ifdef HWC_DEVICE_API_VERSION_2_0
typedef struct
{
	struct HWC2EventListener listener;
	struct wlr_hwcomposer_backend *hwc;
} hwc_procs_v20;

void hwcomposer2_vsync_callback(HWC2EventListener* listener, int32_t sequence_id,
		hwc2_display_t display, int64_t timestamp)
{
	struct wlr_hwcomposer_backend *hwc = ((hwc_procs_v20 *)listener)->hwc;
	wlr_signal_emit_safe(&hwc->events.vsync, hwc);
}

void hwcomposer2_hotplug_callback(HWC2EventListener* listener, int32_t sequence_id,
		hwc2_display_t display, bool connected,
		bool primary_display)
{
	struct wlr_hwcomposer_backend *hwc = ((hwc_procs_v20*) listener)->hwc;

	wlr_log(WLR_INFO, "onHotplugReceived(%d, %" PRIu64 ", %s, %s)",
		sequence_id, display,
		connected ? "connected" : "disconnected",
		primary_display ? "primary\n" : "external\n");

	hwc2_compat_device_on_hotplug(hwc->hwc2_device, display, connected);
}

void hwcomposer2_refresh_callback(HWC2EventListener* listener, int32_t sequence_id,
		hwc2_display_t display)
{
}

bool hwcomposer2_api_init(struct wlr_hwcomposer_backend *hwc)
{
	int err;
	static int composer_sequence_id = 0;

	hwc_procs_v20* procs = malloc(sizeof(hwc_procs_v20));
	procs->listener.on_vsync_received = hwcomposer2_vsync_callback;
	procs->listener.on_hotplug_received = hwcomposer2_hotplug_callback;
	procs->listener.on_refresh_received = hwcomposer2_refresh_callback;
	procs->hwc = hwc;

	hwc2_compat_device_t* hwc2_device = hwc->hwc2_device = hwc2_compat_device_new(false);
	assert(hwc2_device);

	//hwc_set_power_mode(pScrn, HWC_DISPLAY_PRIMARY, 1);

	hwc2_compat_device_register_callback(hwc2_device, &procs->listener,
		composer_sequence_id++);

	for (int i = 0; i < 5 * 1000; ++i) {
		// Wait at most 5s for hotplug events
		if ((hwc->hwc2_primary_display =
			hwc2_compat_device_get_display_by_id(hwc2_device, 0)))
			break;
		sleep(1000);
	}
	assert(hwc->hwc2_primary_display);

	HWC2DisplayConfig *config = hwc2_compat_display_get_active_config(hwc->hwc2_primary_display);
	assert(config);

	hwc->hwc_width = config->width;
	hwc->hwc_height = config->height;
	hwc->hwc_refresh = (config->vsyncPeriod == 0) ? 60000 : 10E11 / config->vsyncPeriod;
	wlr_log(WLR_INFO, "width: %i height: %i Refresh: %i\n", config->width, config->height, hwc->hwc_refresh);

	if (!hwc->idle_time)
		// Try to be as close as possible as the actual vsync
		// as calculated by the hwc_refresh.
		hwc->idle_time = (1000000 / hwc->hwc_refresh) - 3; // ms

	hwc2_compat_layer_t* layer = hwc->hwc2_primary_layer =
		hwc2_compat_display_create_layer(hwc->hwc2_primary_display);

	hwc2_compat_layer_set_composition_type(layer, HWC2_COMPOSITION_CLIENT);
	hwc2_compat_layer_set_blend_mode(layer, HWC2_BLEND_MODE_NONE);
	hwc2_compat_layer_set_source_crop(layer, 0.0f, 0.0f, hwc->hwc_width, hwc->hwc_height);
	hwc2_compat_layer_set_display_frame(layer, 0, 0, hwc->hwc_width, hwc->hwc_height);
	hwc2_compat_layer_set_visible_region(layer, 0, 0, hwc->hwc_width, hwc->hwc_height);

	return true;
}

void hwcomposer2_close(struct wlr_hwcomposer_backend *hwc)
{
}

void hwcomposer2_present(void *user_data, struct ANativeWindow *window,
		struct ANativeWindowBuffer *buffer)
{
	struct wlr_hwcomposer_backend *hwc = (struct wlr_hwcomposer_backend *)user_data;
	static int last_present_fence = -1;

	uint32_t num_types = 0;
	uint32_t num_requests = 0;
	int display_id = 0;
	hwc2_error_t error = HWC2_ERROR_NONE;

	int acquireFenceFd = HWCNativeBufferGetFence(buffer);
	int sync_before_set = 0;

	if (sync_before_set && acquireFenceFd >= 0) {
		sync_wait(acquireFenceFd, -1);
		close(acquireFenceFd);
		acquireFenceFd = -1;
	}

	hwc2_compat_display_t* hwc_display = hwc->hwc2_primary_display;

	error = hwc2_compat_display_validate(hwc_display, &num_types,
		&num_requests);
	if (error != HWC2_ERROR_NONE && error != HWC2_ERROR_HAS_CHANGES) {
		wlr_log(WLR_ERROR, "prepare: validate failed for display %d: %d", display_id, error);
		return;
	}

	if (num_types || num_requests) {
		wlr_log(WLR_ERROR, "prepare: validate required changes for display %d: %d",
			display_id, error);
		return;
	}

	error = hwc2_compat_display_accept_changes(hwc_display);
	if (error != HWC2_ERROR_NONE) {
		wlr_log(WLR_ERROR, "prepare: acceptChanges failed: %d", error);
		return;
	}

	hwc2_compat_display_set_client_target(hwc_display, /* slot */0, buffer,
		acquireFenceFd,
		HAL_DATASPACE_UNKNOWN);

	hwcomposer_vsync_control(hwc, true);
	int present_fence = -1;
	hwc2_compat_display_present(hwc_display, &present_fence);

	if (last_present_fence != -1) {
		sync_wait(last_present_fence, -1);
		close(last_present_fence);
	}

	last_present_fence = present_fence != -1 ? dup(present_fence) : -1;

	HWCNativeBufferSetFence(buffer, present_fence);
}
#endif // HWC_DEVICE_API_VERSION_2_0
