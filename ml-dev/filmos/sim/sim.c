/* sim: runs the film OS on a PC with a scripted dial, and saves what the
 * camera screen would show, frame by frame.
 *
 *   ./sim script.txt out/frames/name     (scripts in scenes/)
 *
 * A script line is "<ms> <key>" (key names below) or "<ms> end"; "#" starts
 * a comment. "roll <film> <shot> <exp>" before the first event puts a roll in
 * the camera, "open home|shelf|boot" picks the first screen.
 * Time is simulated: every presented frame costs FRAME_MS, so the result
 * plays at roughly the pace the camera would manage.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../fos.h"
#include "../fos_ui.h"
#include "../fos_dev.h"

void sim_fonts();             /* fonts.c */
void sim_films();             /* films.c */

#define FRAME_MS 22

static int now_ms;
static int frame_no;
static int frame_dirty;
static int last_dump = -1000;
static const char *outdir;
static uint8_t screen[FOS_W * FOS_H];

struct ev { int t; int key; };
static struct ev evs[256];
static int nev, next_ev, end_t = 4000;

static int settings[FS_COUNT] = {
    [FS_HUD] = 1, [FS_LEDS] = 1, [FS_FOCUS] = 1, [FS_WIND] = 1,
    [FS_APKEYS] = 0, [FS_LENS_WIDE] = 6, [FS_FSTOP] = 10,
    [FS_STARTUP] = 2, [FS_PLAYLOCK] = 1, [FS_BOOTANIM] = 1, [FS_DEBUG] = 0, [FS_MODE] = 0,
    [FS_FILM] = 0, [FS_EXP] = 0, [FS_SCR_FLIP] = 0, [FS_EXPSIM] = 1,
    /* the camera's default is 0 (no rolls); the simulator starts with a limit of 36 so the scenes that were
     * written for rolls still show them. FOS_ROLLLIM=0 or a scene's "limit 0" shows the camera's default */
    [FS_ROLLLIM] = 1,
};
static struct fos_roll roll = { 1, 12, 1, 36, 14, 0, "2026-10-21 18:02", 3,
    { { 14, "21:47", "2" }, { 13, "21:40", "2" }, { 12, "21:33", "2.8" } } };

int fos_port_ms() { return now_ms; }

/* the palette on the screen now, 0xRRGGBB; each dumped frame gets a copy */
static uint32_t cur_pal[256];

/* FOS_OVERDRAW (below): the rows "Canon" scribbled on, not painted again yet */
static uint8_t overdrawn_row[FOS_H];
static int overdraw_wait, overdraw_n, overdraw_missed, overdraw_shown;

static void dump()
{
    char path[512];
    /* a frame that shows a scribble still there (FOS_OVERDRAW) */
    for (int y = 0; y < FOS_H; y++) if (overdrawn_row[y]) { overdraw_shown++; break; }
    if (!strcmp(outdir, "-")) { frame_no++; last_dump = now_ms; return; }   /* measuring only */
    snprintf(path, sizeof(path), "%s/f_%05d_%06d.idx", outdir, frame_no, now_ms);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(screen, 1, sizeof(screen), f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/f_%05d_%06d.pal", outdir, frame_no++, now_ms);
    f = fopen(path, "wb");
    if (f) { fwrite(cur_pal, 4, 256, f); fclose(f); }
    last_dump = now_ms;
}

/* FOS_OVERDRAW=1: "Canon" scribbles on the screen every 700 ms, as the 60D
 * and 550D do, and the port tells the film OS which rows (fos_ui_overdrawn),
 * as the camera's check does. The film OS puts them back at once, or in a
 * screen change with its next frames: every scribbled row must be painted
 * again within three frames, or the screen on show doesn't repaint all of itself. */
static void overdraw_step()
{
    static int on = -1, last = 0;
    if (on < 0) on = getenv("FOS_OVERDRAW") != 0;
    if (!on) return;
    if (overdraw_wait && --overdraw_wait == 0)
    {
        int y0 = -1, y1 = -1;
        for (int y = 0; y < FOS_H; y++) if (overdrawn_row[y]) { if (y0 < 0) y0 = y; y1 = y; }
        if (y0 >= 0)
        {
            overdraw_missed++;
            fprintf(stderr, "[%6d ms] overdraw: rows %d-%d not painted again\n", now_ms, y0, y1);
            memset(overdrawn_row, 0, sizeof(overdrawn_row));
        }
    }
    if (overdraw_wait || now_ms - last < 700) return;
    last = now_ms;
    int y0 = 40 + (overdraw_n * 97) % 360, x0 = 60 + (overdraw_n * 151) % 480;
    for (int y = y0; y < y0 + 30; y++)
    {
        memset(&screen[y * FOS_W + x0], 255, 160);
        overdrawn_row[y] = 1;
    }
    overdraw_n++;
    overdraw_wait = 3;
    fos_ui_overdrawn(y0, y0 + 30);
}

void fos_port_sleep(int ms)
{
    /* FOS_FULL=1: every frame redrawn whole, to check the partial redraws */
    static int full = -1;
    if (full < 0) full = getenv("FOS_FULL") != 0;
    if (full) ui_clobbered = 1;
    overdraw_step();
    if (frame_dirty)
    {
        now_ms += FRAME_MS;
        if (now_ms - last_dump >= 30) dump();
        frame_dirty = 0;
    }
    now_ms += ms;
}

int fos_port_key()
{
    if (next_ev < nev && evs[next_ev].t <= now_ms)
        return evs[next_ev++].key;
    return FK_NONE;
}

int fos_port_abort()
{
    return now_ms > end_t;
}

void fos_port_present(const uint8_t *rows, int pitch, int y0, int y1)
{
    for (int y = y0; y < y1; y++)
    {
        memcpy(&screen[y * FOS_W], &rows[(y - y0) * pitch], FOS_W);
        overdrawn_row[y] = 0;
    }
    frame_dirty = 1;
}

/* the palette as the camera would show it: through the screen colour
 * settings and Canon's conversion, and back */
void fos_port_palette()
{
    char path[512];
    if (!strcmp(outdir, "-")) return;
    uint32_t pal[256];
    for (int i = 0; i < 256; i++)
        pal[i] = i == 0 || i == 255 ? fos_pal[i] :
            settings[FS_SCR_STD] == 1 ? fos_pal[i] :   /* Rec.709: what a BT.601 screen shows is not modelled */
            fos_screen_rgb(fos_screen_yuv(i, 235, 0, settings[FS_SCR_SAT], settings[FS_SCR_WARM], settings[FS_SCR_BRIGHT]), 235);
    memcpy(cur_pal, pal, sizeof(cur_pal));
    frame_dirty = 1;
    snprintf(path, sizeof(path), "%s/../../pal.bin", outdir);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(pal, 4, 256, f); fclose(f); }
}

void fos_port_photo(const uint8_t (*pal)[3], int n, int k)
{
    for (int i = 1; i < 255; i++)
    {
        const uint8_t *c = i <= n ? pal[i - 1] : i >= FOS_FIXED_BASE && i < FOS_FIXED_BASE + FOS_FIXED_N ?
                           fos_fixed_rgb[i - FOS_FIXED_BASE] : (const uint8_t *) "\0\0\0";
        uint32_t rgb = (uint32_t) (c[0] * k >> 8) << 16 | (uint32_t) (c[1] * k >> 8) << 8 | (uint32_t) (c[2] * k >> 8);
        cur_pal[i] = settings[FS_SCR_STD] == 1 ? rgb :
            fos_screen_rgb(fos_screen_yuv_rgb(rgb, 235, 0, settings[FS_SCR_SAT], settings[FS_SCR_WARM], settings[FS_SCR_BRIGHT]), 235);
    }
    frame_dirty = 1;
}

void fos_port_log(const char *msg) { fprintf(stderr, "[%6d ms] %s\n", now_ms, msg); }
int  fos_port_dev_cap() { return 12; }

void *fos_port_alloc(int size) { return malloc(size); }
void fos_port_free(void *p) { free(p); }

void fos_port_roll(struct fos_roll *r) { *r = roll; r->no_rolls = settings[FS_ROLLLIM] == 0; }
int  fos_port_get(int s)
{
    /* the screen orientation is only on swivel screens: not on the EOS M, unless FOS_SWIVEL=1 */
    if (s == FS_SCR_FLIP && !getenv("FOS_SWIVEL")) return -1;
    /* exposure simulation: FOS_NOEXPSIM=1 is a camera Magic Lantern can't set it on (600D, 1100D) */
    if (s == FS_EXPSIM && getenv("FOS_NOEXPSIM")) return -1;
    return s >= 0 && s < FS_COUNT ? settings[s] : 0;
}
void fos_port_set(int s, int v)
{
    int old = s >= 0 && s < FS_COUNT ? settings[s] : 0;
    if (s >= 0 && s < FS_COUNT) settings[s] = v;
    if (s == FS_SCR_STD || s == FS_SCR_SAT || s == FS_SCR_WARM || s == FS_SCR_BRIGHT) fos_port_palette();
    /* as the camera does: the roll ends, and with no rolls (0) a new one is loaded at once, same film */
    if (s == FS_ROLLLIM && v != old)
    {
        roll.loaded = v == 0;
        roll.shot = 0; roll.total = 0; roll.exposures = FOS_SEGMENT;
        if (v == 0) roll.number++;
    }
}

void fos_port_action(int a, int x, int y)
{
    fprintf(stderr, "[%6d ms] action %d (%d, %d)\n", now_ms, a, x, y);
    if (a == FA_LOAD)
    {
        int none = settings[FS_ROLLLIM] == 0;
        /* no rolls: the film the camera has already changes nothing */
        if (!(none && roll.loaded && roll.film == x))
        {
            roll.loaded = 1; roll.number++; roll.film = x;
            roll.exposures = none ? FOS_SEGMENT : fos_exposure_choices[settings[FS_ROLLLIM] - 1];
            roll.shot = 0; roll.nrecent = 0; roll.total = 0;
            strcpy(roll.loaded_at, "2026-10-22 09:41");
        }
    }
    if (a == FA_REWIND)
    {
        if (settings[FS_ROLLLIM] == 0) { roll.number++; roll.shot = 0; }      /* no rolls: the next one starts at once */
        else roll.loaded = 0;
    }
    if (a == FA_BUSY) fprintf(stderr, "[%6d ms] %s\n", now_ms, x ? "busy: developing" : "busy: done");
}

void fos_port_clock(char *buf, int size) { snprintf(buf, size, "09:41"); }
int  fos_port_lens_manual() { return 1; }
const char *fos_port_lens_name() { return "Manual lens"; }
#ifndef MFILM_VERSION
#define MFILM_VERSION "dev"
#endif
const char *fos_port_version() { return MFILM_VERSION; }
void fos_port_stat(const struct fos_stat *s) { }

static int key_of(const char *s)
{
    static const struct { const char *n; int k; } names[] = {
        { "left", FK_LEFT }, { "right", FK_RIGHT }, { "up", FK_UP }, { "down", FK_DOWN },
        { "wl", FK_WHEEL_L }, { "wr", FK_WHEEL_R }, { "set", FK_SET }, { "menu", FK_MENU },
        { "info", FK_INFO }, { "play", FK_PLAY }, { "home", FK_HOME },
    };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(s, names[i].n)) return names[i].k;
    fprintf(stderr, "unknown key %s\n", s);
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: sim script outdir\n"); return 1; }
    outdir = argv[2];
    if (strcmp(outdir, "-")) mkdir(outdir, 0755);
    if (getenv("FOS_MODE")) settings[FS_MODE] = atoi(getenv("FOS_MODE"));     /* 1: Realistic */
    if (getenv("FOS_ROLLLIM")) settings[FS_ROLLLIM] = atoi(getenv("FOS_ROLLLIM"));  /* 0: no rolls (the camera's default), 1..3: 36, 24, 12 */
    int open = FOS_OPEN_SHELF;
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 1; }
    char line[256];
    int hold = 0;
    while (fgets(line, sizeof(line), f))
    {
        char a[64], b[64];
        int t, n1, n2, n3, n4;
        if (line[0] == '#' || line[0] == '\n') continue;
        /* roll <film> <shot> <exposures> [<number> [<total>]]: the roll in the camera; total is the count with
         * no rolls (limit 0): the frames shot on this film since it was chosen, over the hidden rolls */
        int n5 = 0;
        int nr = sscanf(line, "roll %d %d %d %d %d", &n1, &n2, &n3, &n4, &n5);
        if (nr >= 3)
        {
            roll.loaded = 1; roll.film = n1; roll.shot = n2; roll.exposures = n3;
            roll.total = nr == 5 ? n5 : n2;
            if (nr >= 4) roll.number = n4;
            continue;
        }
        if (!strncmp(line, "noroll", 6)) { roll.loaded = 0; continue; }
        /* limit <n>: the roll limit setting (0 Off = no rolls, 1..3 = 36, 24, 12) */
        if (sscanf(line, "limit %d", &n1) == 1) { settings[FS_ROLLLIM] = n1; continue; }
        if (sscanf(line, "open %63s", a) == 1)
        {
            open = !strcmp(a, "home") ? FOS_OPEN_HOME : !strcmp(a, "boot") ? FOS_OPEN_BOOT :
                   !strcmp(a, "darkroom") ? FOS_OPEN_DARKROOM : !strcmp(a, "prints") ? FOS_OPEN_PRINTS :
                   !strcmp(a, "end") ? FOS_OPEN_END : FOS_OPEN_SHELF;
            continue;
        }
        if (sscanf(line, "%d %63s", &t, b) == 2)
        {
            if (!strcmp(b, "end")) { end_t = t; continue; }
            int k = key_of(b);
            evs[nev].t = t; evs[nev].key = k; nev++;
            /* the dial's presses come with a release, like on the camera;
             * "hold" before a key keeps it down for 1.5 s */
            if (k >= FK_LEFT && k <= FK_DOWN)
            {
                evs[nev].t = t + (hold ? 1500 : 110); evs[nev].key = k | FK_RELEASE; nev++;
            }
            hold = 0;
        }
        else if (sscanf(line, "%d hold", &t) == 1) hold = 1;
    }
    fclose(f);
    for (int i = 1; i < nev; i++)
        for (int j = i; j > 0 && evs[j].t < evs[j - 1].t; j--)
        {
            struct ev tmp = evs[j]; evs[j] = evs[j - 1]; evs[j - 1] = tmp;
        }
    fos_ui_init();
    sim_fonts();
    sim_films();
    fos_port_palette();
    fos_ui_run(open);
    if (frame_dirty) dump();
    fprintf(stderr, "%d frames, %d ms\n", frame_no, now_ms);
    if (overdraw_n) fprintf(stderr, "overdraw: %d scribbles, %d not painted again, %d frames showed one\n", overdraw_n, overdraw_missed, overdraw_shown);
    return 0;
}
