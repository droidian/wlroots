#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/param.h>
#include <sys/cdefs.h> // for __BEGIN_DECLS/__END_DECLS found in sync.h
#include <sync/sync.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>

#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/util/log.h>
#include "backend/hwcomposer.h"
#include "util/signal.h"

#include "time.h"
#include <sys/epoll.h>
#include <sys/timerfd.h>

static void schedule_frame(struct wlr_hwcomposer_output *output) {
	struct wlr_output *wlr_output =
		(struct wl_output *)output;

	int64_t time, display_refresh, next_vsync, scheduled_next;
	struct timespec now, frame_tspec;

	clock_gettime(CLOCK_MONOTONIC, &now);

	time = (now.tv_sec * 1000000000LL) +  now.tv_nsec;
	display_refresh = MIN(output->hwc_refresh, output->hwc_backend->hwc_device_refresh);
	next_vsync = output->hwc_backend->hwc_vsync_last_timestamp + display_refresh;

	// We need to schedule the frame render so that it can be hopefully
	// be swapped before the next vsync.
	//
	// If the delta of the current time and the predicted next vsync
	// is too close, we won't make it, so defer the render on the next
	// cycle.
	if ((next_vsync - time) <= output->hwc_backend->idle_time) {
		// Too close! Sad
		scheduled_next = next_vsync + display_refresh - output->hwc_backend->idle_time;
	} else {
		// We can go ahead
		scheduled_next = next_vsync - output->hwc_backend->idle_time;
	}

	timespec_from_nsec(&frame_tspec, scheduled_next);

	struct itimerspec frame_interval = {
		.it_interval = {0},
		.it_value = frame_tspec,
	};

	if (timerfd_settime(output->vsync_timer_fd, TFD_TIMER_ABSTIME, &frame_interval, NULL) == -1) {
		wlr_log(WLR_ERROR, "Failed to arm timer, errno %d", errno);
	}
}

static bool output_set_custom_mode(struct wlr_output *wlr_output, int32_t width,
		int32_t height, int32_t refresh) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;
	struct wlr_hwcomposer_backend *hwc_backend = output->hwc_backend;

	wlr_log(WLR_INFO, "output_set_custom_mode width=%d height=%d refresh=%d idle_time=%d",
		width, height, refresh, hwc_backend->idle_time);

	if (refresh <= 0) {
		refresh = HWCOMPOSER_DEFAULT_REFRESH;
	}

	wlr_egl_destroy_surface(&hwc_backend->egl, output->egl_surface);

	output->egl_surface = eglCreateWindowSurface(hwc_backend->egl.display,
		hwc_backend->egl.config, (EGLNativeWindowType)output->egl_window, NULL);
	if (output->egl_surface == EGL_NO_SURFACE) {
		wlr_log(WLR_ERROR, "Failed to recreate EGL surface");
		wlr_output_destroy(wlr_output);
		return false;
	}
	wlr_log(WLR_DEBUG, "set_custom_mode: surface created");

	output->frame_delay = 1000000 / refresh;

	wlr_output_update_custom_mode(&output->wlr_output, width, height, refresh);
	return true;
}

static bool output_test(struct wlr_output *wlr_output) {
	if ((wlr_output->pending.committed & WLR_OUTPUT_STATE_BUFFER) &&
		(wlr_output->pending.buffer_type & WLR_OUTPUT_STATE_BUFFER_SCANOUT)) {
		/* Direct scan-out not supported yet */
		return false;
	}

	return true;
}

static bool output_commit(struct wlr_output *wlr_output) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;
	struct wlr_hwcomposer_backend *hwc_backend = output->hwc_backend;

	bool should_schedule_frame = false;

	if (wlr_output->pending.committed & WLR_OUTPUT_STATE_ENABLED) {
		wlr_log(WLR_DEBUG, "output_commit: STATE_ENABLE, pending state %d", wlr_output->pending.enabled);

		if (!hwc_backend->impl->set_power_mode(output, wlr_output->pending.enabled)) {
			wlr_log(WLR_ERROR, "output_commit: unable to change display power mode");
			return false;
		}

		// Start timer so that we can let hwc initialize
		if (wlr_output->enabled && wl_event_source_timer_update(output->vsync_timer,
			output->frame_delay) != 0) {
			wlr_log(WLR_ERROR, "Unable to restart vsync timer");
		}
	}

	if (!wlr_output->enabled) {
		return true;
	}

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
			if (!wlr_egl_swap_buffers(&hwc_backend->egl,
					output->egl_surface, damage)) {
				return false;
			}
			should_schedule_frame = true;
			break;
		case WLR_OUTPUT_STATE_BUFFER_SCANOUT:
			wlr_log(WLR_ERROR, "WLR_OUTPUT_STATE_BUFFER_SCANOUT not implemented");
			break;
		}
	}

	wlr_egl_unset_current(&hwc_backend->egl);

	if (should_schedule_frame) {
		// FIXME: wlroots submits a presentation event with commit_seq =
		//  output_commit_seq + 1. For some unknown reason, we aren't
		// off-by-one and the output commit sequence won't match the feedback's,
		// thus presentation feedback will not be reported to the client.
		// Also we should check why there appears a "ghost" presentation
		// event just after the good one.
		struct wlr_output_event_present present_event = {
			.output = &output->wlr_output,
			.commit_seq = output->wlr_output.commit_seq,
		};
		wlr_output_send_present(&output->wlr_output, &present_event);
		schedule_frame(output);
	}

	return true;
}

static bool output_attach_render(struct wlr_output *wlr_output, int *buffer_age) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;
	return wlr_egl_make_current(&output->hwc_backend->egl, output->egl_surface,
		buffer_age);
}

static bool output_handle_damage(struct wlr_output *wlr_output, pixman_region32_t *damage) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;

	if (damage == NULL) {
		return true;
	}

	/*
	 * HACK: transform the damaged area to take in account output
	 * transformations.
	 *
	 * This being here is wrong, as it should be a job of the compositor
	 * and we're doing an assumption on behalf of them.
	 */

	int width, height;
	wlr_output_transformed_resolution(wlr_output, &width, &height);

	pixman_region32_t frame_damage;
	pixman_region32_init(&frame_damage);

	wlr_region_transform(&frame_damage, damage,
		wlr_output_transform_invert(wlr_output->transform), width, height);

	if (wlr_output->needs_frame) {
		/*
		 * By checking for needs_frame, we're predicting the compositor's
		 * behaviour, and it's horribly wrong.
		 *
		 * Unfortunately, the alternative would be to damage the whole
		 * region when rollbacks happen.
		 *
		 * This will probably also break the "damage-tracking" debug
		 * phoc feature.
		*/
		return wlr_egl_set_damage_region(&output->hwc_backend->egl,
			output->egl_surface, &frame_damage);
	}

	return true;
}

static void output_destroy(struct wlr_output *wlr_output) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;
	struct wlr_hwcomposer_backend *hwc_backend = output->hwc_backend;

	// Disable vsync
	hwc_backend->impl->vsync_control(output, false);

	if (output->vsync_event) {
		wl_event_source_remove(output->vsync_event);
	}

	if (output->vsync_timer_fd >= 0 && close(output->vsync_timer_fd) == -1) {
		wlr_log(WLR_ERROR, "Unable to close vsync timer fd!");
	}

	wl_list_remove(&output->link);

	wl_event_source_remove(output->vsync_timer);

	wlr_egl_destroy_surface(&hwc_backend->egl, output->egl_surface);

	hwc_backend->impl->destroy_output(output);
}

static void output_rollback_render(struct wlr_output *wlr_output) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;
	wlr_egl_unset_current(&output->hwc_backend->egl);
}

static const struct wlr_output_impl output_impl = {
	.destroy = output_destroy,
	.attach_render = output_attach_render,
	.handle_damage = output_handle_damage,
	.commit = output_commit,
	.rollback_render = output_rollback_render,
	.test = output_test,
};

bool wlr_output_is_hwcomposer(struct wlr_output *wlr_output) {
	return wlr_output->impl == &output_impl;
}

static int signal_frame(int fd, uint32_t mask, void *data) {
	struct wlr_hwcomposer_output *output = data;

	uint64_t res;
	if (read(fd, &res, sizeof(res)) > 0) {
		wlr_output_send_frame(&output->wlr_output);
	}

	return 0;
}

static int on_vsync_timer_elapsed(void *data) {
	struct wlr_hwcomposer_output *output = data;
	struct wlr_hwcomposer_backend *hwc_backend = output->hwc_backend;
	static int vsync_enable_tries = 0;
	bool vsync_enabled;

	// Ensure vsync gets enabled
	vsync_enabled = hwc_backend->impl->vsync_control(output, true);

	if (vsync_enabled && vsync_enable_tries < 5) {
		// Try again
		if (wl_event_source_timer_update(output->vsync_timer,
				output->frame_delay) == 0) {
			vsync_enable_tries++;
		}
	} else if (vsync_enabled) {
		vsync_enable_tries = 0;
		schedule_frame(output);
	}

	return 0;
}

struct wlr_output *wlr_hwcomposer_add_output(struct wlr_backend *wlr_backend,
	uint64_t display, bool primary_display) {

	struct wlr_hwcomposer_backend *hwc_backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;

	struct wlr_hwcomposer_output *output = hwc_backend->impl->add_output(hwc_backend, display);
	output->hwc_backend = hwc_backend;
	wlr_output_init(&output->wlr_output, &hwc_backend->backend, &output_impl,
		hwc_backend->display);
	struct wlr_output *wlr_output = &output->wlr_output;

	output->hwc_display_id = display;
	output->hwc_is_primary = primary_display;

	output->egl_window = HWCNativeWindowCreate(
		output->hwc_width, output->hwc_height,
		HAL_PIXEL_FORMAT_RGBA_8888, hwc_backend->impl->present, output);

	output->egl_display = hwc_backend->egl.display;

	output_set_custom_mode(wlr_output, output->hwc_width,
		output->hwc_height,
		output->hwc_refresh ?
			(1000000000000LL / output->hwc_refresh) :
			0);
	strncpy(wlr_output->make, "hwcomposer", sizeof(wlr_output->make));
	strncpy(wlr_output->model, "hwcomposer", sizeof(wlr_output->model));
	snprintf(wlr_output->name, sizeof(wlr_output->name), "HWCOMPOSER-%d",
		display + 1);
	if (!wlr_egl_make_current(&hwc_backend->egl, output->egl_surface,
			NULL)) {
		goto error;
	}

	wlr_renderer_begin(hwc_backend->renderer, wlr_output->width, wlr_output->height);
	wlr_renderer_clear(hwc_backend->renderer, (float[]){ 1.0, 1.0, 1.0, 1.0 });
	wlr_renderer_end(hwc_backend->renderer);

	struct wl_event_loop *ev = wl_display_get_event_loop(hwc_backend->display);
	output->vsync_timer = wl_event_loop_add_timer(ev, on_vsync_timer_elapsed, output);

	wl_list_insert(&hwc_backend->outputs, &output->link);

	output->vsync_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (output->vsync_timer_fd < 0) {
		wlr_log(WLR_ERROR, "Failed to create vsync timer fd");
		return NULL;
	}
	output->vsync_event = wl_event_loop_add_fd(ev, output->vsync_timer_fd,
		WL_EVENT_READABLE, signal_frame, output);
	if (!output->vsync_event) {
		wlr_log(WLR_ERROR, "Failed to create vsync event source");
		return NULL;
	}

	if (!hwc_backend->impl->set_power_mode(output, true)) {
		wlr_log(WLR_ERROR, "output: unable to power on display!");
	}

	if (hwc_backend->started) {
		wl_event_source_timer_update(output->vsync_timer, output->frame_delay);
		wlr_output_update_enabled(wlr_output, true);
		wlr_signal_emit_safe(&hwc_backend->backend.events.new_output, wlr_output);
	}

	return wlr_output;

error:
	wlr_output_destroy(&output->wlr_output);
	return NULL;
}
