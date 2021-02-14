#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/cdefs.h> // for __BEGIN_DECLS/__END_DECLS found in sync.h
#include <sync/sync.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>

#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/util/log.h>
#include "backend/hwcomposer.h"
#include "util/signal.h"

static void output_present(void *user_data, struct ANativeWindow *window,
		struct ANativeWindowBuffer *buffer)
{
	struct wlr_hwcomposer_backend *hwc = (struct wlr_hwcomposer_backend *)user_data;
	hwc_display_contents_1_t **contents = hwc->hwc_contents;
	hwc_layer_1_t *fblayer = hwc->fblayer;
	hwc_composer_device_1_t *hwcdevice = hwc->hwc_device_ptr;

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

static bool output_set_custom_mode(struct wlr_output *wlr_output, int32_t width,
		int32_t height, int32_t refresh) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;
	struct wlr_hwcomposer_backend *backend = output->backend;

	wlr_log(WLR_INFO, "output_set_custom_mode width=%d height=%d refresh=%d",
		width, height, refresh);

	if (refresh <= 0) {
		refresh = HWCOMPOSER_DEFAULT_REFRESH;
	}

	wlr_egl_destroy_surface(&backend->egl, output->egl_surface);

	output->egl_surface = eglCreateWindowSurface(backend->egl.display,
		backend->egl.config, (EGLNativeWindowType)output->egl_window, NULL);
	if (output->egl_surface == EGL_NO_SURFACE) {
		wlr_log(WLR_ERROR, "Failed to recreate EGL surface");
		wlr_output_destroy(wlr_output);
		return false;
	}

	output->frame_delay = 1000000 / refresh;

	wlr_output_update_custom_mode(&output->wlr_output, width, height, refresh);
	return true;
}

static bool output_commit(struct wlr_output *wlr_output) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;

	if (wlr_output->pending.committed & WLR_OUTPUT_STATE_ENABLED) {
		hwcomposer_blank_toggle(output->backend);
		wlr_output_update_enabled(wlr_output, !output->backend->is_blank);

		if (output->backend->is_blank)
			// Disable vsync
			hwcomposer_vsync_control(output->backend, false);
		else
			// Start timer so that we can let hwc initialize
			wl_event_source_timer_update(output->vsync_timer,
				output->frame_delay); // ms
	}

	if (output->backend->is_blank)
		return true;

	if (wlr_output->pending.committed & WLR_OUTPUT_STATE_MODE) {
		if (!output_set_custom_mode(wlr_output,
				wlr_output->pending.custom_mode.width,
				wlr_output->pending.custom_mode.height,
				wlr_output->pending.custom_mode.refresh)) {
			return false;
		}
	}

	if (wlr_output->pending.committed & WLR_OUTPUT_STATE_BUFFER) {
		pixman_region32_t *damage = NULL;
		if (wlr_output->pending.committed & WLR_OUTPUT_STATE_DAMAGE) {
			damage = &wlr_output->pending.damage;
		}

		switch (wlr_output->pending.buffer_type) {
		case WLR_OUTPUT_STATE_BUFFER_RENDER:
			if (!wlr_egl_swap_buffers(&output->backend->egl,
					output->egl_surface, damage)) {
				return false;
			}
			break;
		case WLR_OUTPUT_STATE_BUFFER_SCANOUT:;
			wlr_log(WLR_ERROR, "WLR_OUTPUT_STATE_BUFFER_SCANOUT not implemented");
			break;
		}
	}

	wlr_egl_unset_current(&output->backend->egl);

	return true;
}

static bool output_attach_render(struct wlr_output *wlr_output, int *buffer_age) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;
	return wlr_egl_make_current(&output->backend->egl, output->egl_surface,
		buffer_age);
}

static void output_destroy(struct wlr_output *wlr_output) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;

	wl_list_remove(&output->link);

	wl_event_source_remove(output->frame_timer);
	wl_event_source_remove(output->vsync_timer);

	wl_list_remove(&output->vsync_listener.link);

	wlr_egl_destroy_surface(&output->backend->egl, output->egl_surface);

	// Disable vsync
	hwcomposer_vsync_control(output->backend, false);

	// XXX: free native window?

	free(output);
}

static void output_rollback_render(struct wlr_output *wlr_output) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;
	wlr_egl_unset_current(&output->backend->egl);
}

static const struct wlr_output_impl output_impl = {
	.destroy = output_destroy,
	.attach_render = output_attach_render,
	.commit = output_commit,
	.rollback_render = output_rollback_render,
};

bool wlr_output_is_hwcomposer(struct wlr_output *wlr_output) {
	return wlr_output->impl == &output_impl;
}

static int signal_frame(void *data) {
	struct wlr_hwcomposer_output *output = data;

	wlr_output_send_present(&output->wlr_output, NULL);

	wlr_output_send_frame(&output->wlr_output);

	return 0;
}

static int on_vsync_timer_elapsed(void *data) {
	struct wlr_hwcomposer_output *output = data;
	static int vsync_enable_tries = 0;

	// Ensure vsync gets enabled
	hwcomposer_vsync_control(output->backend, true);

	if (!output->backend->hwc_vsync_enabled && vsync_enable_tries < 5) {
		// Try again
		wl_event_source_timer_update(output->vsync_timer,
			output->frame_delay);
		vsync_enable_tries++;
	} else if (output->backend->hwc_vsync_enabled) {
		vsync_enable_tries = 0;
	}

	return 0;
}

static void on_vsync(struct wl_listener *listener, void *data) {
	struct wlr_hwcomposer_output *output = wl_container_of(listener,
		output, vsync_listener);
	wl_event_source_timer_update(output->frame_timer,
		output->backend->idle_time);
}

struct wlr_output *wlr_hwcomposer_add_output(struct wlr_backend *wlr_backend) {
	struct wlr_hwcomposer_backend *backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;

	struct wlr_hwcomposer_output *output =
		calloc(1, sizeof(struct wlr_hwcomposer_output));
	if (output == NULL) {
		wlr_log(WLR_ERROR, "Failed to allocate wlr_hwcomposer_output");
		return NULL;
	}
	output->backend = backend;
	wlr_output_init(&output->wlr_output, &backend->backend, &output_impl,
		backend->display);
	struct wlr_output *wlr_output = &output->wlr_output;

#ifdef HWC_DEVICE_API_VERSION_2_0
	if (backend->hwc_version == HWC_DEVICE_API_VERSION_2_0)
		output->egl_window = HWCNativeWindowCreate(
			backend->hwc_width, backend->hwc_height,
			HAL_PIXEL_FORMAT_RGBA_8888, hwcomposer2_present, backend);
	else
#endif
		output->egl_window = HWCNativeWindowCreate(
			backend->hwc_width, backend->hwc_height,
			HAL_PIXEL_FORMAT_RGBA_8888, output_present, backend);

	output->egl_display = eglGetDisplay(NULL);
	backend->egl.display = output->egl_display;

	output_set_custom_mode(wlr_output, backend->hwc_width,
		backend->hwc_height, backend->hwc_refresh);
	strncpy(wlr_output->make, "hwcomposer", sizeof(wlr_output->make));
	strncpy(wlr_output->model, "hwcomposer", sizeof(wlr_output->model));
	snprintf(wlr_output->name, sizeof(wlr_output->name), "HWCOMPOSER-%d",
		wl_list_length(&backend->outputs) + 1);

	if (!wlr_egl_make_current(&output->backend->egl, output->egl_surface,
			NULL)) {
		goto error;
	}

	wlr_renderer_begin(backend->renderer, wlr_output->width, wlr_output->height);
	wlr_renderer_clear(backend->renderer, (float[]){ 1.0, 1.0, 1.0, 1.0 });
	wlr_renderer_end(backend->renderer);

	struct wl_event_loop *ev = wl_display_get_event_loop(backend->display);
	output->frame_timer = wl_event_loop_add_timer(ev, signal_frame, output);
	output->vsync_timer = wl_event_loop_add_timer(ev, on_vsync_timer_elapsed, output);

	output->vsync_listener.notify = on_vsync;
	wl_signal_add(&backend->events.vsync, &output->vsync_listener);

	wl_list_insert(&backend->outputs, &output->link);

	if (backend->started) {
		wl_event_source_timer_update(output->vsync_timer, output->frame_delay);
		wlr_output_update_enabled(wlr_output, true);
		wlr_signal_emit_safe(&backend->backend.events.new_output, wlr_output);
	}

	return wlr_output;

error:
	wlr_output_destroy(&output->wlr_output);
	return NULL;
}
