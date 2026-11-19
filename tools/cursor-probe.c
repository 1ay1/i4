// tools/cursor-probe.c — a minimal client that draws its own cursor.
//
// Most toolkits now use wp_cursor_shape_v1 (a named shape); this one never
// binds it, and sets a 32x32 image with wl_pointer.set_cursor instead, so
// the image path through i4d and the head can be tested live:
//
//   wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xdg-shell.h
//   wayland-scanner private-code  /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xdg-shell.c
//   cc -O2 -I. tools/cursor-probe.c xdg-shell.c -lwayland-client -o cursor-probe
//   tools/live.sh -- ./cursor-probe
//
// It prints "cursor-probe: set_cursor serial N" each time it sets the
// cursor (on every pointer enter), and exits after the time given (s).
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#include <wayland-client.h>

#include "xdg-shell.h"

static struct wl_compositor* compositor;
static struct wl_shm* shm;
static struct wl_seat* seat;
static struct xdg_wm_base* wm_base;
static struct wl_surface* surface;
static struct wl_surface* cursor_surface;
static struct wl_buffer* cursor_buffer;
static int width = 320, height = 240;
static int configured;

static struct wl_buffer* buffer(int w, int h, uint32_t (*pixel)(int x, int y), uint32_t format) {
    const int stride = w * 4, size = stride * h;
    const int fd = memfd_create("cursor-probe", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0) exit(1);
    uint32_t* p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) exit(1);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) p[y * w + x] = pixel(x, y);
    munmap(p, (size_t)size);
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

// A 32x32 crosshair: opaque red bars on transparent, hotspot at the centre.
static uint32_t cross(int x, int y) { return (x == 15 || x == 16 || y == 15 || y == 16) ? 0xffff0000u : 0u; }
static uint32_t grey(int x, int y) { (void)x; (void)y; return 0xff303030u; }

static void ptr_enter(void* d, struct wl_pointer* p, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y) {
    (void)d; (void)s; (void)x; (void)y;
    wl_pointer_set_cursor(p, serial, cursor_surface, 16, 16);
    printf("cursor-probe: set_cursor serial %u\n", serial);
    fflush(stdout);
}
static void ptr_leave(void* d, struct wl_pointer* p, uint32_t s, struct wl_surface* f) { (void)d; (void)p; (void)s; (void)f; }
static void ptr_motion(void* d, struct wl_pointer* p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d; (void)p; (void)t; (void)x; (void)y; }
static void ptr_button(void* d, struct wl_pointer* p, uint32_t s, uint32_t t, uint32_t b, uint32_t st) {
    (void)d; (void)p; (void)s; (void)t; (void)b; (void)st;
}
static void ptr_axis(void* d, struct wl_pointer* p, uint32_t t, uint32_t a, wl_fixed_t v) { (void)d; (void)p; (void)t; (void)a; (void)v; }
static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button, .axis = ptr_axis};

static void seat_caps(void* d, struct wl_seat* s, uint32_t caps) {
    (void)d;
    if (caps & WL_SEAT_CAPABILITY_POINTER) wl_pointer_add_listener(wl_seat_get_pointer(s), &pointer_listener, NULL);
}
static const struct wl_seat_listener seat_listener = {.capabilities = seat_caps};

static void ping(void* d, struct xdg_wm_base* b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_base_listener = {ping};

static void xs_configure(void* d, struct xdg_surface* xs, uint32_t serial) {
    (void)d;
    xdg_surface_ack_configure(xs, serial);
    struct wl_buffer* b = buffer(width, height, grey, WL_SHM_FORMAT_XRGB8888);
    wl_surface_attach(surface, b, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    wl_surface_commit(surface);
    configured = 1;
}
static const struct xdg_surface_listener xdg_surface_listener = {xs_configure};

static void tl_configure(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* s) {
    (void)d; (void)t; (void)s;
    if (w > 0 && h > 0) { width = w; height = h; }
}
static void tl_close(void* d, struct xdg_toplevel* t) { (void)d; (void)t; exit(0); }
static const struct xdg_toplevel_listener toplevel_listener = {.configure = tl_configure, .close = tl_close};

static void global(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t v) {
    (void)d; (void)v;
    if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "wl_seat")) seat = wl_registry_bind(r, name, &wl_seat_interface, 1);  // v1: the listeners below are v1's
    else if (!strcmp(iface, "xdg_wm_base")) wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}
static void global_remove(void* d, struct wl_registry* r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener registry_listener = {global, global_remove};

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? atof(argv[1]) : 10.0;
    struct wl_display* dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "cursor-probe: no display\n"); return 1; }
    wl_registry_add_listener(wl_display_get_registry(dpy), &registry_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!compositor || !shm || !seat || !wm_base) { fprintf(stderr, "cursor-probe: missing globals\n"); return 1; }
    xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    wl_seat_add_listener(seat, &seat_listener, NULL);

    cursor_surface = wl_compositor_create_surface(compositor);
    cursor_buffer = buffer(32, 32, cross, WL_SHM_FORMAT_ARGB8888);
    wl_surface_attach(cursor_surface, cursor_buffer, 0, 0);
    wl_surface_damage_buffer(cursor_surface, 0, 0, 32, 32);
    wl_surface_commit(cursor_surface);

    surface = wl_compositor_create_surface(compositor);
    struct xdg_surface* xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xs, &xdg_surface_listener, NULL);
    struct xdg_toplevel* tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &toplevel_listener, NULL);
    xdg_toplevel_set_title(tl, "cursor-probe");
    wl_surface_commit(surface);

    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        const double left = seconds - ((double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) / 1e9);
        if (left <= 0) break;
        while (wl_display_prepare_read(dpy) != 0) wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);
        struct pollfd p = {wl_display_get_fd(dpy), POLLIN, 0};
        if (poll(&p, 1, (int)(left * 1000) + 1) > 0) {
            wl_display_read_events(dpy);
            wl_display_dispatch_pending(dpy);
        } else {
            wl_display_cancel_read(dpy);
        }
    }
    wl_display_disconnect(dpy);
    return 0;
}
