/* web: the film OS in a browser (mfilm.app), as WebAssembly.
 *
 * The same screens as the camera, drawn by the same code: this file is the
 * port, as sim/sim.c is on a PC and fork/src/filmroll.c is on the camera.
 * Around them it plays the part of the camera: LiveView with the HUD over
 * it, the roll and its log, the keys that open the film OS.
 *
 * The page (site/js/camera.js) runs it. fos_port_sleep waits on the page
 * (web.sleep, made to return later by Binaryen's Asyncify), so the film
 * OS's own loop runs as it does on the camera. The card is the page's
 * in-memory file system (WASI); the shutter writes a CR2 there (mkcr2.wasm,
 * sim/tools/mkcr2.c) from what LiveView shows, and the darkroom develops it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../fos.h"
#include "../fos_ui.h"
#include "../fos_dev.h"

#define IMPORT(name) __attribute__((import_module("web"), import_name(name)))
#define EXPORT(name) __attribute__((export_name(name)))

IMPORT("sleep")   void web_sleep(int ms, int presented);   /* asynchronous: Asyncify unwinds here */
IMPORT("now")     int  web_now();
IMPORT("present") void web_present(int y0, int y1);
IMPORT("palette") void web_palette();
IMPORT("mode")    void web_mode(int ui);                   /* 1: the film OS is up, 0: LiveView */
IMPORT("log")     void web_log(const char *msg);
IMPORT("clock")   void web_clock(char *buf, int size, int with_date);
IMPORT("event")   void web_event(int what, int a, int b);  /* the page's own reactions */

enum { EV_LOADED, EV_SHOT, EV_FINISHED, EV_REWOUND, EV_DEVELOPING, EV_WIND };

void sim_fonts();
void sim_films();

/* ---------------------------------------------------------------- what the page sees */

static uint8_t screen[FOS_W * FOS_H];
static uint32_t pal[256];                   /* 0xRRGGBB; the HUD's own, or the film OS's */
static uint8_t async_data[16 * 1024];       /* Asyncify's stack (its first 8 bytes: start, end) */

EXPORT("web_screen")  uint8_t *web_screen_ptr() { return screen; }
EXPORT("web_pal")     uint32_t *web_pal_ptr() { return pal; }
EXPORT("web_async")   uint8_t *web_async_ptr()
{
    ((uint32_t *) async_data)[0] = (uint32_t) (uintptr_t) (async_data + 8);
    ((uint32_t *) async_data)[1] = (uint32_t) (uintptr_t) (async_data + sizeof(async_data));
    return async_data;
}

/* ---------------------------------------------------------------- the camera's state */

static int settings[FS_COUNT] = {
    [FS_HUD] = 1, [FS_LEDS] = 1, [FS_FOCUS] = 1, [FS_WIND] = 0,
    [FS_APKEYS] = 1, [FS_LENS_WIDE] = 6, [FS_FSTOP] = 9,
    [FS_STARTUP] = 2, [FS_PLAYLOCK] = 1, [FS_BOOTANIM] = 1, [FS_DEBUG] = 0, [FS_MODE] = 0,
    [FS_FILM] = 0, [FS_EXP] = 2, [FS_SCR_FLIP] = -1, [FS_EXPSIM] = 1,
    [FS_ROLLLIM] = 3,       /* the demo keeps rolls (of 12 frames, as FS_EXP says): the camera's default is no rolls (0) */
};
static struct fos_roll roll;
static int file_no = 100;                   /* the card's last IMG_nnnn */
static int need_wind;

#define CARD "/card/"
#define DCIM CARD "DCIM/100CANON"

static int ui_running, open_req = -1, abort_req;
static int half_press, leds_until, end_due;

/* keys from the page, for the film OS or LiveView */
static int keyq[64], kq_head, kq_tail;
EXPORT("web_key") void web_key(int k)
{
    int next = (kq_tail + 1) % 64;
    if (next != kq_head) { keyq[kq_tail] = k; kq_tail = next; }
}
static int key_pop()
{
    if (kq_head == kq_tail) return FK_NONE;
    int k = keyq[kq_head];
    kq_head = (kq_head + 1) % 64;
    return k;
}

/* ---------------------------------------------------------------- the port */

int  fos_port_ms() { return web_now(); }
int  fos_port_key() { return key_pop(); }
int  fos_port_abort() { return abort_req; }

static int presented;
void fos_port_sleep(int ms)
{
    web_sleep(ms, presented);
    presented = 0;
}

void fos_port_present(const uint8_t *rows, int pitch, int y0, int y1)
{
    for (int y = y0; y < y1; y++)
        memcpy(&screen[y * FOS_W], &rows[(y - y0) * pitch], FOS_W);
    web_present(y0, y1);
    presented = 1;
}

/* the palette as the camera shows it (sim.c does the same) */
void fos_port_palette()
{
    for (int i = 0; i < 256; i++)
        pal[i] = i == 0 || i == 255 ? fos_pal[i] :
            settings[FS_SCR_STD] == 1 ? fos_pal[i] :
            fos_screen_rgb(fos_screen_yuv(i, 235, 0, settings[FS_SCR_SAT], settings[FS_SCR_WARM], settings[FS_SCR_BRIGHT]), 235);
    web_palette();
}

void fos_port_photo(const uint8_t (*p)[3], int n, int k)
{
    for (int i = 1; i < 255; i++)
    {
        const uint8_t *c = i <= n ? p[i - 1] : i >= FOS_FIXED_BASE && i < FOS_FIXED_BASE + FOS_FIXED_N ?
                           fos_fixed_rgb[i - FOS_FIXED_BASE] : (const uint8_t *) "\0\0\0";
        uint32_t rgb = (uint32_t) (c[0] * k >> 8) << 16 | (uint32_t) (c[1] * k >> 8) << 8 | (uint32_t) (c[2] * k >> 8);
        pal[i] = settings[FS_SCR_STD] == 1 ? rgb :
            fos_screen_rgb(fos_screen_yuv_rgb(rgb, 235, 0, settings[FS_SCR_SAT], settings[FS_SCR_WARM], settings[FS_SCR_BRIGHT]), 235);
    }
    web_palette();
}

void fos_port_log(const char *msg) { web_log(msg); }
/* how much the darkroom develops per frame: the browser is many times
 * faster than the camera, and a roll done in a second shows nothing of the
 * darkroom. The page sets the pace (web_dev_pace); 0 is as fast as it goes */
static int dev_cap = 36;          /* about a second a frame */
EXPORT("web_dev_pace") void web_dev_pace(int cap) { dev_cap = cap; }
int  fos_port_dev_cap() { return dev_cap; }
void fos_port_stat(const struct fos_stat *s) { }

void *fos_port_alloc(int size) { return malloc(size); }
void fos_port_free(void *p) { free(p); }

void fos_port_roll(struct fos_roll *r) { *r = roll; }

int fos_port_get(int s)
{
    return s >= 0 && s < FS_COUNT ? settings[s] : 0;
}
void fos_port_set(int s, int v)
{
    if (s == FS_SCR_FLIP || s == FS_EXPSIM) return;
    if (s >= 0 && s < FS_COUNT) settings[s] = v;
    if (s == FS_SCR_STD || s == FS_SCR_SAT || s == FS_SCR_WARM || s == FS_SCR_BRIGHT) fos_port_palette();
}

static void log_append(const char *line)
{
    char path[64];
    snprintf(path, sizeof(path), DCIM "/ROLL_%03d.TXT", roll.number);
    FILE *f = fopen(path, "ab");
    if (f) { fputs(line, f); fclose(f); }
}

static void load_roll(int film, int exposures)
{
    roll.loaded = 1;
    roll.number++;
    roll.film = film;
    roll.exposures = exposures;
    roll.shot = 0;
    roll.nrecent = 0;
    need_wind = 0;
    web_clock(roll.loaded_at, sizeof(roll.loaded_at), 1);
    char head[200];
    snprintf(head, sizeof(head), "# ML film mode roll log (filmroll module)\nroll = %03d\nfilm = %s\nexposures = %d\nloaded = %s:00\n\n",
             roll.number, fos_films[film].key, exposures, roll.loaded_at);
    log_append(head);
    web_event(EV_LOADED, film, exposures);
}

static void rewind_roll()
{
    if (!roll.loaded) return;
    if (roll.shot < roll.exposures)
    {
        char line[96], when[20];
        web_clock(when, sizeof(when), 1);
        snprintf(line, sizeof(line), "\n# rewound %s after %d of %d frames\n", when, roll.shot, roll.exposures);
        log_append(line);
    }
    roll.loaded = 0;
    web_event(EV_REWOUND, 0, 0);
}

void fos_port_action(int a, int x, int y)
{
    if (a == FA_LOAD) load_roll(x, fos_exposure_choices[y]);
    if (a == FA_REWIND) rewind_roll();
    if (a == FA_BUSY) web_event(EV_DEVELOPING, x, 0);
}

void fos_port_clock(char *buf, int size) { web_clock(buf, size, 0); }
int  fos_port_lens_manual() { return 1; }
const char *fos_port_lens_name() { return "EF-M 22mm f/2 STM"; }
#ifndef MFILM_VERSION
#define MFILM_VERSION "web"
#endif
const char *fos_port_version() { return MFILM_VERSION; }

/* ---------------------------------------------------------------- the shutter */

#define FINISHED (roll.loaded && roll.shot >= roll.exposures)

/* the file number the next frame gets, 0 if the shutter can't fire now
 * (no roll, the roll finished, the wind lever locking it) */
EXPORT("web_shot_begin") int web_shot_begin()
{
    if (ui_running || !roll.loaded || FINISHED) return 0;
    if (need_wind && settings[FS_WIND] == 2) return 0;
    return file_no + 1;
}

/* the page has written IMG_nnnn.CR2: the frame goes in the roll's log */
EXPORT("web_shot_end") void web_shot_end()
{
    file_no++;
    roll.shot++;
    char when[20], aperture[8], line[112];
    web_clock(when, sizeof(when), 1);
    fos_format_fstop(aperture, sizeof(aperture), settings[FS_FSTOP]);
    snprintf(line, sizeof(line), "%d  IMG_%04d.CR2  %s:00  f/%s\n", roll.shot, file_no, when, aperture);
    log_append(line);

    memmove(&roll.recent[1], &roll.recent[0], sizeof(roll.recent[0]) * (FOS_RECENT - 1));
    roll.recent[0].no = roll.shot;
    web_clock(roll.recent[0].time, sizeof(roll.recent[0].time), 0);
    fos_copy(roll.recent[0].fstop, sizeof(roll.recent[0].fstop), aperture, 5);
    if (roll.nrecent < FOS_RECENT) roll.nrecent++;

    if (settings[FS_WIND]) need_wind = 1;
    leds_until = web_now() + 1500;
    web_event(EV_SHOT, roll.shot, roll.exposures);
    if (FINISHED)
    {
        end_due = web_now() + 1200;
        web_event(EV_FINISHED, 0, 0);
    }
}

/* half-press: the LEDs light, and the film OS closes as on the camera */
EXPORT("web_half") void web_half(int down)
{
    half_press = down;
    if (down && ui_running) abort_req = 1;
}

/* the page asks for a screen (its own buttons, the guide) */
EXPORT("web_open") void web_open(int open) { if (!ui_running) open_req = open; }

EXPORT("web_roll_shot")      int web_roll_shot() { return roll.shot; }
EXPORT("web_roll_exposures") int web_roll_exposures() { return roll.loaded ? roll.exposures : 0; }
EXPORT("web_roll_film")      const char *web_roll_film() { return roll.loaded ? fos_films[roll.film].key : ""; }
EXPORT("web_ui_running")     int web_ui_running() { return ui_running; }

/* ---------------------------------------------------------------- LiveView */

/* the HUD's colours, at indices of its own (the camera has Canon's) */
static struct fos_hud_palette hud_pal;

static void hud_palette()
{
    memset(pal, 0, sizeof(pal));
    hud_pal.clear = 0;
    hud_pal.black = 1;  pal[1] = 0x000000;
    hud_pal.white = 2;  pal[2] = 0xFFFFFF;
    hud_pal.red = 3;    pal[3] = 0xFF2A1A;
    hud_pal.dark_red = 4; pal[4] = 0x5A0E08;
    for (int i = 0; i <= 100; i++)
    {
        int v = i * 255 / 100;
        hud_pal.gray[i] = (uint8_t) (5 + i);
        pal[5 + i] = (uint32_t) (v << 16 | v << 8 | v);
    }
    web_palette();
}

static int meter_wobble;
static void hud_draw(int now)
{
    memset(screen, hud_pal.clear, sizeof(screen));
    if (settings[FS_HUD] && roll.loaded)
    {
        struct fos_hud h;
        memset(&h, 0, sizeof(h));
        h.lit = !settings[FS_LEDS] || half_press || now < leds_until;
        h.blink = (now / 250) & 1;
        h.fstop = settings[FS_FSTOP];
        h.widest = settings[FS_LENS_WIDE];
        if (h.lit)
        {
            h.meter = 1;
            h.want = fos_clamp(h.fstop + meter_wobble, 0, fos_nfstops - 1);
        }
        static const char *speeds[] = { "60", "125", "250", "500" };
        snprintf(h.shutter, sizeof(h.shutter), "%s", speeds[fos_clamp(settings[FS_FSTOP] / 4, 0, 3)]);
        h.frame = roll.shot + 1;
        h.exposures = roll.exposures;
        h.finished = FINISHED;
        h.need_wind = need_wind;
        h.focus_screen = settings[FS_FOCUS];
        h.film = fos_films[fos_clamp(roll.film, 0, fos_nfilms - 1)].name;
        h.iso = fos_films[fos_clamp(roll.film, 0, fos_nfilms - 1)].box_iso;
        fos_hud_draw(screen, FOS_W, &h, &hud_pal);
    }
    web_present(0, FOS_H);
}

static void open_ui(int open)
{
    ui_running = 1;
    abort_req = 0;
    web_mode(1);
    fos_ui_run(open);
    ui_running = 0;
    abort_req = 0;
    kq_head = kq_tail = 0;
    hud_palette();
    web_mode(0);
}

/* a key on the plain LiveView screen, as filmroll.c's lv_key */
static void lv_key(int k)
{
    if (k & FK_RELEASE) return;
    if (k == FK_HOME || k == FK_MENU) { open_req = FOS_OPEN_HOME; return; }
    if (k == FK_PLAY) { open_req = FOS_OPEN_PRINTS; return; }
    if (!roll.loaded)
    {
        if (k == FK_SET) open_req = FOS_OPEN_SHELF;
        return;
    }
    if (!FINISHED && settings[FS_APKEYS] == 1 && (k == FK_UP || k == FK_DOWN || k == FK_WHEEL_L || k == FK_WHEEL_R))
    {
        int dir = k == FK_UP || k == FK_WHEEL_R ? 1 : -1;
        settings[FS_FSTOP] = fos_clamp(settings[FS_FSTOP] + dir, settings[FS_LENS_WIDE], fos_nfstops - 1);
        leds_until = web_now() + 2000;
        return;
    }
    if (FINISHED)
    {
        if (k == FK_SET) open_req = FOS_OPEN_END;
        return;
    }
    if (need_wind && k == FK_SET)
    {
        need_wind = 0;
        leds_until = web_now() + 1500;
        web_event(EV_WIND, 0, 0);
    }
}

EXPORT("web_main") void web_main()
{
    static int started;
    if (!started)
    {
        started = 1;
        fos_ui_init();
        sim_fonts();
        sim_films();
        open_req = FOS_OPEN_BOOT;
    }
    for (;;)
    {
        int now = web_now();
        if (end_due && now > end_due && FINISHED && !half_press)
        {
            end_due = 0;
            open_req = FOS_OPEN_END;
        }
        if (open_req >= 0)
        {
            int o = open_req;
            open_req = -1;
            /* the camera starts on the shelf (Start screen); the boot animation leads to it */
            open_ui(o);
            continue;
        }
        int k;
        while ((k = key_pop()) != FK_NONE) lv_key(k);
        if ((now / 700) % 4 == 0) meter_wobble = (now / 2800) % 3 - 1;
        hud_draw(now);
        web_sleep(60, 1);
    }
}

/* ---------------------------------------------------------------- the card */

static void card_init()
{
    /* one roll log is enough for the darkroom to know the card */
    roll.number = 0;
    roll.loaded = 0;
}

EXPORT("web_init") void web_init()
{
    card_init();
    hud_palette();
}
