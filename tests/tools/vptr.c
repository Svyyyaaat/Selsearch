// SPDX-License-Identifier: MIT
// Dev tool: drive a wlroots virtual pointer for UI tests in a headless compositor.
//   vptr OUTPUT_W OUTPUT_H cmd...   with cmds:
//   move X Y | down | up | click | rdown | rup | sleep MS | drag X1 Y1 X2 Y2 STEPS
//   circle CX CY R STEPS
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client.h"

static struct zwlr_virtual_pointer_manager_v1 *mgr;
static struct wl_seat *seat;
static struct wl_display *dpy;
static struct zwlr_virtual_pointer_v1 *ptr;
static unsigned W, H;

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
    (void)d; (void)v;
    if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
        mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name) && !seat)
        seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg = {global, global_remove};

static uint32_t now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static void move(double x, double y)
{
    zwlr_virtual_pointer_v1_motion_absolute(ptr, now(), (uint32_t)x, (uint32_t)y, W, H);
    zwlr_virtual_pointer_v1_frame(ptr);
    wl_display_flush(dpy);
    usleep(8000);
}
static void button(uint32_t b, int down)
{
    zwlr_virtual_pointer_v1_button(ptr, now(), b, down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(ptr);
    wl_display_flush(dpy);
    usleep(30000);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: vptr W H cmd...\n"); return 1; }
    W = atoi(argv[1]); H = atoi(argv[2]);
    dpy = wl_display_connect(NULL);
    if (!dpy) return 1;
    struct wl_registry *r = wl_display_get_registry(dpy);
    wl_registry_add_listener(r, &reg, NULL);
    wl_display_roundtrip(dpy);
    if (!mgr) { fprintf(stderr, "no virtual pointer manager\n"); return 1; }
    ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
    for (int i = 3; i < argc; ++i) {
        const char *c = argv[i];
        if (!strcmp(c, "move")) { double x = atof(argv[++i]), y = atof(argv[++i]); move(x, y); }
        else if (!strcmp(c, "down")) button(BTN_LEFT, 1);
        else if (!strcmp(c, "up")) button(BTN_LEFT, 0);
        else if (!strcmp(c, "click")) { button(BTN_LEFT, 1); button(BTN_LEFT, 0); }
        else if (!strcmp(c, "rdown")) button(BTN_RIGHT, 1);
        else if (!strcmp(c, "rup")) button(BTN_RIGHT, 0);
        else if (!strcmp(c, "sleep")) usleep(atoi(argv[++i]) * 1000);
        else if (!strcmp(c, "drag")) {
            double x1 = atof(argv[++i]), y1 = atof(argv[++i]), x2 = atof(argv[++i]), y2 = atof(argv[++i]);
            int steps = atoi(argv[++i]);
            move(x1, y1); button(BTN_LEFT, 1);
            for (int s = 1; s <= steps; ++s) move(x1 + (x2 - x1) * s / steps, y1 + (y2 - y1) * s / steps);
            button(BTN_LEFT, 0);
        } else if (!strcmp(c, "circle")) {
            double cx = atof(argv[++i]), cy = atof(argv[++i]), rad = atof(argv[++i]);
            int steps = atoi(argv[++i]);
            move(cx + rad, cy); button(BTN_LEFT, 1);
            for (int s = 1; s <= steps; ++s) {
                double a = 2 * M_PI * 1.05 * s / steps;
                move(cx + rad * cos(a), cy + rad * 0.6 * sin(a));
            }
            button(BTN_LEFT, 0);
        }
    }
    wl_display_roundtrip(dpy);
    zwlr_virtual_pointer_v1_destroy(ptr);
    wl_display_disconnect(dpy);
    return 0;
}
