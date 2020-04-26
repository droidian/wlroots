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
	uint32_t hwc_version = hwc->hwcVersion;

	hw_module_t *hwcModule = 0;

	err = hw_get_module(HWC_HARDWARE_MODULE_ID, (const hw_module_t **) &hwcModule);
	assert(err == 0);

	wlr_log(WLR_INFO, "== hwcomposer module ==\n");
	wlr_log(WLR_INFO, " * Address: %p\n", hwcModule);
	wlr_log(WLR_INFO, " * Module API Version: %x\n", hwcModule->module_api_version);
	wlr_log(WLR_INFO, " * HAL API Version: %x\n", hwcModule->hal_api_version); /* should be zero */
	wlr_log(WLR_INFO, " * Identifier: %s\n", hwcModule->id);
	wlr_log(WLR_INFO, " * Name: %s\n", hwcModule->name);
	wlr_log(WLR_INFO, " * Author: %s\n", hwcModule->author);
	wlr_log(WLR_INFO, "== hwcomposer module ==\n");

	hw_device_t *hwcDevice = NULL;
	err = hwcModule->methods->open(hwcModule, HWC_HARDWARE_COMPOSER, &hwcDevice);
#ifdef HWC_DEVICE_API_VERSION_2_0
	if (err) {
		// For weird reason module open seems to currently fail on tested HWC2 device
		hwc->hwcVersion = HWC_DEVICE_API_VERSION_2_0;
	} else
#endif
		hwc->hwcVersion = interpreted_version(hwcDevice);

#ifdef HWC_DEVICE_API_VERSION_2_0
	if (hwc->hwcVersion == HWC_DEVICE_API_VERSION_2_0)
		return hwcomposer2_api_init(hwc);
#endif

	hwc_composer_device_1_t *hwcDevicePtr = (hwc_composer_device_1_t*) hwcDevice;
	wlr_log(WLR_INFO, "hwc_version=%x\n", hwc_version);

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
#ifdef HWC_DEVICE_API_VERSION_2_0
	if (hwc_version == HWC_DEVICE_API_VERSION_2_0) {
		hwc2_compat_display_set_power_mode(hwc->hwc2_primary_display, HWC2_POWER_MODE_ON);
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
