#define WLR_USE_UNSTABLE
#include <wayland-server.h>
#include <wlr/util/log.h>
#include <wlr/backend.h>
#include <assert.h>

int main()
{
    struct wl_display *wld;

    wlr_log_init(WLR_DEBUG, NULL);
    wld = wl_display_create();
    assert(wlr_backend_autocreate(wld, NULL));
    return 0;
}
