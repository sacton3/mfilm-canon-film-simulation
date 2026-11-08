/* fos_shelf: the film shelf - the canisters on a dark stage, turning
 * towards you as they glide past - and loading a roll.
 *
 * The carousel position is a spring that chases the selected film, so
 * quick turns of the dial pile up into one smooth glide, and every
 * canister's place, size, light and turn follow from how far it is from
 * that position. The text below follows the same position: it slides out
 * with the film leaving the middle and in with the one arriving.
 *
 * The films stand on shelves by kind (colour, slide, B&W, cine): up and
 * down change shelf, and the canisters of the next one drop in.
 *
 * PLAY shows the film's sample: one photo developed with every film when
 * the card was made (ML/SAMPLES), full screen on its own palette.
 */

#include "fos_ui.h"
#include "fos_dev.h"
#include <string.h>

#define FLOOR_Y     262
#define DOTS_Y      290
#define BRAND_Y     320
#define NAME_Y      366
#define DESC_Y      392
#define CHIPS_Y     406

#define TABS_Y      56
#define MAX_CANS    10      /* labels painted at a time: the shelf's middle and a few around */
#define MAX_LIST    96      /* films on a shelf */

/* everything that moves on the shelf is between these rows */
#define MOVING_Y0   (STATUS_H - 8)
#define MOVING_Y1   HINTS_Y

static struct { int film; struct fos_can *c; unsigned used; } cans[MAX_CANS];
static unsigned can_clock;
static uint8_t *info_mask;         /* the name block for fading, see info_build() */
static int info_film = -1, info_x0, info_x1;
static int16_t info_row0[128], info_row1[128];     /* inked span of each row */

static struct spring pos;           /* carousel, in places on the shelf */
static int target;                  /* the selected film's place on the shelf */
static int group;                   /* the shelf: FOS_GROUP_* */
static int list[MAX_LIST], nlist;   /* its films */
static int group_at[FOS_NGROUPS];   /* the place last chosen on each shelf */
static struct spring drop;          /* a new shelf's canisters dropping in, 1 -> 0 */
static int drop_dir;
static int exp_sel;                 /* the roll limit's size (an index into fos_exposure_choices); the shelf no longer changes it */
static struct spring exp_roll;      /* the exposures chip rolls to the new number */

/* the roll size the canisters wear: the roll limit's; with no rolls, none */
static int shelf_exposures()
{
    return ui_roll.no_rolls ? 0 : fos_exposure_choices[exp_sel];
}

/* the film in the camera: with no rolls always; else while its roll has frames left */
static int film_in_camera()
{
    return ui_roll.loaded && (ui_roll.no_rolls || ui_roll.shot < ui_roll.exposures);
}

/* choosing another film would end a roll with frames left: ask first (never with no rolls, where the film just changes) */
static int roll_in_progress()
{
    return !ui_roll.no_rolls && ui_roll.loaded && ui_roll.shot < ui_roll.exposures;
}
static int exp_prev;
static int nudge;                   /* bump at either end */

/* rewind-and-load question */
static int asking;
static struct spring sheet;

/* loading a roll */
static int loading, load_t;

static int sheet_drawn;             /* the last frame had the sheet on it */

static char baked_clock[16];
static void bake();

/* the sample, full screen; changing what is shown goes through black, as
 * the prints do (fos_dark.c) */
enum { SM_OFF, SM_WAIT, SM_IN, SM_SHOW, SM_OUT };
enum { SM_NEXT_LOAD, SM_NEXT_BACK, SM_NEXT_ROLL };
static int sm_state, sm_t, sm_next;
static int sm_black;                /* the black before the picture is on screen */
static int has_samples;             /* the card has them (made for this camera) */
static struct fos_print sm_print;
static int sm_have;                 /* sm_print holds the film's sample */
static int sm_info = 1;             /* the film's details along the bottom */
static int sm_dirty;
#define SM_FADE_MS  180
#define SM_INFO_H   44

static void sample_path(char *buf, int size, int film)
{
    snprintf(buf, size, "%sML/SAMPLES/F%03d.BMP", fos_port_card(), film);
}

/* ---------------------------------------------------------------- canisters */

/* a film's canister: kept painted for the last few asked for */
struct fos_can *ui_can(int film)
{
    if (film < 0 || film >= fos_nfilms) return 0;
    int slot = 0;
    for (int i = 0; i < MAX_CANS; i++)
    {
        if (cans[i].c && cans[i].film == film)
        {
            cans[i].used = ++can_clock;
            return cans[i].c;
        }
        if (!cans[i].c || (cans[slot].c && cans[i].used < cans[slot].used)) slot = i;
    }
    fos_can_free(cans[slot].c);
    cans[slot].c = fos_can_new(film, shelf_exposures());
    cans[slot].film = film;
    cans[slot].used = ++can_clock;
    return cans[slot].c;
}

void ui_cans_free()
{
    fos_port_free(info_mask);
    info_mask = 0;
    info_film = -1;
    for (int i = 0; i < MAX_CANS; i++)
    {
        fos_can_free(cans[i].c);
        cans[i].c = 0;
    }
}

void ui_cans_exposures(int exposures)
{
    for (int i = 0; i < MAX_CANS; i++)
        if (cans[i].c) fos_can_set_exposures(cans[i].c, exposures);
}

/* the films of shelf g, and the place of film on it (0 if not there) */
static int shelf_list(int g, int film)
{
    int at = 0;
    group = g;
    nlist = 0;
    for (int i = 0; i < fos_nfilms && nlist < MAX_LIST; i++)
        if (fos_films[i].group == g)
        {
            if (i == film) at = nlist;
            list[nlist++] = i;
        }
    return at;
}

/* the labels the shelf will show first, painted now rather than mid-glide */
static void paint_around(int at)
{
    for (int d = 0; d <= 3; d++)
    {
        if (at + d < nlist) ui_can(list[at + d]);
        if (d && at - d >= 0) ui_can(list[at - d]);
    }
}

/* ---------------------------------------------------------------- the stage */

static float absf(float v) { return v < 0 ? -v : v; }

#ifndef ABS
#define ABS(v) ((v) < 0 ? -(v) : (v))
#endif

/* the stage's light at (x, y), 12-bit */
static int stage_lv(int x, int y)
{
    int dx = x - 360;
    int lv;
    if (y < FLOOR_Y - 6)
    {
        /* backdrop: a soft spotlight behind the middle canister */
        int dy = (y - 150) * 3 / 2;
        int d2 = (dx * dx + dy * dy) >> 4;
        int spot = d2 < 9000 ? (9000 - d2) * (9000 - d2) / 9000 * LV12(20) / 9000 : 0;
        lv = LV12(10) + spot + (FLOOR_Y - y) * LV12(3) / FLOOR_Y;
    }
    else
    {
        /* floor: a lit ellipse under the canister fading into the dark */
        int dy = (y - (FLOOR_Y + 4)) * 5;
        int d2 = (dx * dx + dy * dy) >> 4;
        int pool = d2 < 7000 ? (7000 - d2) * LV12(13) / 7000 : 0;
        int fall = fos_clamp((y - FLOOR_Y) * LV12(6) / 60, 0, LV12(6));
        lv = LV12(13) + pool - fall;
        if (y > FLOOR_Y + 40) lv = lv * (FOS_H - y) / (FOS_H - FLOOR_Y - 40);
        lv = fos_clamp(lv, LV12(4), LV_MAX);
    }
    /* the seam between backdrop and floor, very soft */
    if (y >= FLOOR_Y - 6 && y < FLOOR_Y + 2)
        lv = lv * (256 - (4 - ABS(y - (FLOOR_Y - 2))) * 10) / 256;
    return lv;
}

/* the stage: a backdrop lit from above, a floor, a pool of light in the
 * middle. The light changes slowly, so it is worked out every 8 px and
 * stepped in between; rows y0..y1-1 only */
static void stage_bg(int y0, int y1)
{
    const int mul = fos_ramp_mul[RAMP_GRAY];
    const int base = fos_ramp_base[RAMP_GRAY], last = fos_ramp_last[RAMP_GRAY];
    for (int y = y0; y < y1; y++)
    {
        uint8_t *row = &ui_bg[y * FOS_W];
        const uint8_t *bay = fos_bayer[y & 3];
        int p1 = stage_lv(0, y) * mul;
        for (int x = 0; x < FOS_W; x += 8)
        {
            int p0 = p1;
            p1 = stage_lv(x + 8, y) * mul;
            int p = p0, dp = (p1 - p0) >> 3;
            for (int i = 0; i < 8; i++, p += dp)
            {
                int q = ((p >> 16) + bay[i & 3]) >> 4;
                row[x + i] = base + (q > last ? last : q);
            }
        }
    }
}

/* where film i stands for carousel offset d (films from the middle) */
static void pose_of(float d, struct fos_can_pose *p, float lift, float spread)
{
    float ad = absf(d);
    float n = clampf(ad, 0, 1), f = clampf(ad - 1, 0, 1.8f);
    float sx = d < 0 ? -1 : 1;
    float x = 360 + sx * (n * 192 + f * 122) * spread;
    float scale = 1.0f - n * 0.27f - f * 0.17f;
    float light = 1.0f - n * 0.42f - f * 0.30f;
    float rot = d * 0.085f;                         /* turns: faces the middle */
    p->cx8 = (int) (x * 256);
    p->floor_y = (int) (FLOOR_Y - n * 12 - f * 8 - lift);
    p->scale = (int) (scale * 256);
    p->rot = (int) (rot * 65536);
    p->light = (int) (clampf(light, 0, 1) * 256);
    p->leader = 256;
    p->fast = absf(pos.v) > 1.2f;      /* films a second: too quick to see the fine print */
    /* the middle one is mirrored in the floor, fading as it leaves */
    p->reflect = (int) (64 * clampf(1.25f - absf(d), 0, 1));
}

/* a soft contact shadow under a canister */
static void contact_shadow(int cx, int y, int rx, int ry, int strength)
{
    if (rx <= 0 || ry <= 0) return;
    int x0 = fos_clamp(cx - rx, cv->cx0, cv->cx1), x1 = fos_clamp(cx + rx, cv->cx0, cv->cx1);
    int y0 = fos_clamp(y - ry, cv->cy0, cv->cy1), y1 = fos_clamp(y + ry, cv->cy0, cv->cy1);
    int sx = 65536 / rx, sy = 65536 / ry;       /* 1/256 of the radius per px */
    for (int j = y0; j < y1; j++)
    {
        int dy = ((j - y) * sy) >> 8;
        if (dy * dy >= 65536) continue;
        /* only the part of the row inside the ellipse */
        int half = (fos_isqrt(65536 - dy * dy) * rx >> 8) + 1;
        int xa = fos_clamp(cx - half, x0, x1), xb = fos_clamp(cx + half + 1, xa, x1);
        int dx8 = (xa - cx) * sx;
        uint8_t *p = &cv->px[j * cv->pitch + xa];
        for (int i = xa; i < xb; i++, p++, dx8 += sx)
        {
            int dx = dx8 >> 8;
            int d2 = (dx * dx + dy * dy) >> 8;
            if (d2 >= 256) continue;
            int k = 256 - ((strength * (((256 - d2) * (256 - d2)) >> 8)) >> 8);
            *p = fos_darken(*p, k, i, j);
        }
    }
}

/* ---------------------------------------------------------------- screen */

void shelf_enter(int arg)
{
    exp_sel = fos_clamp(fos_port_get(FS_ROLLLIM) - 1, 0, FOS_NEXP - 1);      /* 36, 24 or 12 with a roll limit; unused without */
    exp_prev = exp_sel;
    exp_roll.x = 1; exp_roll.v = 0;
    int film = film_in_camera() ? ui_roll.film : fos_port_get(FS_FILM);
    film = fos_clamp(film, 0, fos_nfilms - 1);
    target = shelf_list(fos_films[film].group, film);
    for (int g = 0; g < FOS_NGROUPS; g++) group_at[g] = 0;
    group_at[group] = target;
    drop.x = 0; drop.v = 0;
    pos.x = target; pos.v = 0;
    /* arriving from elsewhere: the shelf glides in from one film over */
    if (arg == 0) pos.x = target - 0.6f;
    nudge = 0;
    asking = 0;
    sheet.x = 0; sheet.v = 0;
    loading = 0;
    sheet_drawn = 0;
    sm_state = SM_OFF;
    sm_have = 0;
    char path[64];
    sample_path(path, sizeof(path), 0);
    void *f = fos_port_open(path, 0);
    has_samples = f != 0;
    if (f) fos_port_close(f);
    paint_around(target);
    ui_cans_exposures(shelf_exposures());
    bake();
}

void shelf_leave()
{
    if (sm_have) fos_print_free(&sm_print);
    sm_have = 0;
    if (sm_state != SM_OFF) fos_port_palette();
    sm_state = SM_OFF;
}

static void start_loading()
{
    loading = 1;
    load_t = 0;
    asking = 0;
    bake();
    fos_port_set(FS_FILM, list[target]);
    fos_port_set(FS_EXP, exp_sel);
    fos_port_action(FA_LOAD, list[target], exp_sel);
    fos_port_roll(&ui_roll);
}

/* what the next frame changes: the whole screen while the sheet or the
 * loading animation is up, else the band between status bar and hints */
static void shelf_damage()
{
    if (loading || asking || sheet.x > 0 || sheet_drawn || sm_state != SM_OFF) ui_damage_all();
    else ui_damage(MOVING_Y0, MOVING_Y1);
}

/* ---------------------------------------------------------------- the sample */

static void sm_load()
{
    char path[64];
    if (sm_have) fos_print_free(&sm_print);
    sm_have = 0;
    sample_path(path, sizeof(path), list[target]);
    sm_have = fos_print_load(path, &sm_print);
    /* made with another set of films: not this one's */
    if (sm_have && sm_print.film != list[target]) { fos_print_free(&sm_print); sm_have = 0; }
    if (!sm_have)
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "shelf: no sample in %s", path);
        fos_port_log(msg);
    }
    sm_dirty = 1;
}

static void sm_palette(int k)
{
    static const uint8_t black[1][3] = { { 0, 0, 0 } };
    if (sm_have) fos_port_photo((const uint8_t (*)[3]) sm_print.pal, FOS_PRINT_COLOURS, k);
    else fos_port_photo(black, 1, k);
}

static void sm_go(int next)
{
    if (sm_state == SM_OUT) return;
    sm_next = next;
    sm_state = SM_OUT;
    sm_t = 0;
}

static void sample_key(int k)
{
    if (sm_state != SM_SHOW) return;
    switch (k)
    {
        case FK_LEFT: case FK_WHEEL_L: case FK_RIGHT: case FK_WHEEL_R:
        {
            /* the next film on this shelf; the shelf follows */
            int to = target + (k == FK_LEFT || k == FK_WHEEL_L ? -1 : 1);
            if (to < 0 || to >= nlist) break;
            target = to;
            group_at[group] = target;
            sm_go(SM_NEXT_LOAD);
            break;
        }
        case FK_INFO:
            sm_info = !sm_info;
            sm_dirty = 1;
            break;
        case FK_SET:
            sm_go(SM_NEXT_ROLL);
            break;
        case FK_MENU: case FK_PLAY: case FK_HOME:
            sm_go(SM_NEXT_BACK);
            break;
    }
}

static int sample_update(int dt)
{
    sm_t += dt;
    switch (sm_state)
    {
        case SM_WAIT:
            /* once the screen is black in the film OS's colours, the picture */
            if (!sm_black) return 1;
            sm_load();
            sm_palette(0);
            sm_state = SM_IN;
            sm_t = 0;
            return 1;
        case SM_IN:
            sm_palette(fos_clamp(sm_t * 256 / SM_FADE_MS, 0, 256));
            if (sm_t >= SM_FADE_MS) sm_state = SM_SHOW;
            return 1;
        case SM_OUT:
            sm_palette(fos_clamp(256 - sm_t * 256 / SM_FADE_MS, 0, 256));
            if (sm_t < SM_FADE_MS) return 1;
            if (sm_next == SM_NEXT_LOAD)
            {
                sm_load();
                sm_state = SM_IN;
                sm_t = 0;
                return 1;
            }
            /* back to the shelf's colours, on black, at the film shown last */
            sm_state = SM_OFF;
            if (sm_have) fos_print_free(&sm_print);
            sm_have = 0;
            memset(cv->px, fos_ix(FC(RAMP_GRAY, 0)), FOS_W * FOS_H);
            fos_port_present(cv->px, FOS_W, 0, FOS_H);
            fos_port_palette();
            pos.x = target; pos.v = 0;
            paint_around(target);
            ui_clobbered = 1;
            if (sm_next == SM_NEXT_ROLL)
            {
                if (roll_in_progress()) asking = 1;
                else start_loading();
            }
            shelf_damage();
            return 1;
    }
    return 0;
}

static void sample_draw()
{
    if (sm_state == SM_WAIT)
    {
        memset(cv->px, fos_ix(FC(RAMP_GRAY, 0)), FOS_W * FOS_H);
        ui_clobbered = 0;
        ui_damage_all();
        sm_black = 1;
        return;
    }
    if (sm_state == SM_OUT) return;         /* what is fading out stays as it was */
    if (!sm_dirty && !ui_clobbered) return;
    sm_dirty = 0;
    ui_clobbered = 0;
    const struct fos_film *f = &fos_films[list[target]];
    if (sm_have) memcpy(cv->px, sm_print.px, FOS_W * FOS_H);
    else
    {
        memset(cv->px, PX_BLACK, FOS_W * FOS_H);
        ui_photo_text(&fos_font_md, 360, 244, "No sample for this film", 0, 1);
    }
    if (sm_info)
    {
        char b[96];
        int y = FOS_H - SM_INFO_H;
        memset(&cv->px[y * cv->pitch], PX_PANEL, FOS_W * SM_INFO_H);
        memset(&cv->px[y * cv->pitch], PX_LINE, FOS_W);
        int x = 20 + ui_photo_text(&fos_font_xs, 20, y + 28, "SAMPLE", 1, 0);
        snprintf(b, sizeof(b), "%s %s " T_DOT " ISO %d " T_DOT " %s", f->brand, f->name, f->box_iso, f->process);
        ui_photo_text(&fos_font_sm, x + 14, y + 28, b, 0, 0);
        ui_photo_text(&fos_font_xs, FOS_W - 20, y + 28, "SET LOAD   PLAY BACK", 0, 2);
    }
    ui_damage_all();
}

/* ---------------------------------------------------------------- keys */

void shelf_key(int k)
{
    if (loading) return;
    if (sm_state != SM_OFF)
    {
        sample_key(k);
        shelf_damage();
        return;
    }
    if (asking)
    {
        if (k == FK_SET) start_loading();
        else if (k == FK_MENU || k == FK_LEFT || k == FK_RIGHT || k == FK_WHEEL_L || k == FK_WHEEL_R)
            asking = 0;
        shelf_damage();
        return;
    }
    switch (k)
    {
        case FK_LEFT: case FK_WHEEL_L:
            if (target > 0) target--; else nudge = -1;
            break;
        case FK_RIGHT: case FK_WHEEL_R:
            if (target < nlist - 1) target++; else nudge = 1;
            break;
        case FK_UP: case FK_DOWN:
        {
            /* the next shelf (skipping empty ones), at the film last chosen there */
            int dir = k == FK_DOWN ? 1 : -1, from = group, g = group;
            group_at[group] = target;
            do
            {
                g = (g + dir + FOS_NGROUPS) % FOS_NGROUPS;
                shelf_list(g, -1);
            }
            while (!nlist && g != from);
            target = fos_clamp(group_at[g], 0, nlist - 1);
            pos.x = target; pos.v = 0;
            drop.x = 1; drop.v = 0;
            drop_dir = dir;
            paint_around(target);
            bake();
            ui_clobbered = 1;
            break;
        }
        case FK_INFO:
            /* the roll size is the roll limit's now (System): nothing to change here */
            break;
        case FK_SET:
            if (roll_in_progress()) asking = 1;
            else start_loading();
            break;
        case FK_MENU:
            if (ui_from() == SCR_HOME) ui_go(SCR_HOME, 0, GO_POP);
            else ui_leave();
            break;
        case FK_PLAY:
            if (has_samples)
            {
                /* the canisters settle where they are going first */
                pos.x = target; pos.v = 0;
                sm_state = SM_WAIT;
                sm_t = 0;
                sm_black = 0;
                break;
            }
            ui_go(SCR_HOME, 0, GO_POP);
            break;
        case FK_HOME:
            ui_go(SCR_HOME, 0, GO_POP);
            break;
    }
    shelf_damage();
}

int shelf_update(int dt)
{
    if (sm_state != SM_OFF) return sample_update(dt);
    int moving = 0;
    char clock[16];
    fos_port_clock(clock, sizeof(clock));
    if (strcmp(clock, baked_clock))
    {
        /* a new minute: only the status bar changes */
        stage_bg(0, STATUS_H);
        ui_to_bg(1);
        ui_status_bar("FILM");
        ui_to_bg(0);
        memcpy(baked_clock, clock, sizeof(baked_clock));
        ui_damage(0, STATUS_H);
        ui_clobbered = 1;
    }
    float goal = target + (nudge ? nudge * 0.18f : 0);
    moving |= spring_step(&pos, goal, dt, 2.1f, 0.86f);
    if (nudge && absf(pos.x - goal) < 0.05f) nudge = 0;
    moving |= spring_step(&exp_roll, 1, dt, 3.2f, 0.9f);
    moving |= spring_step(&sheet, asking ? 1 : 0, dt, 3.0f, 0.95f);
    moving |= spring_step(&drop, 0, dt, 2.6f, 0.9f);
    if (loading)
    {
        load_t += dt;
        moving = 1;
        if (load_t > 1900) ui_leave();
    }
    if (moving)
        shelf_damage();
    return moving;
}

/* ---------------------------------------------------------------- drawing */

static void draw_canisters(float lift_mid, float spread, float fade_sides, int leader_mid)
{
    int t0 = fos_port_ms();
    struct { int i; float d; } order[8];
    int n = 0;
    for (int i = 0; i < nlist && n < 8; i++)
    {
        float d = i - pos.x;
        if (absf(d) > 2.7f) continue;
        order[n].i = i; order[n].d = d; n++;
    }
    /* far ones first */
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (absf(order[b].d) > absf(order[a].d))
            {
                int ti = order[a].i; float td = order[a].d;
                order[a] = order[b];
                order[b].i = ti; order[b].d = td;
            }

    for (int k = 0; k < n; k++)
    {
        int i = list[order[k].i];
        float d = order[k].d;
        struct fos_can_pose p;
        int mid = absf(d) < 0.5f;
        pose_of(d, &p, mid ? lift_mid : 0, spread);
        if (!mid) p.light = (int) (p.light * (1 - fade_sides));
        if (drop.x > 0.002f)
        {
            /* a new shelf: its canisters drop in from above (or rise from below), the far ones a little later */
            float t = clampf(drop.x * (1 + absf(d) * 0.25f), 0, 1);
            p.floor_y -= (int) (t * 90 * drop_dir);
            p.light = (int) (p.light * (1 - t));
            p.reflect = (int) (p.reflect * (1 - t));
        }
        if (mid && leader_mid) p.leader = leader_mid;
        if (p.light <= 4) continue;
        struct fos_can *c = ui_can(i);
        int w = fos_can_width(p.scale);
        contact_shadow(p.cx8 >> 8, p.floor_y, w * 6 / 10, 7 * p.scale / 256 + 2, 150 * p.light / 256);
        fos_can_draw(cv, c, &p);

        /* the film that is in the camera wears a tag */
        if (film_in_camera() && i == ui_roll.film && !loading)
        {
            int cx = p.cx8 >> 8;
            int top = p.floor_y - fos_can_height(p.scale) - 14;
            int a = (int) (p.light * 1.0f);
            const char *t = "IN CAMERA";
            int tw = fos_text_w(&fos_font_xs, t, 24);
            fos_rrect(cv, cx - tw / 2 - 10, top - 13, tw + 20, 20, 10, FC(RAMP_AMBER, 186 * a / 256), OPAQUE);
            fos_text_c(cv, &fos_font_xs, cx, top + 2, t, FC(RAMP_GRAY, 14), OPAQUE, 24);
        }
    }
    ui_cans_ms += fos_port_ms() - t0;
}

/* chips under the name: fixed sizes, so only the values change */
#define CHIP_H 26
static const char *chip_labels[3] = { "ISO", "PROCESS", "EXP" };
static const char *chip_widest[3] = { "800", "C-41", "36" };
static int chip_x[3], chip_w[3], chip_vx[3];

/* ISO and PROCESS, and with a roll limit EXP: the size, read only (no rolls: there is none to show) */
static int nchips() { return ui_roll.no_rolls ? 2 : 3; }

static void chips_layout()
{
    int total = 0;
    for (int i = 0; i < nchips(); i++)
    {
        int lw = fos_text_w(&fos_font_xs, chip_labels[i], 24);
        int vw = fos_text_w(&fos_font_mdb, chip_widest[i], 0);
        chip_w[i] = lw + vw + 32;
        total += chip_w[i] + (i ? 10 : 0);
    }
    int x = 360 - total / 2;
    for (int i = 0; i < nchips(); i++)
    {
        chip_x[i] = x;
        chip_vx[i] = x + 12 + fos_text_w(&fos_font_xs, chip_labels[i], 24) + 8;
        x += chip_w[i] + 10;
    }
}

static void draw_static(int alpha)
{
    struct hint h[] = { { "<>", "Film" }, { "^v", "Shelf" },
                        { "SET", ui_roll.no_rolls ? "Use film" : roll_in_progress() ? (ui_realistic() ? "Rewind & load" : "Finish & load") : "Load" },
                        { "MENU", "Back" }, { "PLAY", "Sample" } };
    if (alpha >= OPAQUE) ui_hints(h, has_samples ? 5 : 4);
    /* the shelves, this one lit */
    int tw = 0;
    for (int g = 0; g < FOS_NGROUPS; g++) tw += fos_text_w(&fos_font_xs, fos_group_names[g], 64) + (g ? 30 : 0);
    int tx = 360 - tw / 2;
    for (int g = 0; g < FOS_NGROUPS; g++)
    {
        int w = fos_text_w(&fos_font_xs, fos_group_names[g], 64);
        fos_text(cv, &fos_font_xs, tx, TABS_Y, fos_group_names[g], g == group ? C_ACCENT : C_TEXT3, alpha, 64);
        if (g == group) fos_rrect(cv, tx + w / 2 - 8, TABS_Y + 6, 16, 3, 1, C_ACCENT, alpha);
        tx += w + 30;
    }
    for (int i = 0; i < nchips(); i++)
    {
        int exp = i == 2;
        fos_rrect(cv, chip_x[i], CHIPS_Y, chip_w[i], CHIP_H, CHIP_H / 2, exp ? FC(RAMP_GRAY, 24) : FC(RAMP_GRAY, 17), alpha);
        fos_rrect_line(cv, chip_x[i], CHIPS_Y, chip_w[i], CHIP_H, CHIP_H / 2, 1, exp ? C_ACCENT2 : FC(RAMP_GRAY, 46), alpha);
        fos_text(cv, &fos_font_xs, chip_x[i] + 12, CHIPS_Y + 18, chip_labels[i], C_TEXT3, alpha, 24);
    }
    int n = nlist, x0 = 360 - n * 6;
    for (int i = 0; i < n; i++)
        fos_disc(cv, (x0 + i * 12 + 6) * 256, DOTS_Y * 256, 2 * 256, FC(RAMP_GRAY, 60), alpha);
}

/* the stage and everything that doesn't move, drawn once; while a roll
 * loads, only the stage stays */
static void bake()
{
    stage_bg(0, FOS_H);
    ui_to_bg(1);
    ui_status_bar("FILM");
    chips_layout();
    if (!loading) draw_static(OPAQUE);
    ui_to_bg(0);
    fos_port_clock(baked_clock, sizeof(baked_clock));
}

static void draw_dot_marker()
{
    /* the current film: a short amber bar sliding with the carousel */
    int x0 = 360 - nlist * 6;
    float px = x0 + clampf(pos.x, 0, nlist - 1) * 12 + 6;
    fos_rrect(cv, (int) px - 7, DOTS_Y - 2, 14, 4, 2, C_ACCENT, OPAQUE);
}

/* The film's name block, kept for fading: per pixel its ink (high
 * nibble) and coverage (low nibble), so a frame of the glide blends the
 * pixels without decoding the glyphs again. */
#define INFO_Y0 (BRAND_Y - 16)
#define INFO_Y1 (DESC_Y + 8)
#define INFO_H  (INFO_Y1 - INFO_Y0)

static void info_build(int film)
{
    const struct fos_film *f = &fos_films[film];
    const struct { const struct fos_font *font; int y; const char *s; int track; } part[3] =
    {
        { &fos_font_xs, BRAND_Y, f->brand, 64 },
        { &fos_font_xl, NAME_Y, f->name, 0 },
        { &fos_font_sm, DESC_Y, f->desc, 0 },
    };
    memset(info_mask, 0, FOS_W * INFO_H);
    info_x0 = FOS_W; info_x1 = 0;
    for (int i = 0; i < 3; i++)
    {
        uint8_t map[16];
        map[0] = 0;
        for (int v = 1; v < 16; v++) map[v] = (i + 1) << 4 | v;
        int w = fos_text_w(part[i].font, part[i].s, part[i].track);
        int x = 360 - w / 2;
        fos_text_map(info_mask, FOS_W, FOS_W, INFO_H, part[i].font, x, part[i].y - INFO_Y0, part[i].s, map, part[i].track);
        if (x - 4 < info_x0) info_x0 = fos_clamp(x - 4, 0, FOS_W);
        if (x + w + 4 > info_x1) info_x1 = fos_clamp(x + w + 4, 0, FOS_W);
    }
    for (int j = 0; j < INFO_H; j++)
    {
        const uint8_t *m = &info_mask[j * FOS_W];
        int a = info_x0, b = info_x1;
        while (a < b && !m[a]) a++;
        while (b > a && !m[b - 1]) b--;
        info_row0[j] = a; info_row1[j] = b;
    }
    info_film = film;
}

/* the block at opacity alpha, moved xo px. Grey ink over the grey stage
 * blends as levels of one ramp, which is cheap; the few pixels of the
 * amber brand take the full blend. */
static void info_fade(int alpha, int xo)
{
    const fcol inks[4] = { 0, C_ACCENT, C_TEXT, C_TEXT2 };
    int ink_lv[4];
    for (int i = 1; i < 4; i++)
        ink_lv[i] = FC_RAMP(inks[i]) == RAMP_GRAY ? LV12(FC_LEVEL(inks[i])) : -1;
    const int mul = fos_ramp_mul[RAMP_GRAY], base = fos_ramp_base[RAMP_GRAY], last = fos_ramp_last[RAMP_GRAY];
    int ka = alpha * 17;                        /* coverage 15 at full opacity -> 255 */
    for (int j = 0; j < INFO_H; j++)
    {
        int y = INFO_Y0 + j;
        if (y < cv->cy0 || y >= cv->cy1) continue;
        int xa = fos_clamp(info_row0[j], cv->cx0 - xo, cv->cx1 - xo), xb = fos_clamp(info_row1[j], xa, cv->cx1 - xo);
        const uint8_t *m = &info_mask[j * FOS_W];
        uint8_t *row = &cv->px[y * cv->pitch + xo];
        uint8_t bay[4];
        for (int k = 0; k < 4; k++)
            bay[k] = fos_bayer[y & 3][(k + xo) & 3];
        for (int x = xa; x < xb; x++)
        {
            int v = m[x];
            if (!v) continue;
            int op = ((v & 15) * ka) >> 8;
            int k = v >> 4, d = row[x];
            if (ink_lv[k] >= 0 && fos_rev_ramp[d] == RAMP_GRAY)
            {
                int lv = fos_rev_lv[d] + (((ink_lv[k] - fos_rev_lv[d]) * op) >> 8);
                int q = (((lv * mul) >> 16) + bay[x & 3]) >> 4;
                row[x] = base + (q > last ? last : q);
            }
            else
                row[x] = fos_mix(d, FC_RAMP(inks[k]), LV12(FC_LEVEL(inks[k])), op);
        }
    }
}

static void draw_info()
{
    int place = (int) (pos.x + 100.5f) - 100;
    place = fos_clamp(place, 0, nlist - 1);
    int ni = list[place];
    float frac = pos.x - place;
    float a = clampf(1 - absf(frac) * 2.2f, 0, 1) * (1 - drop.x);
    a = a * a * (3 - 2 * a);
    int alpha = (int) (a * 256);
    int xo = (int) (-frac * 70);
    const struct fos_film *f = &fos_films[ni];

    if (alpha > 0)
    {
        if (!info_mask) info_mask = fos_port_alloc(FOS_W * INFO_H);
        if (alpha < OPAQUE && info_mask)
        {
            if (info_film != ni) info_build(ni);
            info_fade(alpha, xo);
        }
        else
        {
            fos_text_c(cv, &fos_font_xs, 360 + xo, BRAND_Y, f->brand, C_ACCENT, alpha, 64);
            fos_text_c(cv, &fos_font_xl, 360 + xo, NAME_Y, f->name, C_TEXT, alpha, 0);
            fos_text_c(cv, &fos_font_sm, 360 + xo, DESC_Y, f->desc, C_TEXT2, alpha, 0);
        }
        char b[16];
        snprintf(b, sizeof(b), "%d", f->box_iso);
        fos_text(cv, &fos_font_mdb, chip_vx[0], CHIPS_Y + 19, b, C_TEXT, alpha, 0);
        fos_text(cv, &fos_font_mdb, chip_vx[1], CHIPS_Y + 19, f->process, C_TEXT, alpha, 0);
    }

    /* the roll limit's size (no rolls: no chip) */
    if (ui_roll.no_rolls) return;
    struct fos_canvas save = *cv;
    fos_clip(cv, chip_vx[2] - 4, CHIPS_Y + 3, chip_x[2] + chip_w[2] - 8, CHIPS_Y + CHIP_H - 3);
    char nb[8], ob[8];
    snprintf(nb, sizeof(nb), "%d", fos_exposure_choices[exp_sel]);
    snprintf(ob, sizeof(ob), "%d", fos_exposure_choices[exp_prev]);
    int dy = (int) ((1 - exp_roll.x) * 20);
    fos_text(cv, &fos_font_mdb, chip_vx[2], CHIPS_Y + 19 + dy, nb, C_ACCENT, OPAQUE, 0);
    if (exp_roll.x < 1)
        fos_text(cv, &fos_font_mdb, chip_vx[2], CHIPS_Y + 19 + dy - 20, ob, C_ACCENT, OPAQUE, 0);
    *cv = save;
}

static void draw_sheet()
{
    char t[48], b[96];
    snprintf(t, sizeof(t), ui_realistic() ? "Rewind roll %03d?" : "Finish roll %03d?", ui_roll.number);
    snprintf(b, sizeof(b), "%d of %d frames on %s are still unexposed. They will be lost.",
             ui_roll.exposures - ui_roll.shot, ui_roll.exposures, fos_films[ui_roll.film].name);
    ui_sheet(sheet.x, t, b, ui_realistic() ? "Rewind and load" : "Finish and load", "Keep shooting");
}

/* SET: the leader is drawn out of the canister across the stage, the
 * other stocks step aside, and the roll number is stamped */
static void draw_loading()
{
    float t = load_t;
    float aside = ease_in_out(clampf(t / 450.0f, 0, 1));
    float lift = 10 * ease_out(clampf(t / 400.0f, 0, 1));
    float pull = ease_in_out(clampf((t - 250) / 850.0f, 0, 1));
    int leader = 256 + (int) (pull * 256 * 7);
    ui_bg_restore(0, 0, FOS_W, FOS_H);
    /* the chips and dots fade away with the other stocks */
    if (aside < 1) draw_static((int) (256 * (1 - aside)));
    draw_canisters(lift, 1 + aside * 0.8f, aside, leader);

    float ta = ease_out(clampf((t - 900) / 450.0f, 0, 1));
    if (ta > 0)
    {
        int a = (int) (ta * 256);
        char b[48];
        /* no rolls: no number and no size, the film is in the camera */
        if (ui_roll.no_rolls) snprintf(b, sizeof(b), "IN CAMERA");
        else snprintf(b, sizeof(b), "ROLL %03d", ui_roll.number);
        fos_text_c(cv, &fos_font_xs, 360, BRAND_Y + 8, b, C_ACCENT, a, 64);
        fos_text_c(cv, &fos_font_xl, 360, NAME_Y + 8 - (int) ((1 - ta) * 12), ui_roll.no_rolls ? "Ready" : "Loaded", C_TEXT, a, 0);
        if (ui_roll.no_rolls) snprintf(b, sizeof(b), "%s  " T_DOT "  ISO %d", fos_films[list[target]].name, fos_films[list[target]].box_iso);
        else snprintf(b, sizeof(b), "%s  " T_DOT "  %d exposures", fos_films[list[target]].name, fos_exposure_choices[exp_sel]);
        fos_text_c(cv, &fos_font_sm, 360, DESC_Y + 8, b, C_TEXT2, a, 0);
    }
}

void shelf_draw()
{
    if (sm_state != SM_OFF)
    {
        sample_draw();
        return;
    }
    if (loading)
    {
        draw_loading();
        return;
    }
    if (ui_clobbered || sheet.x > 0 || sheet_drawn)
    {
        ui_clobbered = 0;
        ui_damage_all();
        ui_bg_restore(0, 0, FOS_W, FOS_H);
    }
    else
    {
        /* the status bar and the key hints never move here */
        ui_bg_restore(0, MOVING_Y0, FOS_W, MOVING_Y1 - MOVING_Y0);
        ui_damage(MOVING_Y0, MOVING_Y1);
    }
    draw_canisters(0, 1, 0, 0);
    draw_dot_marker();
    draw_info();
    draw_sheet();
    sheet_drawn = sheet.x > 0;
}
