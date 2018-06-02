#ifndef WLR_BACKEND_HWCOMPOSER_H
#define WLR_BACKEND_HWCOMPOSER_H

#include <wlr/backend.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_output.h>

/**
 * Creates a hwcomposer backend. A hwcomposer backend has no outputs or inputs by
 * default.
 */
struct wlr_backend *wlr_hwcomposer_backend_create(struct wl_display *display,
	wlr_renderer_create_func_t create_renderer_func);
/**
 * Create a new hwcomposer output backed by an in-memory EGL framebuffer. You can
 * read pixels from this framebuffer via wlr_renderer_read_pixels but it is
 * otherwise not displayed.
 */
struct wlr_output *wlr_hwcomposer_add_output(struct wlr_backend *backend,
	unsigned int width, unsigned int height);
/**
 * Creates a new input device. The caller is responsible for manually raising
 * any event signals on the new input device if it wants to simulate input
 * events.
 */
struct wlr_input_device *wlr_hwcomposer_add_input_device(
	struct wlr_backend *backend, enum wlr_input_device_type type);
bool wlr_backend_is_hwcomposer(struct wlr_backend *backend);
bool wlr_input_device_is_hwcomposer(struct wlr_input_device *device);
bool wlr_output_is_hwcomposer(struct wlr_output *output);

#endif
