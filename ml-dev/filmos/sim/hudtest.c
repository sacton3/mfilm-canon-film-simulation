/* hudtest: the viewfinder HUD over a made-up LiveView picture, with a
 * stand-in for Canon's palette (white 1, black 2, red 8, dark red 12,
 * greys 38..79)
 *   hudtest [1|0|e|p][f] [liveview.ppm]   the picture: a 720 x 480 binary PPM;
 *   p: the top plate a DSLR shows instead when you shoot through the viewfinder;
 *   f after it: no rolls (roll limit Off): a count of 143, no "/36", no frames-left bar */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../fos.h"

void sim_fonts();             /* fonts.c */

void *fos_port_alloc(int size) { return malloc(size); }
void fos_port_free(void *p) { free(p); }

static uint8_t ov[480 * 960];

int main(int argc, char **argv)
{
    struct fos_hud_palette p = { 0, 2, 1, 8, 12, { 0 } };
    for (int i = 0; i <= 100; i++) p.gray[i] = 38 + i * 41 / 100;
    struct fos_hud h = { 1, 1, 1, 13, 6, 15, "125", 12, 36, 0, 1, 1, "PORTRA 400" };
    if (argc > 1 && argv[1][0] == '0') { h.lit = 0; h.need_wind = 1; }
    if (argc > 1 && argv[1][0] == 'e') { h.finished = 1; strcpy(h.shutter, "0.5\""); h.want = 30; }
    h.iso = 400;
    if (argc > 1 && argv[1][0] && argv[1][1] == 'f') { h.no_rolls = 1; h.frame = 143; }
    fos_gfx_init();
    sim_fonts();
    if (argc > 1 && argv[1][0] == 'p') { h.want = h.fstop + 2; fos_hud_plate(ov, 960, &h, &p); }
    else fos_hud_draw(ov, 960, &h, &p);

    static uint8_t lv[480 * 720 * 3];
    int have_lv = 0;
    if (argc > 2)
    {
        FILE *pf = fopen(argv[2], "rb");
        int w = 0, ht = 0, mx = 0;
        if (pf && fscanf(pf, "P6 %d %d %d", &w, &ht, &mx) == 3 && w == 720 && ht == 480 && mx == 255 && fgetc(pf) != EOF)
            have_lv = fread(lv, 1, sizeof(lv), pf) == sizeof(lv);
        if (!have_lv) fprintf(stderr, "%s: not a 720 x 480 PPM, using the made-up picture\n", argv[2]);
        if (pf) fclose(pf);
    }

    /* a dusk street as LiveView: sky gradient, a sun, dark buildings */
    FILE *f = fopen("out/hud.ppm", "wb");
    fprintf(f, "P6 720 480 255\n");
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 720; x++)
        {
            int r, g, b;
            float t = y / 480.0f;
            r = (int) (230 - 120 * t); g = (int) (150 - 70 * t); b = (int) (120 + 40 * t);
            float d = sqrtf((x - 250.0f) * (x - 250.0f) + (y - 230.0f) * (y - 230.0f));
            if (d < 40) { r = 255; g = 230; b = 190; }
            int roof = 300 + (int) (30 * sinf(x / 37.0f)) + ((x / 90) % 2) * 40;
            if (y > roof) { r = 40; g = 34; b = 38; }
            if (have_lv) { const uint8_t *q = lv + (y * 720 + x) * 3; r = q[0]; g = q[1]; b = q[2]; }
            int k = ov[y * 960 + x];
            if (k == 1) r = g = b = 255;
            else if (k == 2) r = g = b = 0;
            else if (k == 8) { r = 240; g = 30; b = 30; }
            else if (k == 12) { r = 120; g = 12; b = 12; }
            else if (k >= 38 && k <= 79) r = g = b = (k - 38) * 255 / 41;
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    fclose(f);
    return 0;
}
