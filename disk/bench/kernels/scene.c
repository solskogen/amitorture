// SPDX-License-Identifier: 0BSD
//
// A landscape with an off-center sun over two ridges, rippled water and a
// boat, rendered per pixel (benchwork's scene-2d; its quantize-floyd remap
// is not on the bench disk). This is what a software renderer looks like:
// a dozen 32-bit multiplies and divides per pixel, a handful of
// data-dependent branches, and byte stores. The scene is p96cts's
// backdrop, with the RastPort blit removed.
//
// Nothing is vectorizable and nothing hoists, so what is left is how well the
// compiler schedules plain integer code and keeps the loops' invariants in
// registers.

#include <exec/types.h>

#include "bench.h"

#define WIDTH KS_SCENE_W
#define HEIGHT KS_SCENE_H

// Integer hash of one value, for the terrain outline. Knuth's multiplicative
// constant; the shift picks bits that vary quickly.
static int hash8(int i) {
    unsigned int u = (unsigned int)i * 2654435761U;
    return (int)((u >> 13) & 0xFF);
}

// A ridge line: hashed control points every `step` pixels, linearly
// interpolated. Irregular and non-repeating, unlike a sine.
static int ridge(int x, int seed, int amp, int base, int step) {
    int i = x / step, f = x % step;
    int a = hash8(i + seed) * amp / 255;
    int b = hash8(i + 1 + seed) * amp / 255;
    return base - (a + (b - a) * f / step);
}

static UBYTE clamp8(int v) {
    return (UBYTE)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Render the scene into px, w*h*3 bytes of R8G8B8.
static void render(UBYTE *px, SHORT w, SHORT h) {
    int horizon = h * 3 / 5;
    int sun_x = w * 7 / 10, sun_y = h * 7 / 25, sun_r = h / 7;
    int boat_x = w / 4, boat_y = horizon + h / 5;
    int boat_w = w / 6, boat_h = h / 12;
    SHORT x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int dx = x - sun_x, dy = y - sun_y;
            int r, g, b;
            ULONG p;

            if (y < horizon) {
                // Sky: deep blue overhead warming to orange at the horizon.
                int t = y * 255 / horizon;
                r = 40 + t * 200 / 255;
                g = 60 + t * 120 / 255;
                b = 150 - t * 100 / 255;
                if (dx * dx + dy * dy < sun_r * sun_r) {
                    r = 255;
                    g = 230;
                    b = 120;
                }
            } else {
                // Water: the sky's horizon tone, darkening with depth, with
                // ripples that break up every row differently.
                int t = (y - horizon) * 255 / (h - horizon);
                // Ripples: horizontal bars whose phase shifts per row, so the
                // water varies along both axes without looking like noise.
                int rip = ((x + hash8(y) / 8) / 3 & 7) * 5;
                r = 120 - t * 80 / 255 + rip;
                g = 90 - t * 60 / 255 + rip;
                b = 110 + t * 40 / 255 + rip;
                // Sun's reflection, wobbling as it goes down the water.
                if (x > sun_x - sun_r / 2 + hash8(y * 3) / 16 - 8 &&
                    x < sun_x + sun_r / 2 + hash8(y * 3) / 16 - 8) {
                    r += 90;
                    g += 70;
                }
            }

            // Two ridges, the near one darker, both drawn over the sky.
            if (y > ridge(x, 11, h / 5, horizon, w / 10) && y < horizon) {
                r = 90;
                g = 70;
                b = 90;
            }
            if (y > ridge(x, 77, h / 8, horizon, w / 16) && y < horizon) {
                r = 45;
                g = 40;
                b = 55;
            }

            // A boat: hull, then mast and sail.
            if (y >= boat_y && y < boat_y + boat_h &&
                x >= boat_x + (y - boat_y) && x < boat_x + boat_w - (y - boat_y)) {
                r = 30;
                g = 20;
                b = 25;
            }
            if (x >= boat_x + boat_w / 2 && x < boat_x + boat_w / 2 + 2 &&
                y < boat_y && y > boat_y - boat_h * 3)
                r = g = b = 20;
            if (y < boat_y && y > boat_y - boat_h * 3 &&
                x > boat_x + boat_w / 2 + 2 &&
                x < boat_x + boat_w / 2 + 2 + (y - (boat_y - boat_h * 3)) / 2) {
                r = 230;
                g = 225;
                b = 210;
            }

            p = ((ULONG)y * w + x) * 3;
            px[p] = clamp8(r);
            px[p + 1] = clamp8(g);
            px[p + 2] = clamp8(b);
        }
    }
}

static UBYTE *pixels;

static void scene_teardown(void *state) {
    (void)state;
    bench_free(pixels);
    pixels = NULL;
}

static void *scene_setup(void) {
    pixels = bench_alloc((ULONG)WIDTH * HEIGHT * 3, "scene pixels");
    return pixels;
}

static const void *scene_run(void *state) {
    render(state, WIDTH, HEIGHT);
    return state;
}

static bool scene_verify(void *state, const void *result, ULONG *check) {
    (void)state;
    *check = checksum(0, result, (ULONG)WIDTH * HEIGHT * 3);
    return true;
}

const struct Bench bench_scene_2d = {
    /* .name= */ "scene-2d",
    /* .desc= */ "a landscape rendered per pixel",
    /* .group= */ GROUP_GRAPHICS,
    /* .unit= */ "px",
    /* .per_loop= */ (double)WIDTH * HEIGHT,
    /* .setup= */ scene_setup,
    /* .run= */ scene_run,
    /* .verify= */ scene_verify,
    /* .teardown= */ scene_teardown,
};
