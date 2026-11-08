/* fos_pages: home, the settings pages and the roll in the camera.
 *
 * Home is the film OS's main menu: the roll you are shooting on the left
 * (its canister slowly turning), the places you can go on the right. The
 * settings pages replace the Magic Lantern menu; every row is a toggle,
 * a choice or an action, and the highlight glides between rows.
 */

#include "fos_ui.h"
#include <string.h>
#include <math.h>

static float absf(float v) { return v < 0 ? -v : v; }

/* a roll in progress that changing film would finish (never with no rolls: there the film just changes) */
static int roll_active()
{
    return !ui_roll.no_rolls && ui_roll.loaded && ui_roll.shot < ui_roll.exposures;
}

/* the highlight behind the selected row of a list */
static void row_highlight(int x, int w, float y, int h)
{
    int yi = (int) (y + 0.5f);
    fos_rrect(cv, x, yi, w, h, 12, FC(RAMP_GRAY, 27), OPAQUE);
    fos_rrect(cv, x, yi + 10, 3, h - 20, 1, C_ACCENT, OPAQUE);
}

/* ---------------------------------------------------------------- the film card */

#define CARD_X   24
#define CARD_Y   52
#define CARD_W   268
#define CARD_H   378

#define CAN_FLOOR (CARD_Y + 210)
#define CAN_SCALE 186

/* the canister in the card, turning slowly (a turn every 24 s) */
static void card_canister(int t_ms)
{
    if (!ui_roll.loaded) return;
    struct fos_can *c = ui_can(ui_roll.film);
    struct fos_can_pose p = { (CARD_X + CARD_W / 2) * 256, CAN_FLOOR, CAN_SCALE, 0, 256, 256, 70, 0 };
    p.rot = (int) ((int64_t) t_ms * 65536 / 24000);
    fos_can_draw(cv, c, &p);
}

/* the box the turning canister lives in */
static void card_canister_box(int *x0, int *y0, int *x1, int *y1)
{
    int cx = CARD_X + CARD_W / 2;
    int w = fos_can_width(CAN_SCALE), h = fos_can_height(CAN_SCALE);
    *x0 = cx - w / 2 - 50; *x1 = cx + w / 2 + 56;
    *y0 = CAN_FLOOR - h - 4; *y1 = CAN_FLOOR + 34;
}

/* the roll in the camera, everything but the canister: a counter and a
 * strip of frames */
static void film_card()
{
    fos_rrect(cv, CARD_X, CARD_Y, CARD_W, CARD_H, 18, FC(RAMP_GRAY, 15), OPAQUE);
    int cx = CARD_X + CARD_W / 2;
    char b[64];
    if (!ui_roll.loaded)
    {
        /* an empty chamber */
        fos_rrect_line(cv, cx - 40, CARD_Y + 60, 80, 132, 14, 2, FC(RAMP_GRAY, 48), OPAQUE);
        fos_text_c(cv, &fos_font_lg, cx, CARD_Y + 240, "No film", C_TEXT, OPAQUE, 0);
        fos_text_c(cv, &fos_font_sm, cx, CARD_Y + 266, "Load a roll to start shooting", C_TEXT3, OPAQUE, 0);
        return;
    }

    fos_text_c(cv, &fos_font_xs, cx, CARD_Y + 26, "IN CAMERA", C_TEXT3, OPAQUE, 48);
    int y = CARD_Y + 250;
    if (ui_roll.no_rolls)
    {
        /* no rolls: the film, and a count that only goes up */
        const struct fos_film *f = &fos_films[ui_roll.film];
        snprintf(b, sizeof(b), "%s " T_DOT " ISO %d", f->process, f->box_iso);
        fos_text_c(cv, &fos_font_xs, cx, y, b, C_ACCENT, OPAQUE, 64);
        fos_text_c(cv, &fos_font_lg, cx, y + 30, f->name, C_TEXT, OPAQUE, 0);
        if (ui_roll.total > 0)
            snprintf(b, sizeof(b), "%d %s", ui_roll.total, ui_roll.total == 1 ? "frame" : "frames");
        else
            snprintf(b, sizeof(b), "No frames yet");
        fos_text_c(cv, &fos_font_md, cx, y + 70, b, ui_roll.total > 0 ? C_TEXT : C_TEXT3, OPAQUE, 0);
        return;
    }
    snprintf(b, sizeof(b), "ROLL %03d", ui_roll.number);
    fos_text_c(cv, &fos_font_xs, cx, y, b, C_ACCENT, OPAQUE, 64);
    fos_text_c(cv, &fos_font_lg, cx, y + 30, fos_films[ui_roll.film].name, C_TEXT, OPAQUE, 0);

    /* frames as a strip: shot ones lit */
    int n = ui_roll.exposures, shot = ui_roll.shot;
    int fw = n > 24 ? 5 : n > 12 ? 7 : 12, gap = 1;
    int sw = n * (fw + gap) - gap;
    int sx = cx - sw / 2, sy = y + 46;
    fos_rrect(cv, sx - 6, sy - 6, sw + 12, 24, 4, FC(RAMP_GRAY, 8), OPAQUE);
    for (int i = 0; i < n; i++)
        fos_fill(cv, sx + i * (fw + gap), sy, fw, 12, i < shot ? C_ACCENT2 : FC(RAMP_GRAY, 30));
    if (shot < n)
        fos_fill(cv, sx + shot * (fw + gap), sy, fw, 12, C_ACCENT);
    int left = n - shot;
    if (left > 0)
        snprintf(b, sizeof(b), "%d shot " T_DOT " %d left", shot, left);
    else
        snprintf(b, sizeof(b), ui_realistic() ? "End of roll " T_DOT " rewind to load" : "End of roll " T_DOT " ready to develop");
    fos_text_c(cv, &fos_font_sm, cx, sy + 36, b, left > 0 ? C_TEXT2 : C_RED, OPAQUE, 0);
}

/* ---------------------------------------------------------------- home */

enum { H_FILM, H_ROLL, H_DARK, H_FINDER, H_LENS, H_SYSTEM, H_COUNT };

static int home_sel;
static struct spring home_hl;
static int home_t, home_spun;
static int home_spin;               /* the canister's turn is due */
static int home_full = 1;           /* next frame redraws everything */
static int home_list = 1;           /* ... the list on the right */

#define LIST_X   312
#define LIST_W   384
#define LIST_Y   56
#define ROW_H    64

static void home_bake()
{
    ui_bg_plain();
    ui_to_bg(1);
    ui_status_bar("HOME");
    struct hint h[] = { { "^v", "Select" }, { "SET", "Open" }, { "PLAY", "Prints" }, { "MENU", "Back to camera" } };
    ui_hints(h, 4);
    film_card();
    ui_to_bg(0);
}

void home_enter(int arg)
{
    fos_port_roll(&ui_roll);
    if (arg >= 0 && arg < H_COUNT) home_sel = arg;
    home_hl.x = home_sel; home_hl.v = 0;
    home_t = home_spun = 0;
    home_full = 1;
    if (ui_roll.loaded) ui_can(ui_roll.film);
    home_bake();
}

static void home_open(int i)
{
    switch (i)
    {
        case H_FILM:   ui_go(SCR_SHELF, 1, GO_PUSH); break;
        case H_ROLL:   ui_go(SCR_ROLL, 0, GO_PUSH); break;
        case H_DARK:   ui_go(SCR_DARK, 0, GO_PUSH); break;
        case H_FINDER: ui_go(SCR_PAGE, 0, GO_PUSH); break;
        case H_LENS:   ui_go(SCR_PAGE, 1, GO_PUSH); break;
        case H_SYSTEM: ui_go(SCR_PAGE, 2, GO_PUSH); break;
    }
}

void home_key(int k)
{
    switch (k)
    {
        case FK_UP: case FK_WHEEL_L:
            home_sel = (home_sel + H_COUNT - 1) % H_COUNT;
            break;
        case FK_DOWN: case FK_WHEEL_R:
            home_sel = (home_sel + 1) % H_COUNT;
            break;
        case FK_SET: case FK_RIGHT:
            home_open(home_sel);
            break;
        case FK_MENU: case FK_HOME:
            ui_leave();
            break;
        case FK_PLAY:
            ui_go(SCR_PRINTS, 0, GO_FADE);
            break;
    }
    home_list = 1;
    ui_damage(LIST_Y, LIST_Y + H_COUNT * ROW_H);
}

int home_update(int dt)
{
    int moving = 0;
    home_t += dt;
    if (spring_step(&home_hl, home_sel, dt, 3.4f, 0.82f))
    {
        home_list = 1;
        ui_damage(LIST_Y, LIST_Y + H_COUNT * ROW_H);
        moving = 1;
    }
    /* the canister keeps turning, slowly enough that a dozen frames a
     * second of only its corner of the screen look smooth */
    if (ui_roll.loaded && home_t - home_spun >= 80)
    {
        int x0, y0, x1, y1;
        card_canister_box(&x0, &y0, &x1, &y1);
        home_spun = home_t;
        home_spin = 1;
        ui_damage(y0, y1);
        moving = 1;
    }
    return moving;
}

static void home_row(int i, int y, int selected)
{
    static const char *titles[H_COUNT] = { "Film", "Roll", "Darkroom", "Viewfinder", "Lens", "System" };
    static const int icons[H_COUNT] = { ICON_FILM, ICON_ROLL, ICON_PRINTS, ICON_FINDER, ICON_LENS, ICON_SYSTEM };
    char sub[80];
    char ap[8];
    const char *title = i == H_ROLL && ui_roll.no_rolls ? "Frames" : titles[i];     /* no rolls: there is no roll to open */
    switch (i)
    {
        case H_FILM:
            if (ui_roll.no_rolls)
                snprintf(sub, sizeof(sub), "Choose a film");
            else
                snprintf(sub, sizeof(sub), "%s", roll_active() ? (ui_realistic() ? "Change film (rewinds this roll)" : "Change film (finishes this roll)") : "Choose a stock and load a roll");
            break;
        case H_ROLL:
            if (ui_roll.no_rolls)
                snprintf(sub, sizeof(sub), "%d shot on %s", ui_roll.total, fos_films[ui_roll.film].name);
            else if (ui_roll.loaded)
                snprintf(sub, sizeof(sub), "Roll %03d " T_DOT " %d of %d", ui_roll.number, ui_roll.shot, ui_roll.exposures);
            else
                snprintf(sub, sizeof(sub), "No film in the camera");
            break;
        case H_DARK:
            snprintf(sub, sizeof(sub), ui_roll.no_rolls ? "Develop your frames " T_DOT " your prints" : "Develop your rolls " T_DOT " your prints");
            break;
        case H_FINDER:
            snprintf(sub, sizeof(sub), "HUD %s " T_DOT " wind %s", fos_port_get(FS_HUD) ? "on" : "off",
                     fos_port_get(FS_WIND) == 0 ? "off" : fos_port_get(FS_WIND) == 1 ? "reminder" : "locks shutter");
            break;
        case H_LENS:
            fos_format_fstop(ap, sizeof(ap), fos_port_get(FS_FSTOP));
            if (fos_port_lens_manual())
                snprintf(sub, sizeof(sub), "Manual lens " T_DOT " f/%s", ap);
            else
                snprintf(sub, sizeof(sub), "%s", fos_port_lens_name());
            break;
        case H_SYSTEM:
            if (ui_roll.no_rolls)
                snprintf(sub, sizeof(sub), "Roll limit " T_DOT " screen colour " T_DOT " Canon menu");
            else
                snprintf(sub, sizeof(sub), "%s mode " T_DOT " screen colour " T_DOT " Canon menu", ui_realistic() ? "Realistic" : "Basic");
            break;
    }
    int x = LIST_X + 20;
    fcol ic = selected ? C_ACCENT : C_TEXT3;
    ui_icon(icons[i], x + 14, y + ROW_H / 2, ic, OPAQUE);
    fos_text(cv, &fos_font_mdb, x + 46, y + 28, title, selected ? C_TEXT : C_TEXT2, OPAQUE, 0);
    fos_text(cv, &fos_font_sm, x + 46, y + 49, sub, C_TEXT3, OPAQUE, 0);
    fos_tri(cv, LIST_X + LIST_W - 22, y + ROW_H / 2, 8, 0, selected ? C_TEXT2 : FC(RAMP_GRAY, 60), OPAQUE);
}

void home_draw()
{
    int x0, y0, x1, y1;
    card_canister_box(&x0, &y0, &x1, &y1);
    if (ui_clobbered) home_full = 1;
    if (!home_full)
    {
        /* the card and the list sit side by side: each is redrawn alone */
        struct fos_canvas save = *cv;
        if (home_spin)
        {
            home_spin = 0;
            ui_bg_restore(x0, y0, x1 - x0, y1 - y0);
            fos_clip(cv, x0, y0, x1, y1);
            card_canister(home_t);
            *cv = save;
            ui_damage(y0, y1);
        }
        if (home_list)
        {
            int ly1 = LIST_Y + H_COUNT * ROW_H;
            ui_bg_restore(LIST_X, LIST_Y, LIST_W, ly1 - LIST_Y);
            fos_clip(cv, LIST_X, LIST_Y, LIST_X + LIST_W, ly1);
            row_highlight(LIST_X, LIST_W, LIST_Y + home_hl.x * ROW_H, ROW_H);
            for (int i = 0; i < H_COUNT; i++)
                home_row(i, LIST_Y + i * ROW_H, i == home_sel);
            *cv = save;
            ui_damage(LIST_Y, ly1);
            home_list = 0;
        }
        return;
    }
    home_full = 0;
    home_list = 0;
    home_spin = 0;
    ui_clobbered = 0;
    ui_damage_all();
    ui_bg_restore(0, 0, FOS_W, FOS_H);
    card_canister(home_t);
    row_highlight(LIST_X, LIST_W, LIST_Y + home_hl.x * ROW_H, ROW_H);
    for (int i = 0; i < H_COUNT; i++)
        home_row(i, LIST_Y + i * ROW_H, i == home_sel);
}

/* ---------------------------------------------------------------- settings pages */

enum { IT_TOGGLE, IT_CHOICE, IT_FSTOP, IT_ACTION, IT_INFO, IT_LEVEL, IT_PAGE };

#define PAGE_SYSTEM  2
#define PAGE_SCREEN  3
#define SYS_SCREEN_ROW 4                /* where Screen colour sits in System */
#define ACT_SCR_RESET 100               /* handled here, not by the port */

struct item
{
    const char *label;
    int kind;
    int setting;                /* FS_*, or FA_* for actions */
    const char *const *choices;
    int n;
    const char *help;
};

static const char *const wind_choices[] = { "Off", "Reminder", "Locks shutter" };
static const char *const apkey_choices[] = { "Dial in Av", "Up / Down", "Menu only" };
static const char *const start_choices[] = { "Off", "When no film", "Always" };
static const char *const mode_choices[] = { "Basic", "Realistic" };
static const char *const rolllim_choices[] = { "Off", "36 frames", "24 frames", "12 frames" };
static const char *const flip_choices[] = { "Auto", "Normal", "Upside down", "Mirrored" };

static const struct item finder_items[] =
{
    { "Viewfinder HUD", IT_TOGGLE, FS_HUD, 0, 0, "Meter and LEDs over LiveView; on a DSLR, a top plate on the back screen too." },
    { "LEDs on half-press", IT_TOGGLE, FS_LEDS, 0, 0, "Like a real SLR: the readout lights only while metering." },
    { "Focusing screen", IT_TOGGLE, FS_FOCUS, 0, 0, "A split-image circle and microprism collar (decoration only)." },
    { "Wind lever", IT_CHOICE, FS_WIND, wind_choices, 3, "After each frame press SET to wind on. Locking is experimental." },
    /* the last row: pages_fit leaves it out where the camera can't */
    { "Exposure simulation", IT_TOGGLE, FS_EXPSIM, 0, 0, "On: LiveView shows the exposure you'll get. Off: bright, to focus a manual lens." },
};

static const struct item lens_items[] =
{
    { "Lens", IT_INFO, -1, 0, 0, "Adapted manual lenses report no aperture: set it here." },
    { "Aperture", IT_FSTOP, FS_FSTOP, 0, 0, "The f-stop on the lens ring. Shown in the HUD, logged for the lab." },
    { "Widest aperture", IT_FSTOP, FS_LENS_WIDE, 0, 0, "Your manual lens' widest f-stop: the ring stops there." },
    { "Aperture control", IT_CHOICE, FS_APKEYS, apkey_choices, 3, "How to turn the aperture ring in LiveView." },
};

static const struct item system_items[] =
{
    { "Roll limit", IT_CHOICE, FS_ROLLLIM, rolllim_choices, 4, "Off: no rolls, shoot as many frames as you like. Or end a roll at 36, 24 or 12." },
    { "Film mode", IT_CHOICE, FS_MODE, mode_choices, 2, "A roll limit only. Basic: a full roll goes to the darkroom. Realistic: rewind it." },
    { "Start screen", IT_CHOICE, FS_STARTUP, start_choices, 3, "The film shelf when the camera turns on." },
    { "Boot animation", IT_TOGGLE, FS_BOOTANIM, 0, 0, "The aperture opening on the name at power on." },
    { "Screen colour", IT_PAGE, PAGE_SCREEN, 0, 0, "Match these colours to what you see around you." },
    { "Screen orientation", IT_CHOICE, FS_SCR_FLIP, flip_choices, 4, "Auto follows the hinge. Set one if the screen shows the wrong way round." },
    { "PLAY shows prints only", IT_TOGGLE, FS_PLAYLOCK, 0, 0, "Canon's playback would show frames the darkroom hasn't developed." },
    { "Re-apply film settings", IT_ACTION, FA_REAPPLY, 0, 0, "After changing picture style, white balance or ISO by hand." },
    { "Canon menu", IT_ACTION, FA_CANON_MENU, 0, 0, "Leave and open the camera's own menu (format card, RAW...)." },
    { "Debug log", IT_TOGGLE, FS_DEBUG, 0, 0, "Writes ML/FILMROLL.LOG for troubleshooting." },
    { "Support " FOS_NAME, IT_INFO, -3, 0, 0, "Scan the code with your phone to buy the developer a coffee." },
    { "About", IT_INFO, -2, 0, 0, FOS_NAME " " T_DOT " Barlow typeface by Jeremy Tribby (SIL OFL)" },
};

static const char *const std_choices[] = { "Canon", "Rec.709" };

static const struct item screen_items[] =
{
    { "Conversion", IT_CHOICE, FS_SCR_STD, std_choices, 2, "How colours are sent to the screen. Canon's way suits the EOS M." },
    { "Colour", IT_LEVEL, FS_SCR_SAT, 0, 0, "Less or more saturated." },
    { "Warmth", IT_LEVEL, FS_SCR_WARM, 0, 0, "Towards blue or towards amber, white included." },
    { "Brightness", IT_LEVEL, FS_SCR_BRIGHT, 0, 0, "Deeper or lighter mid tones; black and white stay." },
    { "Reset", IT_ACTION, ACT_SCR_RESET, 0, 0, "Back to the colours as designed." },
};

#define NITEMS(a) ((int) (sizeof(a) / sizeof((a)[0])))

static struct { const char *title; const char *sub; const struct item *items; int n; } pages[] =
{
    { "Viewfinder", "What you see in LiveView while you shoot", finder_items, NITEMS(finder_items) },
    { "Lens", "Aperture for manual lenses", lens_items, NITEMS(lens_items) },
    { "System", "Roll limit, screen colour, start up and the Canon menu", system_items, NITEMS(system_items) },
    { "Screen colour", "Live: the whole film OS changes as you go", screen_items, NITEMS(screen_items) },
};

static int page, page_sel, page_top;
#define P_KNOBS   12            /* one per row: more rows than any page has */
static struct spring page_hl, page_scroll, knob[P_KNOBS];

/* The pages without what this camera can't do (the port answers -1 for it:
 * screen orientation on a fixed screen, exposure simulation where Magic
 * Lantern can't set it), worked out once. */
static struct item system_here[NITEMS(system_items)];

static void pages_fit()
{
    static int done = 0;
    if (done) return;
    done = 1;
    int n = 0;
    for (int i = 0; i < NITEMS(system_items); i++)
    {
        const struct item *it = &system_items[i];
        if (it->kind == IT_CHOICE && fos_port_get(it->setting) < 0) continue;
        system_here[n++] = *it;
    }
    pages[PAGE_SYSTEM].items = system_here;
    pages[PAGE_SYSTEM].n = n;
    /* the Viewfinder page's last row: no copy needed (memory at boot) */
    if (fos_port_get(FS_EXPSIM) < 0) pages[0].n--;
}
static int help_fade;           /* ms since the selection changed */

#define P_LIST_Y  118
#define P_ROW_H   46
#define P_ROWS    6
#define P_X       24
#define P_W       404

/* the preview panel beside the list */
#define PV_X      444
#define PV_Y      P_LIST_Y
#define PV_W      252
#define PV_H      (P_ROWS * P_ROW_H)

static struct spring lens_open;     /* the aperture in the lens preview, 0..1 */

/* what the next frame redraws */
enum { D_LIST = 1, D_PREV = 2, D_HELP = 4, D_ALL = 8 };
static int pg_dirty = D_ALL;
#define LIST_RECT_Y0 (P_LIST_Y - 4)
#define LIST_RECT_Y1 (P_LIST_Y + P_ROWS * P_ROW_H + 16)
#define HELP_RECT_Y0 (HINTS_Y - 32)

/* how open the blades look for fos_fstops[index]: a stop closes the
 * opening by a constant factor, slower than real so f/16 still shows */
static float fstop_open(int index)
{
    float stops = (index - 1) / 3.0f;
    return 0.94f * expf(-0.30f * stops);
}

static void page_bake()
{
    ui_bg_plain();
    ui_to_bg(1);
    ui_status_bar("SETTINGS");
    fos_text(cv, &fos_font_lg, P_X + 4, 80, pages[page].title, C_TEXT, OPAQUE, 0);
    fos_text(cv, &fos_font_sm, P_X + 4, 104, pages[page].sub, C_TEXT3, OPAQUE, 0);
    struct hint h[] = { { "^v", "Select" }, { "<>", "Change" }, { "SET", "Toggle" }, { "MENU", "Back" } };
    struct hint hs[] = { { "^v", "Select" }, { "<>", "Adjust" }, { "MENU", "Back" } };
    if (page == PAGE_SCREEN) ui_hints(hs, 3);
    else ui_hints(h, 4);
    fos_rrect(cv, PV_X, PV_Y, PV_W, PV_H, 16, FC(RAMP_GRAY, 15), OPAQUE);
    static const char *captions[] = { "LIVEVIEW PREVIEW", "APERTURE", 0, "TEST CHART" };
    if (captions[page])             /* System's panel captions itself */
        fos_text_c(cv, &fos_font_xs, PV_X + PV_W / 2, PV_Y + PV_H - 14, captions[page], C_TEXT3, OPAQUE, 48);
    ui_to_bg(0);
}

static int item_value(const struct item *it)
{
    return it->setting >= 0 && it->kind != IT_ACTION && it->kind != IT_PAGE ? fos_port_get(it->setting) : 0;
}

/* where an item's knob springs to: a switch 0 / 1, a slider its value */
static float knob_target(const struct item *it)
{
    if (it->kind == IT_LEVEL) return item_value(it);
    return it->kind == IT_TOGGLE && item_value(it) ? 1 : 0;
}

void page_enter(int arg)
{
    pages_fit();
    /* from home: a page; between pages: page | row << 4 */
    if (ui_from() == SCR_HOME || ui_from() == SCR_PAGE)
    {
        page = fos_clamp(arg & 15, 0, PAGE_SCREEN);
        page_sel = fos_clamp(arg >> 4, 0, pages[page].n - 1);
    }
    page_top = page_sel >= P_ROWS ? page_sel - P_ROWS + 1 : 0;
    page_hl.x = page_sel; page_hl.v = 0;
    page_scroll.x = page_top; page_scroll.v = 0;
    for (int i = 0; i < pages[page].n && i < P_KNOBS; i++)
    {
        knob[i].x = knob_target(&pages[page].items[i]);
        knob[i].v = 0;
    }
    help_fade = 1000;
    lens_open.x = fstop_open(fos_port_get(FS_FSTOP)); lens_open.v = 0;
    pg_dirty = D_ALL;
    page_bake();
}

static void change(const struct item *it, int dir)
{
    int v = fos_port_get(it->setting);
    switch (it->kind)
    {
        case IT_TOGGLE:
            fos_port_set(it->setting, !v);
            break;
        case IT_CHOICE:
            fos_port_set(it->setting, fos_clamp(v + dir, 0, it->n - 1));
            break;
        case IT_LEVEL:
            fos_port_set(it->setting, fos_clamp(v + dir, -FOS_SCR_STEPS, FOS_SCR_STEPS));
            break;
        case IT_FSTOP:
        {
            int lo = it->setting == FS_FSTOP ? fos_port_get(FS_LENS_WIDE) : 0;
            fos_port_set(it->setting, fos_clamp(v + dir, lo, fos_nfstops - 1));
            if (it->setting == FS_LENS_WIDE && fos_port_get(FS_FSTOP) < fos_port_get(FS_LENS_WIDE))
                fos_port_set(FS_FSTOP, fos_port_get(FS_LENS_WIDE));
            break;
        }
    }
}

static void page_damage(int what)
{
    pg_dirty |= what;
    if (what & D_ALL) ui_damage_all();
    if (what & (D_LIST | D_PREV)) ui_damage(LIST_RECT_Y0, LIST_RECT_Y1);
    if (what & D_HELP) ui_damage(HELP_RECT_Y0, HINTS_Y);
}

void page_key(int k)
{
    const struct item *items = pages[page].items;
    int n = pages[page].n;
    const struct item *it = &items[page_sel];
    /* moving the selection redraws the list and the help (and the lens,
     * which shows the selected f-stop); changing a value, list and preview */
    int dirty = k == FK_UP || k == FK_DOWN || k == FK_WHEEL_L || k == FK_WHEEL_R
              ? D_LIST | D_HELP | (page == 1 || page == PAGE_SYSTEM ? D_PREV : 0)
              : k == FK_LEFT || k == FK_RIGHT || k == FK_SET ? D_LIST | D_PREV : D_ALL;
    switch (k)
    {
        case FK_UP: case FK_WHEEL_L:
            if (page_sel > 0) { page_sel--; help_fade = 0; }
            break;
        case FK_DOWN: case FK_WHEEL_R:
            if (page_sel < n - 1) { page_sel++; help_fade = 0; }
            break;
        case FK_LEFT: case FK_RIGHT:
            if (it->kind == IT_TOGGLE) change(it, 0);
            else if (it->kind == IT_CHOICE || it->kind == IT_FSTOP || it->kind == IT_LEVEL)
                change(it, k == FK_RIGHT ? 1 : -1);
            else if (it->kind == IT_PAGE && k == FK_RIGHT)
                ui_go(SCR_PAGE, it->setting, GO_PUSH);
            break;
        case FK_SET:
            if (it->kind == IT_TOGGLE) change(it, 0);
            else if (it->kind == IT_CHOICE)
                fos_port_set(it->setting, (fos_port_get(it->setting) + 1) % it->n);
            else if (it->kind == IT_FSTOP) change(it, 1);
            else if (it->kind == IT_PAGE) ui_go(SCR_PAGE, it->setting, GO_PUSH);
            else if (it->kind == IT_ACTION && it->setting == ACT_SCR_RESET)
            {
                fos_port_set(FS_SCR_STD, 0);
                fos_port_set(FS_SCR_SAT, 0);
                fos_port_set(FS_SCR_WARM, 0);
                fos_port_set(FS_SCR_BRIGHT, 0);
                ui_toast("Screen colour as designed");
            }
            else if (it->kind == IT_ACTION)
            {
                fos_port_action(it->setting, 0, 0);
                if (it->setting == FA_CANON_MENU) ui_leave();
                if (it->setting == FA_REAPPLY)
                    ui_toast(ui_roll.loaded ? "Film settings go in when you return to the camera" : "No film loaded");
            }
            break;
        case FK_MENU:
            if (page == PAGE_SCREEN) ui_go(SCR_PAGE, PAGE_SYSTEM | SYS_SCREEN_ROW << 4, GO_POP);
            else ui_go(SCR_HOME, page + H_FINDER, GO_POP);
            break;
        case FK_HOME: case FK_PLAY:
            ui_leave();
            break;
    }
    if (page_sel < page_top) page_top = page_sel;
    if (page_sel >= page_top + P_ROWS) page_top = page_sel - P_ROWS + 1;
    page_damage(dirty);
}

int page_update(int dt)
{
    int dirty = 0;
    if (spring_step(&page_hl, page_sel, dt, 3.6f, 0.82f)) dirty |= D_LIST;
    if (spring_step(&page_scroll, page_top, dt, 3.0f, 0.95f)) dirty |= D_LIST;
    for (int i = 0; i < pages[page].n && i < P_KNOBS; i++)
        if (spring_step(&knob[i], knob_target(&pages[page].items[i]), dt, 4.0f, 0.75f))
            dirty |= D_LIST | (page == 0 ? D_PREV : 0);     /* the viewfinder preview follows the switches */
    if (help_fade < 400) { help_fade += dt; dirty |= D_HELP; }
    if (page == 1)
    {
        const struct item *it = &pages[page].items[page_sel];
        int idx = it->setting == FS_LENS_WIDE ? fos_port_get(FS_LENS_WIDE) : fos_port_get(FS_FSTOP);
        if (spring_step(&lens_open, fstop_open(idx), dt, 2.6f, 0.72f)) dirty |= D_PREV;
    }
    page_damage(dirty);
    return dirty != 0;
}

/* ---- the previews */

/* a dusk scene through the viewfinder, with the HUD as set */
static void preview_finder()
{
    int sx = PV_X + 12, sy = PV_Y + 14, sw = PV_W - 24, sh = 152;
    int horizon = sy + sh * 64 / 100;
    fos_vgrad(cv, sx, sy, sw, horizon - sy, RAMP_AMBER, LV12(64), LV12(178));
    fos_vgrad(cv, sx, horizon, sw, sy + sh - horizon, RAMP_GRAY, LV12(16), LV12(8));
    fos_disc(cv, (sx + 74) * 256, (horizon - 26) * 256, 24 * 256, FC(RAMP_AMBER, 214), 110);
    fos_disc(cv, (sx + 74) * 256, (horizon - 26) * 256, 15 * 256, FC(RAMP_GRAY, 246), OPAQUE);
    /* hills */
    int hills[][6] = {
        { sx - 20, horizon + 8, sx + 70, horizon - 16, sx + 150, horizon + 8 },
        { sx + 90, horizon + 8, sx + 170, horizon - 28, sx + sw + 30, horizon + 8 },
    };
    for (int i = 0; i < 2; i++)
    {
        int q[6];
        for (int j = 0; j < 6; j++) q[j] = hills[i][j] * 256;
        fos_convex(cv, q, 3, FC(RAMP_GRAY, 12), OPAQUE);
    }
    fos_fill(cv, sx, horizon + 8, sw, sy + sh - horizon - 8, FC(RAMP_GRAY, 12));
    /* exposure simulation (the last row, where there is one) off: Canon
     * brightens LiveView, whatever the exposure */
    int es = pages[0].n - 1;
    if (pages[0].items[es].setting == FS_EXPSIM && knob[es].x < 0.99f)
        fos_blend(cv, sx, sy, sw, sh, FC(RAMP_GRAY, 255), (int) ((1 - knob[es].x) * 72));

    float hud = knob[0].x, leds_half = knob[1].x, focus = knob[2].x;
    int wind = fos_port_get(FS_WIND);
    if (focus > 0.01f)
    {
        int cx = (sx + (sw - 20) / 2) * 256, cy = (sy + (sh - 14) / 2) * 256;
        fos_ring(cv, cx, cy, 21 * 256, 256, FC(RAMP_GRAY, 210), (int) (focus * 150));
        fos_ring(cv, cx, cy, 9 * 256, 300, FC(RAMP_GRAY, 250), (int) (focus * 230));
        fos_line(cv, cx - 9 * 256, cy, cx + 9 * 256, cy, 300, FC(RAMP_GRAY, 250), (int) (focus * 230));
    }
    if (hud > 0.01f)
    {
        int a = (int) (hud * 256);
        /* meter strip and LED strip, as on the camera but small */
        fos_blend(cv, sx + sw - 20, sy, 20, sh - 14, FC(RAMP_GRAY, 0), a);
        fos_blend(cv, sx, sy + sh - 14, sw, 14, FC(RAMP_GRAY, 0), a);
        for (int i = 0; i < 10; i++)
            fos_fill(cv, sx + sw - 12, sy + 8 + i * 12, 5, 1, FC(RAMP_GRAY, 120 * a / 256));
        fos_tri(cv, sx + sw - 16, sy + 8 + 5 * 12, 6, 0, FC(RAMP_RED, 150), a);
        int lit = (int) (150 - leds_half * 100);
        fos_text(cv, &fos_font_xs, sx + 50, sy + sh - 3, "125   f4   12", FC(RAMP_RED, lit), a, 16);
        if (wind)
            fos_text(cv, &fos_font_xs, sx + 6, sy + sh - 3, "WIND", FC(RAMP_RED, 150), a, 16);
    }
    /* what that means */
    const char *l1 = !fos_port_get(FS_HUD) ? "Canon's own screen only" :
                     fos_port_get(FS_LEDS) ? "LEDs light on half-press" : "LEDs always lit";
    const char *l2 = wind == 0 ? "No wind lever" : wind == 1 ? "SET winds on after each frame" : "Shutter locked until you wind";
    fos_text_c(cv, &fos_font_sm, PV_X + PV_W / 2, sy + sh + 32, l1, C_TEXT2, OPAQUE, 0);
    fos_text_c(cv, &fos_font_sm, PV_X + PV_W / 2, sy + sh + 54, l2, C_TEXT3, OPAQUE, 0);
}

static void preview_lens()
{
    int cx = PV_X + PV_W / 2, cy = PV_Y + 90;
    int open = (int) (clampf(lens_open.x, 0.02f, 1) * 4096);
    ui_aperture(cx, cy, 68, open, 3000 + (4096 - open) * 3);
    const struct item *it = &pages[page].items[page_sel];
    int idx = it->setting == FS_LENS_WIDE ? fos_port_get(FS_LENS_WIDE) : fos_port_get(FS_FSTOP);
    char ap[8], b[24];
    fos_format_fstop(ap, sizeof(ap), idx);
    snprintf(b, sizeof(b), "f/%s", ap);
    fos_text_c(cv, &fos_font_xl, cx, cy + 126, b, C_TEXT, OPAQUE, 0);
    if (it->setting == FS_LENS_WIDE)
        snprintf(b, sizeof(b), "WIDEST THE LENS OPENS");
    else
        snprintf(b, sizeof(b), "SET ON THE RING");
    fos_text_c(cv, &fos_font_xs, cx, cy + 148, b, C_ACCENT2, OPAQUE, 32);
}

/* swatches of the labels' inks and a grey wedge: compare with the boxes */
static void preview_chart()
{
    static const struct { int ramp, level; const char *name; } inks[8] =
    {
        { RAMP_AMBER, 190, "GOLD" }, { RAMP_RED, 150, "RED" }, { RAMP_ORANGE, 160, "PORTRA" }, { RAMP_CRIMSON, 128, "EKTAR" },
        { RAMP_GREEN, 140, "FUJI" }, { RAMP_VIOLET, 118, "VELVIA" }, { RAMP_BLUE, 160, "ILFORD" }, { RAMP_LEADER, 120, "FILM" },
    };
    int x0 = PV_X + 14, y0 = PV_Y + 16, sw = 50, gap = 6;
    for (int i = 0; i < 8; i++)
    {
        int x = x0 + (i % 4) * (sw + gap), y = y0 + (i / 4) * 74;
        fos_rrect(cv, x, y, sw, 44, 8, FC(inks[i].ramp, inks[i].level), OPAQUE);
        fos_text_c(cv, &fos_font_xs, x + sw / 2, y + 60, inks[i].name, C_TEXT3, OPAQUE, 16);
    }
    /* eleven steps from black to white: the darkest two should still differ */
    int wy = y0 + 160, ww = PV_W - 28;
    for (int i = 0; i < 11; i++)
    {
        int xa = x0 + i * ww / 11, xb = x0 + (i + 1) * ww / 11;
        fos_fill(cv, xa, wy, xb - xa, 36, FC(RAMP_GRAY, i * 255 / 10));
    }
    fos_rrect_line(cv, x0 - 1, wy - 1, ww + 2, 38, 2, 1, FC(RAMP_GRAY, 60), OPAQUE);
    fos_text(cv, &fos_font_xs, x0, wy + 54, "0", C_TEXT3, OPAQUE, 0);
    fos_text_r(cv, &fos_font_xs, x0 + ww, wy + 54, "100%", C_TEXT3, OPAQUE, 0);
}

/* the donation links as QR codes (ML/DATA/FOSQR.BIN, see tools/genqr.py),
 * read the first time they are shown */
struct qr { char label[48], url[80]; int n; const uint8_t *bits; };
static struct qr qrs[2];
static int nqr = -1;

static void qr_load()
{
    char path[64];
    snprintf(path, sizeof(path), "%sML/DATA/FOSQR.BIN", fos_port_card());
    nqr = 0;
    void *f = fos_port_open(path, 0);
    if (!f) return;
    static uint8_t blob[1024];
    int size = fos_port_read(f, blob, sizeof(blob));
    fos_port_close(f);
    if (size < 8 || memcmp(blob, "FOSQ", 4)) return;
    int n = blob[4], p = 8;
    for (int i = 0; i < n && i < 2; i++)
    {
        if (p + 132 > size) break;
        struct qr *q = &qrs[nqr];
        memcpy(q->label, blob + p, 48); q->label[47] = 0;
        memcpy(q->url, blob + p + 48, 80); q->url[79] = 0;
        q->n = blob[p + 128] | blob[p + 129] << 8;
        q->bits = blob + p + 132;
        p += 132 + (q->n * q->n + 7) / 8;
        if (q->n < 21 || q->n > 61 || p > size) break;
        nqr++;
    }
}

/* a code centred on (cx, cy), its label at base */
static void draw_qr(const struct qr *q, int cx, int cy, int mod, int base)
{
    int quiet = 2, side = (q->n + 2 * quiet) * mod, x0 = cx - side / 2, y = cy - side / 2;
    fos_fill(cv, x0, y, side, side, FC(RAMP_GRAY, 245));
    for (int r = 0; r < q->n; r++)
        for (int c = 0; c < q->n; c++)
        {
            int i = r * q->n + c;
            if (q->bits[i >> 3] >> (7 - (i & 7)) & 1)
                fos_fill(cv, x0 + (quiet + c) * mod, y + (quiet + r) * mod, mod, mod, FC(RAMP_GRAY, 8));
        }
    const struct fos_font *f = fos_text_w(&fos_font_mdb, q->label, 0) <= PV_W - 24 ? &fos_font_mdb : &fos_font_sm;
    fos_text_c(cv, f, cx, base, q->label, C_TEXT, OPAQUE, 0);
}

static void preview_support()
{
    int cx = PV_X + PV_W / 2;
    if (nqr < 0) qr_load();
    if (nqr == 0)
    {
        ui_aperture(cx, PV_Y + 80, 40, 1700, 5000);
        fos_text_c(cv, &fos_font_lg, cx, PV_Y + 162, "Thank you", C_TEXT, OPAQUE, 0);
        fos_text_c(cv, &fos_font_sm, cx, PV_Y + 186, "for shooting film with " FOS_NAME, C_TEXT2, OPAQUE, 0);
    }
    else if (nqr == 1)
        draw_qr(&qrs[0], cx, PV_Y + 116, 200 / (qrs[0].n + 4), PV_Y + 238);
    else
        for (int i = 0; i < 2; i++)
            draw_qr(&qrs[i], cx + (i ? 62 : -62), PV_Y + 116, 112 / (qrs[i].n + 4), PV_Y + 190);
    fos_text_c(cv, &fos_font_xs, cx, PV_Y + PV_H - 14, "SUPPORT", C_TEXT3, OPAQUE, 48);
}

static void preview_about()
{
    int cx = PV_X + PV_W / 2;
    fos_text_c(cv, &fos_font_xs, cx, PV_Y + PV_H - 14, "ABOUT", C_TEXT3, OPAQUE, 48);
    ui_aperture(cx, PV_Y + 64, 40, 1700, 5000);
    fos_text_c(cv, &fos_font_lg, cx, PV_Y + 142, FOS_NAME, C_TEXT, OPAQUE, 64);
    char m[48];
    snprintf(m, sizeof(m), "Film OS for the %s", fos_port_model());
    fos_text_c(cv, &fos_font_sm, cx, PV_Y + 166, m, C_TEXT2, OPAQUE, 0);
    char b[48];
    snprintf(b, sizeof(b), "VERSION %s " T_DOT " %s", fos_port_version(), __DATE__);
    fos_text_c(cv, &fos_font_xs, cx, PV_Y + 196, b, C_TEXT3, OPAQUE, 24);
    fos_text_c(cv, &fos_font_xs, cx, PV_Y + 214, "MAGIC LANTERN INSIDE", C_TEXT3, OPAQUE, 24);
    fos_text_c(cv, &fos_font_xs, cx, PV_Y + 232, "BARLOW TYPEFACE " T_DOT " SIL OFL", C_TEXT3, OPAQUE, 24);
}

/* an on / off switch; k = 0..1 where the knob is */
static void toggle_switch(int rx, int cy, float k)
{
    int w = 44, h = 24;
    int x = rx - w, y = cy - h / 2;
    int on_lv = (int) (k * 186);
    fos_rrect(cv, x, y, w, h, h / 2, k > 0.5f ? FC(RAMP_AMBER, 40 + on_lv * 146 / 186) : FC(RAMP_GRAY, 40 + (int) ((1 - k) * 12)), OPAQUE);
    int kx = x + 12 + (int) (k * (w - 24));
    fos_disc(cv, kx * 256, cy * 256, 9 * 256, FC(RAMP_GRAY, 244), OPAQUE);
}

/* a -6..6 slider ending at rx: notches, a knob at k, the value beside it */
static void level_slider(int rx, int cy, float k, int value, int selected)
{
    int w = 150, x0 = rx - w;
    fos_fill(cv, x0, cy, w, 2, FC(RAMP_GRAY, 40));
    for (int i = -FOS_SCR_STEPS; i <= FOS_SCR_STEPS; i++)
    {
        int x = x0 + (i + FOS_SCR_STEPS) * w / (2 * FOS_SCR_STEPS);
        int big = i == 0;
        fos_fill(cv, x, cy - (big ? 6 : 3), 1, big ? 13 : 7, FC(RAMP_GRAY, big ? 120 : 60));
    }
    /* from the middle to the knob, lit */
    int kx = x0 + (int) ((k + FOS_SCR_STEPS) * w / (2 * FOS_SCR_STEPS));
    int mid = x0 + w / 2;
    if (kx != mid)
        fos_fill(cv, kx < mid ? kx : mid, cy, kx < mid ? mid - kx : kx - mid, 2, selected ? C_ACCENT : C_ACCENT2);
    fos_disc(cv, kx * 256 + 128, cy * 256 + 128, 8 * 256, selected ? FC(RAMP_GRAY, 244) : FC(RAMP_GRAY, 170), OPAQUE);
    char b[16];
    if (value > 0) snprintf(b, sizeof(b), "+%d", value);
    else if (value < 0) snprintf(b, sizeof(b), T_NDASH "%d", -value);
    else snprintf(b, sizeof(b), "0");
    fos_text_r(cv, &fos_font_mdb, x0 - 16, cy + 7, b, selected ? C_TEXT : C_TEXT2, OPAQUE, 0);
}

static void page_row(const struct item *it, int i, int y, int selected)
{
    int base = y + 30;
    fos_text(cv, &fos_font_md, P_X + 22, base, it->label, selected ? C_TEXT : C_TEXT2, OPAQUE, 0);
    int rx = P_X + P_W - 20;
    char b[48];
    switch (it->kind)
    {
        case IT_TOGGLE:
            toggle_switch(rx, y + P_ROW_H / 2, knob[i].x);
            break;
        case IT_CHOICE:
        case IT_FSTOP:
        {
            int v = fos_port_get(it->setting);
            if (it->kind == IT_CHOICE)
                snprintf(b, sizeof(b), "%s", it->choices[fos_clamp(v, 0, it->n - 1)]);
            else
            {
                char ap[8];
                fos_format_fstop(ap, sizeof(ap), v);
                snprintf(b, sizeof(b), "f/%s", ap);
            }
            int w = fos_text_w(&fos_font_mdb, b, 0);
            fcol vc = selected ? C_ACCENT : C_TEXT2;
            if (selected)
            {
                fos_tri(cv, rx - w - 18, y + P_ROW_H / 2 - 1, 7, 2, C_TEXT3, OPAQUE);
                fos_tri(cv, rx - 4, y + P_ROW_H / 2 - 1, 7, 0, C_TEXT3, OPAQUE);
                rx -= 14;
            }
            fos_text_r(cv, &fos_font_mdb, rx, base, b, vc, OPAQUE, 0);
            break;
        }
        case IT_ACTION:
        case IT_PAGE:
            fos_tri(cv, rx - 4, y + P_ROW_H / 2 - 1, 8, 0, selected ? C_ACCENT : C_TEXT3, OPAQUE);
            break;
        case IT_LEVEL:
            level_slider(rx, y + P_ROW_H / 2, knob[i].x, fos_port_get(it->setting), selected);
            break;
        case IT_INFO:
            if (it->setting == -1)
                snprintf(b, sizeof(b), "%s", fos_port_lens_manual() ? "Manual" : fos_port_lens_name());
            else if (it->setting == -3)
            {
                if (nqr < 0) qr_load();
                snprintf(b, sizeof(b), "%s", nqr > 0 ? "Scan the code" : "");
            }
            else
                snprintf(b, sizeof(b), "%s", fos_port_version());
            fos_text_r(cv, &fos_font_md, rx, base, b, C_TEXT3, OPAQUE, 0);
            break;
    }
}

void page_draw()
{
    const struct item *items = pages[page].items;
    int n = pages[page].n;
    int what = ui_clobbered ? D_ALL : pg_dirty;
    pg_dirty = 0;
    struct fos_canvas save = *cv;
    if (what & D_ALL)
    {
        ui_clobbered = 0;
        ui_damage_all();
        ui_bg_restore(0, 0, FOS_W, FOS_H);
        what = D_LIST | D_PREV | D_HELP;
    }
    else
    {
        /* only the parts that change, each from the background */
        if (what & D_LIST) ui_bg_restore(0, LIST_RECT_Y0, PV_X - 4, LIST_RECT_Y1 - LIST_RECT_Y0);
        if (what & D_PREV) ui_bg_restore(PV_X, PV_Y, PV_W, PV_H);
        if (what & D_HELP) ui_bg_restore(0, HELP_RECT_Y0, FOS_W, HINTS_Y - HELP_RECT_Y0);
    }
    if (what & D_PREV)
    {
        fos_clip(cv, PV_X, PV_Y, PV_X + PV_W, PV_Y + PV_H);
        if (page == 0) preview_finder();
        else if (page == 1) preview_lens();
        else if (page == PAGE_SCREEN) preview_chart();
        else if (items[page_sel].setting == -3 && items[page_sel].kind == IT_INFO) preview_support();
        else preview_about();
        *cv = save;
    }
    if (what & D_HELP)
    {
        /* help for the selected row, fading in */
        float a = clampf(help_fade / 260.0f, 0, 1);
        if (!ui_toast_up())
            fos_text(cv, &fos_font_sm, P_X + 22, HINTS_Y - 14, items[page_sel].help, C_TEXT3, (int) (ease_out(a) * 256), 0);
    }
    if (!(what & D_LIST)) return;
    fos_clip(cv, 0, P_LIST_Y - 2, PV_X - 4, P_LIST_Y + P_ROWS * P_ROW_H + 2);
    float top = page_scroll.x;
    row_highlight(P_X, P_W, P_LIST_Y + (page_hl.x - top) * P_ROW_H, P_ROW_H);
    for (int i = 0; i < n; i++)
    {
        float y = P_LIST_Y + (i - top) * P_ROW_H;
        if (y < P_LIST_Y - P_ROW_H || y > P_LIST_Y + P_ROWS * P_ROW_H) continue;
        page_row(&items[i], i, (int) (y + 0.5f), i == page_sel);
        if (i < n - 1 && i != page_sel && i + 1 != page_sel)
            fos_fill(cv, P_X + 22, (int) (y + 0.5f) + P_ROW_H, P_W - 44, 1, FC(RAMP_GRAY, 22));
    }
    *cv = save;
    /* more below / above */
    if (page_top + P_ROWS < n)
        fos_tri(cv, P_X + P_W / 2, P_LIST_Y + P_ROWS * P_ROW_H + 8, 8, 1, C_TEXT3, OPAQUE);
}

/* ---------------------------------------------------------------- the roll */

static struct spring dial;      /* frames shown on the counter */
static int asking_rewind;
static struct spring sheet;
static int rewinding, rewind_t;
static int rewind_then;                 /* after rewinding: 1 develop it, 0 load a new roll */
static int rewind_number;
static int roll_sheet_drawn;            /* the last frame had the sheet on it */

#define DIAL_CX  176
#define DIAL_CY  244
#define DIAL_R   118

static void roll_bake()
{
    ui_bg_plain();
    ui_to_bg(1);
    /* no rolls: this is the frame counter of the film in the camera, and SET goes to the darkroom */
    ui_status_bar(ui_roll.no_rolls ? "FRAMES" : "ROLL");
    struct hint h1[] = { { "SET", ui_realistic() ? "Rewind roll" : "Finish roll" }, { "MENU", "Back" } };
    struct hint h2[] = { { "SET", "Load film" }, { "MENU", "Back" } };
    struct hint h3[] = { { "SET", "Darkroom" }, { "MENU", "Back" } };
    if (ui_roll.no_rolls) ui_hints(h3, 2);
    else if (ui_roll.loaded) ui_hints(h1, 2);
    else ui_hints(h2, 2);
    ui_to_bg(0);
}

void roll_enter(int arg)
{
    fos_port_roll(&ui_roll);
    dial.x = ui_roll.no_rolls ? ui_roll.total - 2.0f : -1.5f; dial.v = 0;     /* from "S", like a fresh counter (no rolls: just short of the count, so it does not spin through hundreds) */
    roll_sheet_drawn = 0;
    asking_rewind = arg == 1 && ui_roll.loaded && !ui_roll.no_rolls;     /* the roll has just been finished */
    sheet.x = 0; sheet.v = 0;
    rewinding = 0;
    roll_bake();
}

void roll_key(int k)
{
    if (rewinding) return;
    if (asking_rewind)
    {
        if (k == FK_SET || k == FK_INFO)
        {
            asking_rewind = 0;
            rewinding = 1;
            rewind_t = ui_realistic() ? 0 : 1500;       /* Basic: no rewinding to watch */
            rewind_then = k == FK_SET;
            rewind_number = ui_roll.number;
            fos_port_action(FA_REWIND, 0, 0);
        }
        else if (k == FK_MENU) asking_rewind = 0;
        ui_damage_all();
        return;
    }
    switch (k)
    {
        case FK_SET:
            if (ui_roll.no_rolls) ui_go(SCR_DARK, 0, GO_PUSH);
            else if (ui_roll.loaded) asking_rewind = 1;
            else ui_go(SCR_SHELF, 1, GO_PUSH);
            break;
        case FK_MENU:
            ui_go(SCR_HOME, H_ROLL, GO_POP);
            break;
        case FK_HOME: case FK_PLAY:
            ui_leave();
            break;
    }
    ui_damage_all();
}

int roll_update(int dt)
{
    int moving = 0;
    float target = ui_roll.no_rolls ? (float) ui_roll.total : ui_roll.loaded ? ui_roll.shot : -1.5f;
    if (rewinding)
    {
        rewind_t += dt;
        target = -1.5f;
        moving = 1;
        if (rewind_t > 1500)
        {
            fos_port_roll(&ui_roll);
            if (rewind_then)
            {
                dark_queue_clear();
                dark_queue_add(rewind_number);
                ui_go(SCR_DEVELOP, 0, GO_FADE);
            }
            else ui_go(SCR_SHELF, 1, GO_PUSH);
            return 1;
        }
    }
    moving |= spring_step(&dial, target, dt, rewinding ? 1.2f : 0.9f, rewinding ? 1.0f : 0.78f);
    moving |= spring_step(&sheet, asking_rewind ? 1 : 0, dt, 3.0f, 0.95f);
    if (moving) ui_damage_all();
    return moving;
}

/* the box the dial turns in */
#define DIAL_BOX_X0 (DIAL_CX - DIAL_R - 18)
#define DIAL_BOX_X1 (DIAL_CX + DIAL_R + 18)
#define DIAL_BOX_Y0 (DIAL_CY - DIAL_R - 22)
#define DIAL_BOX_Y1 (DIAL_CY + DIAL_R + 18)

/* the frame counter: a dial of numbers turning under a fixed mark, like
 * the little window on top of a film camera */
static void counter_dial(float frames)
{
    int cx = DIAL_CX * 256, cy = DIAL_CY * 256;
    fos_disc(cv, cx, cy, (DIAL_R + 14) * 256, FC(RAMP_GRAY, 15), OPAQUE);
    fos_disc(cv, cx, cy, DIAL_R * 256, FC(RAMP_METAL, 26), OPAQUE);
    fos_ring(cv, cx, cy, DIAL_R * 256, 512, FC(RAMP_METAL, 60), OPAQUE);
    fos_ring(cv, cx, cy, (DIAL_R - 30) * 256, 256, FC(RAMP_METAL, 40), OPAQUE);
    /* 7.5 degrees a frame; frame "frames" sits at the top */
    const int per = 65536 * 75 / 3600;
    /* a roll's dial has its 36 or so numbers; with no rolls the count goes on, so only the ones near the mark are drawn */
    int f0 = -2, f1 = 40;
    if (ui_roll.no_rolls)
    {
        f0 = (int) frames - 24 < -2 ? -2 : (int) frames - 24;
        f1 = (int) frames + 24;
    }
    /* a number every 2 frames, every 5 once they have three digits (they would touch) */
    int step = ui_roll.no_rolls && f1 >= 100 ? 5 : 2;
    for (int f = f0; f <= f1; f++)
    {
        float rel = f - frames;
        if (absf(rel) > 22) continue;
        int a = -16384 + (int) (rel * per);
        int r = f % step == 0 || f < 0 ? DIAL_R - 16 : DIAL_R - 10;
        int x = cx + fos_cos(a) * r / 16, y = cy + fos_sin(a) * r / 16;
        float near = clampf(1 - absf(rel) / 3.0f, 0, 1);
        if (f == -2)
            fos_text_c(cv, &fos_font_mdb, x >> 8, (y >> 8) + 7, "S", FC(RAMP_RED, 140 + (int) (near * 40)), OPAQUE, 0);
        else if (f >= 0 && f % step == 0)
        {
            char b[8];
            snprintf(b, sizeof(b), "%d", f);
            fcol c = near > 0.5f ? C_TEXT : FC(RAMP_GRAY, 120 + (int) (near * 100));
            fos_text_c(cv, &fos_font_mdb, x >> 8, (y >> 8) + 7, b, c, OPAQUE, 0);
        }
        else if (f > 0)
            fos_disc(cv, x, y, 2 * 256, FC(RAMP_GRAY, 120), OPAQUE);
        int t0 = DIAL_R - 4, t1 = DIAL_R;
        fos_line(cv, cx + fos_cos(a) * t0 / 16, cy + fos_sin(a) * t0 / 16, cx + fos_cos(a) * t1 / 16,
                 cy + fos_sin(a) * t1 / 16, 300, FC(RAMP_METAL, 90), OPAQUE);
    }
    /* the fixed mark */
    fos_tri(cv, DIAL_CX, DIAL_CY - DIAL_R - 6, 12, 1, C_ACCENT, OPAQUE);
    /* the middle: what the counter means */
    char b[16];
    int shown = (int) (frames + 0.5f);
    if (frames < -0.75f) snprintf(b, sizeof(b), "S");
    else snprintf(b, sizeof(b), "%d", shown < 0 ? 0 : shown);
    fos_text_c(cv, &fos_font_num, DIAL_CX, DIAL_CY + 26, b, C_TEXT, OPAQUE, 0);
    fos_text_c(cv, &fos_font_xs, DIAL_CX, DIAL_CY + 52, "FRAMES", C_TEXT3, OPAQUE, 48);
}

void roll_draw()
{
    if (!ui_clobbered && sheet.x <= 0 && !roll_sheet_drawn && !rewinding)
    {
        /* only the dial turns */
        struct fos_canvas save = *cv;
        ui_bg_restore(DIAL_BOX_X0, DIAL_BOX_Y0, DIAL_BOX_X1 - DIAL_BOX_X0, DIAL_BOX_Y1 - DIAL_BOX_Y0);
        fos_clip(cv, DIAL_BOX_X0, DIAL_BOX_Y0, DIAL_BOX_X1, DIAL_BOX_Y1);
        counter_dial(dial.x);
        *cv = save;
        return;
    }
    ui_clobbered = 0;
    roll_sheet_drawn = sheet.x > 0;
    ui_bg_restore(0, 0, FOS_W, FOS_H);
    counter_dial(dial.x);
    int x = 336;
    char b[96];
    if (!ui_roll.loaded)
    {
        fos_text(cv, &fos_font_xs, x, 96, "NO ROLL", C_ACCENT, OPAQUE, 64);
        fos_text(cv, &fos_font_lg, x, 132, "The camera is empty", C_TEXT, OPAQUE, 0);
        fos_text(cv, &fos_font_sm, x, 160, "Load a roll from the film shelf.", C_TEXT3, OPAQUE, 0);
        return;
    }
    const struct fos_film *f = &fos_films[ui_roll.film];
    if (ui_roll.no_rolls) snprintf(b, sizeof(b), "IN CAMERA");          /* no rolls: no roll number, no size */
    else snprintf(b, sizeof(b), "ROLL %03d", ui_roll.number);
    fos_text(cv, &fos_font_xs, x, 96, b, C_ACCENT, OPAQUE, 64);
    fos_text(cv, &fos_font_lg, x, 132, f->name, C_TEXT, OPAQUE, 0);
    if (ui_roll.no_rolls) snprintf(b, sizeof(b), "%s " T_DOT " ISO %d", f->process, f->box_iso);
    else snprintf(b, sizeof(b), "%s " T_DOT " ISO %d " T_DOT " %d exposures", f->process, f->box_iso, ui_roll.exposures);
    fos_text(cv, &fos_font_sm, x, 158, b, C_TEXT2, OPAQUE, 0);
    if (!ui_roll.no_rolls && ui_roll.loaded_at[0])
    {
        snprintf(b, sizeof(b), "Loaded %s", ui_roll.loaded_at);
        fos_text(cv, &fos_font_sm, x, 180, b, C_TEXT3, OPAQUE, 0);
    }

    /* the last frames, newest first */
    int y = 220;
    fos_text(cv, &fos_font_xs, x, y, "LAST FRAMES", C_TEXT3, OPAQUE, 48);
    fos_fill(cv, x, y + 8, 340, 1, C_LINE);
    y += 34;
    if (!ui_roll.nrecent)
        fos_text(cv, &fos_font_sm, x, y, ui_roll.no_rolls ? "Nothing shot yet." : "Nothing shot yet " T_MDASH " the roll is fresh.", C_TEXT3, OPAQUE, 0);
    for (int i = 0; i < ui_roll.nrecent && i < FOS_RECENT; i++, y += 30)
    {
        snprintf(b, sizeof(b), "%d", ui_roll.recent[i].no);
        fos_text_r(cv, &fos_font_mdb, x + 30, y, b, i == 0 ? C_TEXT : C_TEXT2, OPAQUE, 0);
        fos_text(cv, &fos_font_md, x + 54, y, ui_roll.recent[i].time, C_TEXT2, OPAQUE, 0);
        snprintf(b, sizeof(b), "f/%s", ui_roll.recent[i].fstop);
        fos_text(cv, &fos_font_md, x + 140, y, b, C_TEXT3, OPAQUE, 0);
    }

    if (rewinding && ui_realistic())
    {
        float t = clampf(rewind_t / 900.0f, 0, 1);
        fos_text_c(cv, &fos_font_lg, 360, 420, "Rewinding" T_ELLIPSIS, FC(RAMP_GRAY, (int) (240 * ease_out(t))), OPAQUE, 0);
    }
    char body[96];
    int left = ui_roll.exposures - ui_roll.shot;
    if (left > 0)
    {
        snprintf(b, sizeof(b), ui_realistic() ? "Rewind roll %03d?" : "Finish roll %03d?", ui_roll.number);
        snprintf(body, sizeof(body), "%d frames stay unexposed. Develop it now, or load a new roll first.", left);
    }
    else
    {
        snprintf(b, sizeof(b), "Roll %03d is finished", ui_roll.number);
        snprintf(body, sizeof(body), ui_realistic() ? "Rewind it, then develop it now or load a new roll first."
                                                    : "Develop it now, or load a new roll first.");
    }
    ui_sheet3(sheet.x, b, body, "Develop now", "New roll", left > 0 ? "Cancel" : "Not now");
}
