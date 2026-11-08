/* fos: the film OS - the EOS M's own user interface on top of Magic Lantern.
 *
 * Everything the user sees (film shelf, home, settings, roll status,
 * transitions) lives in fos_ui.c and draws with fos_gfx. It talks to the
 * camera only through the fos_port_* functions below, which filmroll.c
 * implements on the camera and sim/sim.c implements on a PC.
 */

#ifndef _fos_h_
#define _fos_h_

#include "fos_gfx.h"
#include <stdio.h>
#include <string.h>

#define FOS_NAME "M" T_DOT "FILM"

/* the first n characters of src (fewer if it is shorter) into dst: the
 * camera's snprintf is Canon's, which doesn't know "%.5s" */
static inline char *fos_copy(char *dst, int size, const char *src, int n)
{
    int i = 0;
    for (; i < n && i < size - 1 && src[i]; i++) dst[i] = src[i];
    if (size > 0) dst[i] = 0;
    return dst;
}

/* ---------------------------------------------------------------- films */

#define PS_STANDARD   0x81      /* raw PROP_PICTURE_STYLE values */
#define PS_LANDSCAPE  0x83
#define PS_MONO       0x86

/* the shelves the films stand on */
enum { FOS_GROUP_COLOUR, FOS_GROUP_SLIDE, FOS_GROUP_BW, FOS_GROUP_CINE, FOS_NGROUPS };
extern const char *const fos_group_names[FOS_NGROUPS];

/* a canister label drawn from parts (the films from ML/DATA/FOSFILMS.BIN;
 * the built-in ones have their own painting in fos_film.c). An ink is a
 * ramp and a level. */
enum { FOS_LABEL_BAND, FOS_LABEL_CAP, FOS_LABEL_STRIPES };
struct fos_label
{
    uint8_t style;
    uint8_t leader;             /* level of a grey (B&W) leader, 0 = colour film */
    uint8_t bg[2], band[2], accent[2], text[2], num[2];
    char top[20];               /* small, above the name */
    char word[14];              /* the name */
    char num_s[8];              /* big: digits and H, P, T */
    char sub[18];               /* small, under the number */
};

struct fos_film
{
    const char *key;            /* must match films/films.py */
    const char *brand;
    const char *name;
    const char *process;        /* "C-41", "E-6", "B&W" */
    const char *desc;
    int box_iso;                /* rated speed */
    int iso;                    /* what the camera is set to (EOS M has no ISO 50) */
    int picstyle;               /* in-camera preview only; the look comes from the lab */
    int contrast, saturation, color_tone, sharpness;
    int kelvin;
    int wbs_gm;                 /* + green, - magenta (-9..9) */
    int wbs_ba;                 /* + amber, - blue    (-9..9) */
    uint8_t grain, punch, colour, warmth;       /* 1..5, for the shelf */
    const char *shortname;      /* in folder names: "Portra400" */
    int group;                  /* FOS_GROUP_* */
    const struct fos_label *label;              /* NULL: painted by hand */
};

/* the built-in films, then those from ML/DATA/FOSFILMS.BIN once loaded */
extern const struct fos_film *fos_films;
extern int fos_nfilms;
#define FOS_FILM_FILE "FOSFILMS.BIN"
/* blob: the file's contents, kept while the films are used; 1 when usable */
int fos_films_load(const uint8_t *blob, int size);

extern const int fos_exposure_choices[];        /* 36, 24, 12 */
#define FOS_NEXP 3

/* f-numbers x100 in 1/3 stops; see fos_film.c */
extern const int fos_fstops[];
extern const int fos_nfstops;
void fos_format_fstop(char *buf, int size, int index);

/* ---------------------------------------------------------------- keys */

enum
{
    FK_NONE,
    FK_LEFT, FK_RIGHT, FK_UP, FK_DOWN,      /* the four presses of the rear dial */
    FK_WHEEL_L, FK_WHEEL_R,                 /* turning it */
    FK_SET, FK_MENU, FK_INFO, FK_PLAY,
    FK_HOME,                                /* what used to open the ML menu */
    FK_RELEASE = 0x100,                     /* or-ed in: a press above ended */
};

/* ---------------------------------------------------------------- settings */

enum
{
    FS_HUD, FS_LEDS, FS_FOCUS, FS_WIND,
    FS_APKEYS, FS_LENS_WIDE, FS_FSTOP,
    FS_STARTUP, FS_PLAYLOCK, FS_BOOTANIM, FS_DEBUG,
    FS_MODE,            /* film mode: 0 Basic (a finished roll goes straight to the darkroom), 1 Realistic (rewind it first) */
    FS_FILM, FS_EXP,
    FS_SCR_STD,         /* screen colour: 0 Canon's conversion, 1 Rec.709 */
    FS_SCR_SAT, FS_SCR_WARM, FS_SCR_BRIGHT,     /* -FOS_SCR_STEPS..FOS_SCR_STEPS */
    FS_SCR_FLIP,        /* swivel screens: 0 as Canon's hinge says, 1 normal, 2 upside down, 3 mirrored;
                           the port answers -1 on a camera that can't, and the item isn't shown */
    FS_EXPSIM,          /* LiveView's exposure simulation: 1 on (the exposure the frame gets), 0 off (bright
                           enough to focus a manual lens); -1 where Magic Lantern can't set it: not shown */
    FS_ROLLLIM,         /* roll limit: 0 Off (no rolls, the default), 1 = 36, 2 = 24, 3 = 12 frames (an index
                           into fos_exposure_choices, plus one). Off: the camera is always loaded and the screens
                           say nothing of rolls; behind them it keeps a roll of FOS_SEGMENT frames and starts the
                           next one by itself, same film, so the darkroom and the logs are the ones of a roll */
    FS_COUNT
};

#define FOS_SEGMENT 36          /* the roll the camera keeps behind the screens when the roll limit is Off */
#define FOS_SCR_STEPS 6

enum
{
    FA_LOAD,            /* a = film, b = exposure choice */
    FA_REWIND,          /* end the roll in the camera */
    FA_REAPPLY,         /* film settings again (after changing WB by hand) */
    FA_CANON_MENU,      /* leave and open Canon's menu */
    FA_BUSY,            /* a = 1: long work (developing) begins, keep the camera awake; 0: done */
};

#define FOS_RECENT 5
struct fos_roll
{
    int loaded;
    int number;
    int film;
    int exposures;
    int shot;
    int need_wind;
    char loaded_at[20];         /* "2026-08-08 09:10" */
    int nrecent;                /* newest first */
    struct { int no; char time[6]; char fstop[6]; } recent[FOS_RECENT];
    /* after recent, so the simulator's positional initialiser still fits */
    int no_rolls;                   /* the roll limit is Off: show a count, not a roll (see FS_ROLLLIM) */
    int total;                  /* then: frames shot on this film since it was chosen, over the hidden rolls; recent[].no counts the same way */
};

/* screens fos_ui_run can open on; BOOT_LV plays the boot animation and
 * goes straight back to the camera */
enum { FOS_OPEN_SHELF, FOS_OPEN_HOME, FOS_OPEN_BOOT, FOS_OPEN_BOOT_LV,
       FOS_OPEN_PRINTS,     /* PLAY: the latest prints */
       FOS_OPEN_END,        /* the roll has just been finished */
       FOS_OPEN_DARKROOM };

/* ---------------------------------------------------------------- the port */

int  fos_port_ms();
void fos_port_sleep(int ms);
int  fos_port_key();                        /* next key event, FK_NONE if none */
int  fos_port_abort();                      /* leave right away (half-shutter, power off) */
void fos_port_present(const uint8_t *rows, int pitch, int y0, int y1);  /* rows = row y0 */
void fos_port_palette();                    /* load fos_pal[1..254] */
void *fos_port_alloc(int size);             /* big, temporary buffers */
void fos_port_free(void *p);
/* files on the card, for developing; fos_port_open returns NULL on failure */
void *fos_port_open(const char *path, int write);
int  fos_port_read(void *f, void *buf, int size);          /* bytes read */
int  fos_port_write(void *f, const void *buf, int size);   /* bytes written */
int  fos_port_seek(void *f, int offset);                   /* from the start; < 0 on failure */
void fos_port_close(void *f);
const char *fos_port_card();                                /* the card's root, ending in '/' */
/* every entry of a directory, until fn returns 0; < 0 if it can't be read */
int  fos_port_list(const char *dir, int (*fn)(void *ctx, const char *name, int is_dir), void *ctx);
int  fos_port_mkdir(const char *path);                      /* 0 on success or if it exists */
/* a file, or an empty folder (never called on one with anything in it);
 * 0 once it isn't there, removed or never there */
int  fos_port_remove(const char *path);

void fos_port_roll(struct fos_roll *r);
int  fos_port_get(int setting);
void fos_port_set(int setting, int value);
void fos_port_action(int action, int a, int b);
void fos_port_clock(char *buf, int size);   /* "14:32" */
int  fos_port_lens_manual();
const char *fos_port_lens_name();
const char *fos_port_version();
const char *fos_port_model();               /* "Canon EOS M": the camera it runs on */
/* photographs: palette entries 1..n become pal's colours at k/256 of
 * their brightness, FOS_FIXED_BASE.. the fixed overlay colours (fos_dev.h);
 * fos_port_palette() goes back to the film OS's own */
void fos_port_photo(const uint8_t (*pal)[3], int n, int k);
void fos_port_log(const char *msg);         /* the debug log, if it is on */
/* developing works in slices until a time budget is spent; the simulator's
 * clock stands still meanwhile, so it caps the slices instead (0: no cap) */
int  fos_port_dev_cap();

/* frame times, summed over about a second of animation; open_ms is how
 * long the film OS took to show its first frame (first report only) */
struct fos_stat { int frames, draw_ms, present_ms, rest_ms, worst_ms, open_ms, cans_ms; };   /* cans_ms: part of draw_ms */
void fos_port_stat(const struct fos_stat *s);   /* keep it, don't log while animating */

/* ---------------------------------------------------------------- the viewfinder HUD */

/* what the HUD shows; fos_hud.c draws it with Canon's palette */
struct fos_hud
{
    int lit;                    /* LEDs on (half-press, or always) */
    int blink;                  /* 0/1, toggles a few times a second */
    int meter;                  /* the meter has a reading */
    int fstop, widest, want;    /* indices into fos_fstops; want = what the meter asks for */
    char shutter[12];           /* "125", "2\"", "0.5\"" */
    int frame, exposures, finished, need_wind;
    int focus_screen;
    const char *film;
    int iso;                    /* the film's speed (the top plate shows it) */
    int no_rolls;                   /* the roll limit is Off: frame is a count since the film was chosen; no "/N", no frames-left bar */
};

struct fos_hud_palette
{
    uint8_t clear, black, white, red, dark_red;
    uint8_t gray[101];          /* percent -> index */
};

void fos_hud_draw(uint8_t *vram, int pitch, const struct fos_hud *h, const struct fos_hud_palette *p);
void fos_hud_clear(uint8_t *vram, int pitch, const struct fos_hud_palette *p);
/* the top plate: a DSLR's back screen while you shoot through the viewfinder */
void fos_hud_plate(uint8_t *vram, int pitch, const struct fos_hud *h, const struct fos_hud_palette *p);
void fos_hud_plate_clear(uint8_t *vram, int pitch, const struct fos_hud_palette *p);
/* the same a band at a time, to put together off-screen: screen rows y0 .. y0 + n
 * into buf, whose first row is y0. The HUD leaves what it doesn't draw on as it
 * was; fos_hud_spans says where in row y it draws (x0, x1 pairs; returns 2 or 4) */
void fos_hud_draw_rows(uint8_t *buf, int pitch, int y0, int n, const struct fos_hud *h, const struct fos_hud_palette *p);
void fos_hud_plate_rows(uint8_t *buf, int pitch, int y0, int n, const struct fos_hud *h, const struct fos_hud_palette *p);
int  fos_hud_spans(int y, int x[4]);

/* ---------------------------------------------------------------- the UI */

void fos_ui_init();
void fos_ui_run(int screen);
/* the port saw something else draw on rows y0 .. y1 of the screen: they are put back at once */
void fos_ui_overdrawn(int y0, int y1);

/* canister art, fos_film.c; quality 2 = everything, 1 = no texture
 * filtering, 0 = no reflections either (the UI lowers it on a slow camera) */
extern int fos_can_quality;
struct fos_can;
struct fos_can *fos_can_new(int film, int exposures);
void fos_can_free(struct fos_can *c);
void fos_can_set_exposures(struct fos_can *c, int exposures);

/* draw a canister standing on floor_y, centred on cx (1/256 px);
 * scale 256 = full size, rot = turn (0..65535, 0 = label to the front),
 * light 0..256 dims it for depth, leader 0 = none, 256 = the usual tongue,
 * more = pulled further out, reflect 0..256 = its mirror image in the floor,
 * fast = moving too quickly to need the label filtered between texels */
struct fos_can_pose { int cx8, floor_y, scale, rot, light, leader, reflect, fast; };
void fos_can_draw(struct fos_canvas *cv, struct fos_can *c, const struct fos_can_pose *p);
int  fos_can_height(int scale);
int  fos_can_width(int scale);

#endif
