#include <android-config.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <stddef.h>
#include <malloc.h>
#include <sys/cdefs.h> // for __BEGIN_DECLS/__END_DECLS found in sync.h
#include <sync/sync.h>

#include <wlr/util/log.h>
#include <wlr/interfaces/wlr_output.h>

#include "backend/hwcomposer.h"

/* NOTICE: This is only compile-tested, and it **DOESN'T WORK**.
 * Droidian only uses hwc2, and this is being kept around "just in case".
 *
 * Interested parties should:
 * a) register callbacks to get hotplug and vsync events
 * b) feed hotplug and vsync events to the backend
 * c) tidy up the code and add multiple output support
*/

const struct hwcomposer_impl hwcomposer_hwc1;

struct wlr_hwcomposer_backend_hwc1
{
	struct wlr_hwcomposer_backend hwc_backend;

	hwc_composer_device_1_t *hwc_device_ptr;
	hwc_display_contents_1_t **hwc_contents;
	hwc_layer_1_t *fblayer;
};

struct wlr_hwcomposer_output_hwc1
{
	struct wlr_hwcomposer_output output;
};

static struct wlr_hwcomposer_backend_hwc1 *hwc1_backend_from_base(struct wlr_hwcomposer_backend *hwc_backend)
{
	assert(hwc_backend->impl == &hwcomposer_hwc1);
	return (struct wlr_hwcomposer_backend_hwc1 *)hwc_backend;
}

static struct wlr_hwcomposer_output_hwc1 *hwc1_output_from_base(struct wlr_hwcomposer_output *output)
{
	// TODO: ensure it's the correct one?
	return (struct wlr_hwcomposer_output_hwc1 *)output;
}

static bool hwcomposer_vsync_control(struct wlr_hwcomposer_output *output, bool enable)
{
	struct wlr_hwcomposer_backend *hwc_backend = output->hwc_backend;
	struct wlr_hwcomposer_backend_hwc1 *hwc1 = hwc1_backend_from_base(hwc_backend);

	if (hwc_backend->hwc_vsync_enabled == enable) {
		return true;
	}

	if (hwc1->hwc_device_ptr->eventControl(hwc1->hwc_device_ptr, 0, HWC_EVENT_VSYNC, enable ? 1 : 0)) {
		hwc_backend->hwc_vsync_enabled = enable;

		return true;
	}

	return false;
}

static bool hwcomposer_set_power_mode(struct wlr_hwcomposer_output *output, bool enable)
{
	struct wlr_hwcomposer_backend *hwc_backend = output->hwc_backend;
	struct wlr_hwcomposer_backend_hwc1 *hwc1 = hwc1_backend_from_base(hwc_backend);
	int result;

	hwcomposer_vsync_control(output, enable);
#if defined(HWC_DEVICE_API_VERSION_1_4) || defined(HWC_DEVICE_API_VERSION_1_5)
	if (hwc_backend->hwc_version > HWC_DEVICE_API_VERSION_1_3)
		result = hwc1->hwc_device_ptr->setPowerMode(hwc1->hwc_device_ptr, 0, enable ?
			HWC_POWER_MODE_NORMAL : HWC_POWER_MODE_OFF);
	else
#endif
		result = hwc1->hwc_device_ptr->blank(hwc1->hwc_device_ptr, 0, enable ? 0 : 1);

	if (result) {
		wlr_output_update_enabled(&output->wlr_output, enable);

		return true;
	}

	return false;
}

static void hwcomposer_present(void *user_data, struct ANativeWindow *window,
		struct ANativeWindowBuffer *buffer)
{
	struct wlr_hwcomposer_output *output = (struct wlr_hwcomposer_output *)user_data;
	struct wlr_hwcomposer_backend_hwc1 *hwc1 = hwc1_backend_from_base(output->hwc_backend);

	hwc_display_contents_1_t **contents = hwc1->hwc_contents;
	hwc_layer_1_t *fblayer = hwc1->fblayer;
	hwc_composer_device_1_t *hwcdevice = hwc1->hwc_device_ptr;

	int oldretire = contents[0]->retireFenceFd;
	contents[0]->retireFenceFd = -1;

	fblayer->handle = buffer->handle;
	fblayer->acquireFenceFd = HWCNativeBufferGetFence(buffer);
	fblayer->releaseFenceFd = -1;
	int err = hwcdevice->prepare(hwcdevice, HWC_NUM_DISPLAY_TYPES, contents);
	assert(err == 0);

	err = hwcdevice->set(hwcdevice, HWC_NUM_DISPLAY_TYPES, contents);
	// in android surfaceflinger ignores the return value as not all display types may be supported
	HWCNativeBufferSetFence(buffer, fblayer->releaseFenceFd);

	if (oldretire != -1) {
		sync_wait(oldretire, -1);
		close(oldretire);
	}
}

static void hwcomposer_close(struct wlr_hwcomposer_backend *hwc)
{
}

static void init_hwcomposer_layer(hwc_layer_1_t *layer, const hwc_rect_t *rect, int layerCompositionType)
{
	memset(layer, 0, sizeof(hwc_layer_1_t));
	layer->compositionType = layerCompositionType;
	layer->hints = 0;
	layer->flags = 0;
	layer->handle = 0;
	layer->transform = 0;
	layer->blending = HWC_BLENDING_NONE;
#ifdef HWC_DEVICE_API_VERSION_1_3
	layer->sourceCropf.top = 0.0f;
	layer->sourceCropf.left = 0.0f;
	layer->sourceCropf.bottom = (float) rect->bottom;
	layer->sourceCropf.right = (float) rect->right;
#else
	layer->sourceCrop = *rect;
#endif
	layer->displayFrame = *rect;
	layer->visibleRegionScreen.numRects = 1;
	layer->visibleRegionScreen.rects = &layer->displayFrame;
	layer->acquireFenceFd = -1;
	layer->releaseFenceFd = -1;
	layer->planeAlpha = 0xFF;
#ifdef HWC_DEVICE_API_VERSION_1_5
	layer->surfaceDamage.numRects = 0;
#endif
}

struct wlr_hwcomposer_backend *hwcomposer_api_init(hw_device_t *hwc_device)
{
	struct wlr_hwcomposer_backend_hwc1 *hwc1 =
		calloc(1, sizeof(struct wlr_hwcomposer_backend_hwc1));
	if (!hwc1) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_hwcomposer_backend_hwc1");
		return NULL;
	}

	hwcomposer_init (&hwc1->hwc_backend);

	hwc1->hwc_device_ptr = (hwc_composer_device_1_t*) hwc_device;

	hwc1->hwc_backend.impl = &hwcomposer_hwc1;

	return &hwc1->hwc_backend;
}

static struct wlr_hwcomposer_output* hwcomposer_add_output(struct wlr_hwcomposer_backend *hwc_backend, int display)
{
	int err;
	uint32_t configs[5];
	size_t numConfigs = 5;
	struct wlr_hwcomposer_backend_hwc1 *hwc1 = hwc1_backend_from_base(hwc_backend);
	struct wlr_hwcomposer_output_hwc1 *hwc1_output;

	if (display > 0) {
		wlr_log(WLR_ERROR, "hwcomposer1 backend only supports one display for now");
		return NULL;
	}

	hwc1_output = calloc(1, sizeof(struct wlr_hwcomposer_output_hwc1));
	if (hwc1_output == NULL) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_hwcomposer_output_hwc1");
		return NULL;
	}

	err = hwc1->hwc_device_ptr->getDisplayConfigs(hwc1->hwc_device_ptr, 0, configs, &numConfigs);
	assert (err == 0);

	hwc1_output->output.hwc_is_primary = (display == 0);

	int32_t attr_values[3];
	uint32_t attributes[] = { HWC_DISPLAY_WIDTH, HWC_DISPLAY_HEIGHT, HWC_DISPLAY_VSYNC_PERIOD, HWC_DISPLAY_NO_ATTRIBUTE };

	hwc1->hwc_device_ptr->getDisplayAttributes(hwc1->hwc_device_ptr, 0,
		configs[0], attributes, attr_values);

	wlr_log(WLR_INFO, "width: %i height: %i\n", attr_values[0], attr_values[1]);
	hwc1_output->output.hwc_width = attr_values[0];
	hwc1_output->output.hwc_height = attr_values[1];
	hwc1_output->output.hwc_refresh = (attr_values[2] == 0) ?
		(1000000000000LL / HWCOMPOSER_DEFAULT_REFRESH) : attr_values[2];

	size_t size = sizeof(hwc_display_contents_1_t) + 2 * sizeof(hwc_layer_1_t);
	hwc_display_contents_1_t *list = (hwc_display_contents_1_t *) malloc(size);
	hwc1->hwc_contents = (hwc_display_contents_1_t **) malloc(HWC_NUM_DISPLAY_TYPES * sizeof(hwc_display_contents_1_t *));

	int counter = 0;
	for (; counter < HWC_NUM_DISPLAY_TYPES; counter++)
		hwc1->hwc_contents[counter] = NULL;
	// Assign the layer list only to the first display,
	// otherwise HWC might freeze if others are disconnected
	hwc1->hwc_contents[0] = list;
	hwc1->fblayer = &list->hwLayers[1];

	const hwc_rect_t rect = { 0, 0, attr_values[0], attr_values[1] };
	init_hwcomposer_layer(&list->hwLayers[0], &rect, HWC_FRAMEBUFFER);
		init_hwcomposer_layer(&list->hwLayers[1], &rect, HWC_FRAMEBUFFER_TARGET);

	list->retireFenceFd = -1;
	list->flags = HWC_GEOMETRY_CHANGED;
	list->numHwLayers = 2;

	// FIXME: This being here is wrong
	if (hwc1_output->output.hwc_is_primary) {
		hwc1->hwc_backend.hwc_device_refresh = hwc1_output->output.hwc_refresh;
	}

	return &hwc1_output->output;
}

static void hwcomposer_destroy_output(struct wlr_hwcomposer_output *output)
{
	struct wlr_hwcomposer_output_hwc1 *hwc1_output = hwc1_output_from_base(output);

	free(hwc1_output);
}

static void hwcomposer_register_callbacks(struct wlr_hwcomposer_backend *hwc_backend)
{
	// TODO: implement this
}


const struct hwcomposer_impl hwcomposer_hwc1 = {
	.register_callbacks = hwcomposer_register_callbacks,
	.present = hwcomposer_present,
	.vsync_control = hwcomposer_vsync_control,
	.set_power_mode = hwcomposer_set_power_mode,
	.add_output = hwcomposer_add_output,
	.destroy_output = hwcomposer_destroy_output,
	.close = hwcomposer_close,
};
