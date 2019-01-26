#include <android-config.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <stddef.h>
#include <malloc.h>
#include <sync/sync.h>

#include <wlr/util/log.h>

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

bool hwcomposer_api_init(struct wlr_hwcomposer_backend *hwc)
{
	int err;

	hw_module_t const* module = NULL;
	err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID, &module);
	assert(err == 0);

	hwc->gralloc = (gralloc_module_t*) module;
	err = gralloc_open((const hw_module_t *) hwc->gralloc, &hwc->alloc);

	framebuffer_device_t* fbDev = NULL;
	framebuffer_open(module, &fbDev);

	hw_module_t *hwcModule = 0;

	err = hw_get_module(HWC_HARDWARE_MODULE_ID, (const hw_module_t **) &hwcModule);
	assert(err == 0);

	hwc_composer_device_1_t *hwcDevicePtr = 0;
	err = hwc_open_1(hwcModule, &hwcDevicePtr);
	assert(err == 0);

	hwc->hwcDevicePtr = hwcDevicePtr;
	hw_device_t *hwcDevice = &hwcDevicePtr->common;

	uint32_t hwc_version = hwc->hwcVersion = interpreted_version(hwcDevice);

#ifdef HWC_DEVICE_API_VERSION_1_4
	if (hwc_version == HWC_DEVICE_API_VERSION_1_4) {
		hwcDevicePtr->setPowerMode(hwcDevicePtr, 0, HWC_POWER_MODE_NORMAL);
	} else
#endif
#ifdef HWC_DEVICE_API_VERSION_1_5
	if (hwc_version == HWC_DEVICE_API_VERSION_1_5) {
		hwcDevicePtr->setPowerMode(hwcDevicePtr, 0, HWC_POWER_MODE_NORMAL);
	} else
#endif
		hwcDevicePtr->blank(hwcDevicePtr, 0, 0);

	uint32_t configs[5];
	size_t numConfigs = 5;

	err = hwcDevicePtr->getDisplayConfigs(hwcDevicePtr, 0, configs, &numConfigs);
	assert (err == 0);

	int32_t attr_values[2];
	uint32_t attributes[] = { HWC_DISPLAY_WIDTH, HWC_DISPLAY_HEIGHT, HWC_DISPLAY_NO_ATTRIBUTE };

	hwcDevicePtr->getDisplayAttributes(hwcDevicePtr, 0,
			configs[0], attributes, attr_values);

	wlr_log(WLR_INFO, "width: %i height: %i\n", attr_values[0], attr_values[1]);
	hwc->hwcWidth = attr_values[0];
	hwc->hwcHeight = attr_values[1];

	size_t size = sizeof(hwc_display_contents_1_t) + 2 * sizeof(hwc_layer_1_t);
	hwc_display_contents_1_t *list = (hwc_display_contents_1_t *) malloc(size);
	hwc->hwcContents = (hwc_display_contents_1_t **) malloc(HWC_NUM_DISPLAY_TYPES * sizeof(hwc_display_contents_1_t *));

	int counter = 0;
	for (; counter < HWC_NUM_DISPLAY_TYPES; counter++)
		hwc->hwcContents[counter] = NULL;
	// Assign the layer list only to the first display,
	// otherwise HWC might freeze if others are disconnected
	hwc->hwcContents[0] = list;
	hwc->fblayer = &list->hwLayers[1];

	const hwc_rect_t rect = { 0, 0, attr_values[0], attr_values[1] };
	init_hwcomposer_layer(&list->hwLayers[0], &rect, HWC_FRAMEBUFFER);
    init_hwcomposer_layer(&list->hwLayers[1], &rect, HWC_FRAMEBUFFER_TARGET);

	list->retireFenceFd = -1;
	list->flags = HWC_GEOMETRY_CHANGED;
	list->numHwLayers = 2;

	return true;
}
