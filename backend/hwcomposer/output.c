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
#include <wlr/util/region.h>
#include <wlr/util/log.h>
#include "backend/hwcomposer.h"
#include "util/time.h"
#include "types/wlr_output.h"

#include <sys/epoll.h>
#include <sys/timerfd.h>

static void schedule_frame(struct wlr_hwcomposer_output *output) {
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
	//
	// If the should_destroy flag is set, schedule the timer a bit farther,
	// we don't care about syncronization anymore anyway.
	if (output->should_destroy) {
		scheduled_next = next_vsync + (display_refresh * 3);
	} else if ((next_vsync - time) <= output->hwc_backend->idle_time) {
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

static bool output_commit(struct wlr_output *wlr_output,
		const struct wlr_output_state *state) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;
	struct wlr_hwcomposer_backend *hwc_backend = output->hwc_backend;

	bool should_schedule_frame = false;

	if (output->should_destroy) {
		return false;
	}


	if (state->committed & WLR_OUTPUT_STATE_MODE) {
		struct wlr_hwcomposer_mode *selected = NULL;

		if (state->mode_type == WLR_OUTPUT_STATE_MODE_FIXED && state->mode) {
			selected = wl_container_of(state->mode, selected, wlr_mode);
		} else if (state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
			for (size_t i = 0; i < output->hwc_mode_count; ++i) {
				struct wlr_hwcomposer_mode *candidate = &output->hwc_modes[i];
				if (candidate->wlr_mode.width == state->custom_mode.width &&
						candidate->wlr_mode.height == state->custom_mode.height &&
						(state->custom_mode.refresh == 0 || candidate->wlr_mode.refresh == state->custom_mode.refresh)) {
					selected = candidate;
					break;
				}
			}
		}

		if (!selected) {
			wlr_log(WLR_ERROR, "output_commit: requested HWC mode not found");
			return false;
		}
		if (!hwc_backend->impl->set_mode || !hwc_backend->impl->set_mode(output, selected->hwc_config_id)) {
			wlr_log(WLR_ERROR, "output_commit: failed to switch HWC config %u", selected->hwc_config_id);
			return false;
		}

		output->hwc_refresh = selected->hwc_vsync_period;
		output->frame_delay = 1000000 / MAX(1, selected->wlr_mode.refresh);
		if (output->hwc_is_primary)
			hwc_backend->hwc_device_refresh = selected->hwc_vsync_period;
		wlr_log(WLR_INFO, "output_commit: switched HWC config=%u %dx%d@%d mHz",
			selected->hwc_config_id, selected->wlr_mode.width, selected->wlr_mode.height, selected->wlr_mode.refresh);
	}
if (state->committed & WLR_OUTPUT_STATE_ENABLED) {
		wlr_log(WLR_DEBUG, "output_commit: STATE_ENABLE, pending state %d", state->enabled);
		if (!hwc_backend->impl->set_power_mode(output, state->enabled)) {
			wlr_log(WLR_ERROR, "output_commit: unable to change display power mode");
			return false;
		}

		// Start timer so that we can let hwc initialize
		if (state->enabled && wl_event_source_timer_update(output->vsync_timer,
			output->frame_delay) != 0) {
			wlr_log(WLR_ERROR, "Unable to restart vsync timer");
		}
	}

	if (!wlr_output->enabled) {
		return true;
	}

	if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
		const pixman_region32_t *damage = NULL;
		if (state->committed & WLR_OUTPUT_STATE_DAMAGE) {
			damage = &state->damage;
		}

		if (output->egl_window && output->wlr_output.renderer) {
			if (!wlr_renderer_swap_buffers(output->wlr_output.renderer, damage, &output->wlr_output)){
				wlr_log(WLR_ERROR, "wlr_renderer_swap_buffers failed");
				return false;
			}
			should_schedule_frame = true;
		}
	}

	if (should_schedule_frame || !state->committed || state->committed & WLR_OUTPUT_STATE_TRANSFORM) {
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

	if (output->vsync_timer) {
		wl_event_source_remove(output->vsync_timer);
	}

	//wlr_egl_destroy_surface(&hwc_backend->egl, output->egl_surface);

	hwc_backend->impl->destroy_output(output);
}

static const struct wlr_drm_format_set *output_get_formats(
	struct wlr_output *wlr_output, uint32_t buffer_caps) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;
	struct wlr_hwcomposer_backend *hwc_backend = output->hwc_backend;

	return &hwc_backend->shm_formats;
}

static const struct wlr_output_impl output_impl = {
	.destroy = output_destroy,
	.commit = output_commit,
	.get_primary_formats = output_get_formats,
};

bool wlr_output_is_hwcomposer(struct wlr_output *wlr_output) {
	return wlr_output->impl == &output_impl;
}

static int signal_frame(int fd, uint32_t mask, void *data) {
	struct wlr_hwcomposer_output *output = data;

	uint64_t res;
	if (read(fd, &res, sizeof(res)) > 0 && !output->should_destroy) {
		wlr_output_send_frame(&output->wlr_output);
	} else if (output->should_destroy) {
		wlr_output_destroy(&output->wlr_output);
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

void wlr_hwcomposer_output_schedule_destroy(struct wlr_output *wlr_output) {
	struct wlr_hwcomposer_output *output =
		(struct wlr_hwcomposer_output *)wlr_output;

	// Set should_destroy flag
	output->should_destroy = true;

	// Schedule a new frame. This shouldn't be racy since the only other
	// place where we schedule a new frame is during commits - so in
	// the worst case the timer would be rearmed (but since the flag
	// is set the callback would still destroy the output).
	schedule_frame(output);
}

struct wlr_output *wlr_hwcomposer_add_output(struct wlr_backend *wlr_backend,
	uint64_t display, bool primary_display) {

	struct wlr_hwcomposer_backend *hwc_backend =
		(struct wlr_hwcomposer_backend *)wlr_backend;

	struct wlr_hwcomposer_output *output = hwc_backend->impl->add_output(hwc_backend, display);
	output->hwc_backend = hwc_backend;
	struct wlr_output *wlr_output = &output->wlr_output;
	wl_list_insert(&hwc_backend->outputs, &output->link);

	output->should_destroy = false;
	output->hwc_display_id = display;
	output->hwc_is_primary = primary_display;
	int32_t refresh = output->hwc_refresh ? (1000000000000LL / output->hwc_refresh) : 0;
	if (refresh <= 0) {
		refresh = HWCOMPOSER_DEFAULT_REFRESH;
	}
	output->frame_delay = 1000000 / refresh;

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, output->hwc_width, output->hwc_height, refresh);
	wlr_output_init(&output->wlr_output, &hwc_backend->backend, &output_impl,
					hwc_backend->display, &state);
	wlr_output_state_finish(&state);

	struct wlr_hwcomposer_mode *preferred = NULL;
	for (size_t i = 0; i < output->hwc_mode_count; ++i) {
		struct wlr_hwcomposer_mode *mode = &output->hwc_modes[i];
		wl_list_insert(wlr_output->modes.prev, &mode->wlr_mode.link);
		if (mode->wlr_mode.preferred)
			preferred = mode;
	}
	if (preferred) {
		wlr_output->current_mode = &preferred->wlr_mode;
		wlr_output->width = preferred->wlr_mode.width;
		wlr_output->height = preferred->wlr_mode.height;
		wlr_output->refresh = preferred->wlr_mode.refresh;
	}
	wlr_log(WLR_INFO, "wlr_hwcomposer_add_output width=%d height=%d refresh=%d idle_time=%ld",
			output->hwc_width, output->hwc_height, refresh, hwc_backend->idle_time);

	output->wlr_output.phys_width = output->hwc_phys_width;
	output->wlr_output.phys_height = output->hwc_phys_height;
	wlr_output->make = malloc(64 * sizeof(char));
	wlr_output->model = malloc(64 * sizeof(char));
	wlr_output->name = malloc(64 * sizeof(char));
	strncpy(wlr_output->make, "hwcomposer", 64);
	strncpy(wlr_output->model, "hwcomposer", 64);
	snprintf(wlr_output->name, 64, "HWCOMPOSER-%ld", display + 1);

	output->egl_window = HWCNativeWindowCreate(
		output->hwc_width, output->hwc_height,
		HAL_PIXEL_FORMAT_RGBA_8888, hwc_backend->impl->present, output);
	HWCNativeWindowSetBufferCount(output->egl_window, 3);

	struct wl_event_loop *ev = wl_display_get_event_loop(hwc_backend->display);
	output->vsync_timer = wl_event_loop_add_timer(ev, on_vsync_timer_elapsed, output);

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
		wlr_output_state_set_enabled(&state, true);
		wl_signal_emit_mutable(&hwc_backend->backend.events.new_output, wlr_output);
	}

	return wlr_output;
}
