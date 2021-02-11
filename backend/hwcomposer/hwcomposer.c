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

void hwcomposer_vsync_control(struct wlr_hwcomposer_backend *hwc, bool enable)
{
	if (hwc->hwc_vsync_enabled == enable) {
		return;
	}
	int result = 0;
#if defined(HWC_DEVICE_API_VERSION_2_0)
	if (hwc->hwc_version == HWC_DEVICE_API_VERSION_2_0)
		hwc2_compat_display_set_vsync_enabled(hwc->hwc2_primary_display, enable ? HWC2_VSYNC_ENABLE : HWC2_VSYNC_DISABLE);
	else
#endif
		result = hwc->hwc_device_ptr->eventControl(hwc->hwc_device_ptr, 0, HWC_EVENT_VSYNC, enable ? 1 : 0);
	hwc->hwc_vsync_enabled = enable && (result == 0);
}

void hwcomposer_blank_toggle(struct wlr_hwcomposer_backend *hwc)
{
	hwc->is_blank = !hwc->is_blank;
	hwcomposer_vsync_control(hwc, hwc->is_blank);
#ifdef HWC_DEVICE_API_VERSION_2_0
	if (hwc->hwc_version == HWC_DEVICE_API_VERSION_2_0)
		hwc2_compat_display_set_power_mode(hwc->hwc2_primary_display, hwc->is_blank ? HWC2_POWER_MODE_OFF : HWC2_POWER_MODE_ON);
	else
#endif
#if defined(HWC_DEVICE_API_VERSION_1_4) || defined(HWC_DEVICE_API_VERSION_1_5)
	if (hwc->hwc_version > HWC_DEVICE_API_VERSION_1_3)
		hwc->hwc_device_ptr->setPowerMode(hwc->hwc_device_ptr, 0, hwc->is_blank ? HWC_POWER_MODE_OFF : HWC_POWER_MODE_NORMAL);
	else
#endif
		hwc->hwc_device_ptr->blank(hwc->hwc_device_ptr, 0, hwc->is_blank ? 1 : 0);
}

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

	hw_module_t *hwc_module = 0;

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

	// Get idle time from the environment, if specified
	char *idle_time_env = getenv("WLR_HWC_IDLE_TIME");
	if (idle_time_env) {
		char *end;
		int idle_time = (int)strtol(idle_time_env, &end, 10);

		hwc->idle_time = (*end || idle_time < 4) ? 4 : idle_time;
	} else {
		// Default to 4
		hwc->idle_time = 4;
	}

	hw_device_t *hwcDevice = NULL;
	err = hwc_module->methods->open(hwc_module, HWC_HARDWARE_COMPOSER, &hwcDevice);
#ifdef HWC_DEVICE_API_VERSION_2_0
	if (err) {
		// For weird reason module open seems to currently fail on tested HWC2 device
		hwc->hwc_version = HWC_DEVICE_API_VERSION_2_0;
	} else
#endif
		hwc->hwc_version = interpreted_version(hwcDevice);
	hwc->is_blank = false;
#ifdef HWC_DEVICE_API_VERSION_2_0
	if (hwc->hwc_version == HWC_DEVICE_API_VERSION_2_0) {
		err = hwcomposer2_api_init(hwc);
		hwc2_compat_display_set_power_mode(hwc->hwc2_primary_display, HWC2_POWER_MODE_ON);
		return err;
	}
#endif

	hwc_composer_device_1_t *hwc_device_ptr = (hwc_composer_device_1_t*) hwcDevice;
	wlr_log(WLR_INFO, "HWC Version=%x\n", hwc->hwc_version);

#ifdef defined(HWC_DEVICE_API_VERSION_1_4) || defined(HWC_DEVICE_API_VERSION_1_5)
	if (hwc->hwc_version > HWC_DEVICE_API_VERSION_1_3) {
		hwc_device_ptr->setPowerMode(hwc_device_ptr, 0, HWC_POWER_MODE_NORMAL);
	} else
#endif
		hwc_device_ptr->blank(hwc_device_ptr, 0, 0);

	uint32_t configs[5];
	size_t numConfigs = 5;

	err = hwc_device_ptr->getDisplayConfigs(hwc_device_ptr, 0, configs, &numConfigs);
	assert (err == 0);

	int32_t attr_values[2];
	uint32_t attributes[] = { HWC_DISPLAY_WIDTH, HWC_DISPLAY_HEIGHT, HWC_DISPLAY_VSYNC_PERIOD, HWC_DISPLAY_NO_ATTRIBUTE };

	hwc_device_ptr->getDisplayAttributes(hwc_device_ptr, 0,
		configs[0], attributes, attr_values);

	wlr_log(WLR_INFO, "width: %i height: %i\n", attr_values[0], attr_values[1]);
	hwc->hwc_width = attr_values[0];
	hwc->hwc_height = attr_values[1];
	hwc->hwc_refresh = (attr_values[2] == 0) ? 60000 : 10E11 / attr_values[2];

	size_t size = sizeof(hwc_display_contents_1_t) + 2 * sizeof(hwc_layer_1_t);
	hwc_display_contents_1_t *list = (hwc_display_contents_1_t *) malloc(size);
	hwc->hwc_contents = (hwc_display_contents_1_t **) malloc(HWC_NUM_DISPLAY_TYPES * sizeof(hwc_display_contents_1_t *));

	int counter = 0;
	for (; counter < HWC_NUM_DISPLAY_TYPES; counter++)
		hwc->hwc_contents[counter] = NULL;
	// Assign the layer list only to the first display,
	// otherwise HWC might freeze if others are disconnected
	hwc->hwc_contents[0] = list;
	hwc->fblayer = &list->hwLayers[1];

	const hwc_rect_t rect = { 0, 0, attr_values[0], attr_values[1] };
	init_hwcomposer_layer(&list->hwLayers[0], &rect, HWC_FRAMEBUFFER);
		init_hwcomposer_layer(&list->hwLayers[1], &rect, HWC_FRAMEBUFFER_TARGET);

	list->retireFenceFd = -1;
	list->flags = HWC_GEOMETRY_CHANGED;
	list->numHwLayers = 2;

	return true;
}
