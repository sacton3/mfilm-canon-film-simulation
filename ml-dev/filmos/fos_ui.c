/* fos_ui: the loop, navigation, transitions and shared widgets of the
 * film OS. The screens themselves are in fos_shelf.c and fos_pages.c.
 *
 * One loop reads keys, steps every animation by the time that really
 * passed and redraws only while something moves, so a quiet screen costs
 * nothing. Screens draw into our own canvas; the port copies the rows
 * that changed to the camera's bitmap overlay.
 */

#include "fos_ui.h"
#include "fos_dev.h"
#include <string.h>

struct fos_canvas *cv;
uint8_t *ui_bg;
int ui_now;
struct fos_roll ui_roll;
int ui_clobbered = 1;
int ui_cans_ms;

static struct fos_canvas canvas;
static uint8_t *buf_cv, *buf_bg, *buf_snap;

static int screen, screen_from;
static int dmg_y0 = FOS_H, dmg_y1 = 0;
static int cv_on_screen;                    /* the screen shows the canvas as it is (no screen change on its way) */

/* screen change in progress */
static int tr_how, tr_t, tr_len;            /* tr_t counts up to tr_len (ms) */
static int leaving;                         /* 1 while the shutter opens on LiveView */
static int leave_t;
static int quit;
static int boot_then_leave;

/* opening: the shutter closes over LiveView, then opens on us */
static int enter_t;
#define ENTER_LEN 420
#define LEAVE_LEN 380

/* the toast */
static char toast_msg[80];
static int toast_t = 100000;
#define TOAST_LEN 1800

/* held keys repeat */
static int held_key, held_since, held_last;

/* ---------------------------------------------------------------- animation */

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

float ease_out(float t)
{
    t = clampf(t, 0, 1);
    float u = 1 - t;
    return 1 - u * u * u;
}

float ease_in_out(float t)
{
    t = clampf(t, 0, 1);
    return t < 0.5f ? 4 * t * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) * (-2 * t + 2) / 2;
}

int spring_step(struct spring *s, float target, int dt, float hz, float zeta)
{
    float w = 6.2831853f * hz;
    while (dt > 0)
    {
        float h = (dt > 8 ? 8 : dt) / 1000.0f;
        float a = w * w * (target - s->x) - 2 * zeta * w * s->v;
        s->v += a * h;
        s->x += s->v * h;
        dt -= 8;
    }
    float d = target - s->x;
    if (d < 0.0015f && d > -0.0015f && s->v < 0.02f && s->v > -0.02f)
    {
        s->x = target;
        s->v = 0;
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- damage */

void ui_damage(int y0, int y1)
{
    if (y0 < dmg_y0) dmg_y0 = fos_clamp(y0, 0, FOS_H);
    if (y1 > dmg_y1) dmg_y1 = fos_clamp(y1, 0, FOS_H);
}

void ui_damage_all()
{
    ui_damage(0, FOS_H);
}

/* Canon drew over the screen: the canvas still holds what was presented
 * last, so those rows go back from it now, before Canon's show for another
 * frame (#49, #64). In a screen change the next frame paints them anyway. */
void fos_ui_overdrawn(int y0, int y1)
{
    y0 = fos_clamp(y0, 0, FOS_H);
    y1 = fos_clamp(y1, y0, FOS_H);
    if (cv_on_screen && cv && y1 > y0)
    {
        fos_port_present(cv->px + y0 * FOS_W, FOS_W, y0, y1);
        return;
    }
    ui_clobbered = 1;
    ui_damage_all();
}

/* ---------------------------------------------------------------- widgets */

void ui_bg_restore(int x, int y, int w, int h)
{
    x = fos_clamp(x, 0, FOS_W); y = fos_clamp(y, 0, FOS_H);
    w = fos_clamp(w, 0, FOS_W - x); h = fos_clamp(h, 0, FOS_H - y);
    for (int j = y; j < y + h; j++)
        memcpy(&cv->px[j * cv->pitch + x], &ui_bg[j * FOS_W + x], w);
}

/* static parts of a screen are drawn once into its background */
void ui_to_bg(int on)
{
    static struct fos_canvas bgcv, *saved;
    if (on && cv != &bgcv)
    {
        saved = cv;
        fos_canvas_init(&bgcv, ui_bg, FOS_W, FOS_W, FOS_H);
        cv = &bgcv;
    }
    else if (!on && cv == &bgcv)
        cv = saved;
}

/* a dark backdrop with a little light from above */
static int plain_lv(int x, int y)
{
    int dx = (x - 360) * 2 / 3, dy = y - 40;
    int d2 = (dx * dx + dy * dy) >> 6;                  /* soft pool of light */
    return LV12(9) + (d2 < 2400 ? (2400 - d2) * LV12(9) / 2400 : 0);
}

static uint8_t *buf_plain;          /* the backdrop, worked out once a session */

void ui_bg_plain()
{
    if (buf_plain)
    {
        memcpy(ui_bg, buf_plain, FOS_W * FOS_H);
        return;
    }
    /* the light changes slowly: worked out on an 8 px grid, stepped between */
    const int mul = fos_ramp_mul[RAMP_GRAY];
    const int base = fos_ramp_base[RAMP_GRAY], last = fos_ramp_last[RAMP_GRAY];
    static int grid[2][FOS_W / 8 + 1];
    for (int i = 0; i <= FOS_W / 8; i++)
        grid[1][i] = plain_lv(i * 8, 0) * mul;
    for (int y = 0; y < FOS_H; y++)
    {
        if (!(y & 7))
            for (int i = 0; i <= FOS_W / 8; i++)
            {
                grid[0][i] = grid[1][i];
                grid[1][i] = plain_lv(i * 8, y + 8) * mul;
            }
        int fy = y & 7;
        uint8_t *row = &ui_bg[y * FOS_W];
        const uint8_t *bay = fos_bayer[y & 3];
        for (int x = 0, i = 0; x < FOS_W; x += 8, i++)
        {
            int p0 = grid[0][i] + ((grid[1][i] - grid[0][i]) >> 3) * fy;
            int p1 = grid[0][i + 1] + ((grid[1][i + 1] - grid[0][i + 1]) >> 3) * fy;
            int p = p0, dp = (p1 - p0) >> 3;
            for (int k = 0; k < 8; k++, p += dp)
            {
                int q = ((p >> 16) + bay[k & 3]) >> 4;
                row[x + k] = base + (q > last ? last : q);
            }
        }
    }
    /* keep it for the next page, if there is room */
    buf_plain = fos_port_alloc(FOS_W * FOS_H);
    if (buf_plain) memcpy(buf_plain, ui_bg, FOS_W * FOS_H);
}

void ui_status_bar(const char *title)
{
    /* wordmark: a small aperture and the name */
    int y = 24;
    for (int i = 0; i < 6; i++)
    {
        int a = i * 65536 / 6 + 3000;
        int x0 = 26 * 256 + fos_cos(a) * 7 / 16, y0 = 18 * 256 + fos_sin(a) * 7 / 16;
        int x1 = 26 * 256 + fos_cos(a + 65536 / 6) * 7 / 16, y1 = 18 * 256 + fos_sin(a + 65536 / 6) * 7 / 16;
        fos_line(cv, x0, y0, x1, y1, 400, C_ACCENT, OPAQUE);
    }
    int x = fos_text(cv, &fos_font_xs, 42, y, FOS_NAME, C_TEXT2, OPAQUE, 40) + 42;
    if (title && *title)
    {
        fos_fill(cv, x + 12, 12, 1, 14, C_LINE);
        fos_text(cv, &fos_font_xs, x + 25, y, title, C_TEXT3, OPAQUE, 40);
    }

    /* right: the roll in the camera and the time */
    char clock[16], buf[48];
    fos_port_clock(clock, sizeof(clock));
    int rx = FOS_W - 24;
    rx -= fos_text_r(cv, &fos_font_mdb, rx, y + 1, clock, C_TEXT, OPAQUE, 0) + 18;
    if (ui_roll.loaded)
    {
        /* no rolls: the film and the count, never an end (the count is over the hidden rolls) */
        int left = ui_roll.no_rolls ? 1 : ui_roll.exposures - ui_roll.shot;
        if (ui_roll.no_rolls)
            snprintf(buf, sizeof(buf), "%s  %d", fos_films[ui_roll.film].name, ui_roll.total);
        else if (left > 0)
            snprintf(buf, sizeof(buf), "%s  %d/%d", fos_films[ui_roll.film].name, ui_roll.shot, ui_roll.exposures);
        else
            snprintf(buf, sizeof(buf), "%s  END OF ROLL", fos_films[ui_roll.film].name);
        int w = fos_text_w(&fos_font_xs, buf, 16);
        fos_rrect(cv, rx - w - 24, 9, w + 24, 20, 10, C_PANEL, OPAQUE);
        fos_disc(cv, (rx - w - 14) * 256, 19 * 256, 3 * 256, left > 0 ? C_ACCENT : C_RED, OPAQUE);
        fos_text(cv, &fos_font_xs, rx - w - 6, y - 1, buf, C_TEXT2, OPAQUE, 16);
    }
}

int ui_keycap(int x, int base, const char *key)
{
    /* arrows are drawn ("<>", "^v", or "<" alone), words are written */
    int w;
    if (key[0] == '<' || key[0] == '^')
    {
        w = key[1] ? 34 : 22;
        fos_rrect_line(cv, x, base - 15, w, 20, 6, 1, FC(RAMP_GRAY, 90), OPAQUE);
        int vert = key[0] == '^';
        fos_tri(cv, x + 11, base - 5, 7, vert ? 3 : 2, C_TEXT2, OPAQUE);
        if (key[1]) fos_tri(cv, x + 23, base - 5, 7, vert ? 1 : 0, C_TEXT2, OPAQUE);
        return w;
    }
    w = fos_text_w(&fos_font_xs, key, 16) + 14;
    fos_rrect_line(cv, x, base - 15, w, 20, 6, 1, FC(RAMP_GRAY, 90), OPAQUE);
    fos_text(cv, &fos_font_xs, x + 7, base - 1, key, C_TEXT2, OPAQUE, 16);
    return w;
}

static int keycap_w(const char *key)
{
    return key[0] == '<' || key[0] == '^' ? (key[1] ? 34 : 22) : fos_text_w(&fos_font_xs, key, 16) + 14;
}

void ui_hints(const struct hint *h, int n)
{
    int base = HINTS_Y + 23;
    /* the gap between hints closes up so the row fits the screen: with a
     * roll in progress the shelf's six ran off the right edge (#43) */
    int need = 0;
    for (int i = 0; i < n; i++)
        need += keycap_w(h[i].key) + 8 + fos_text_w(&fos_font_sm, h[i].label, 0);
    int gap = 22, room = FOS_W - 48 - need;
    if (n > 1 && room < gap * (n - 1)) gap = room / (n - 1);
    if (gap < 8) gap = 8;
    int x = 24;
    for (int i = 0; i < n; i++)
    {
        x += ui_keycap(x, base, h[i].key) + 8;
        x += fos_text(cv, &fos_font_sm, x, base - 1, h[i].label, C_TEXT3, OPAQUE, 0) + gap;
    }
}

/* overlay text: coverage onto the panel greys, or the accent */
int ui_photo_text(const struct fos_font *f, int x, int y, const char *s, int accent, int align)
{
    static const uint8_t grey[16] = { 0, PX_G1, PX_G1, PX_G2, PX_G2, PX_G3, PX_G3, PX_G3, PX_G4, PX_G4,
                                      PX_G5, PX_G5, PX_G5, PX_WHITE, PX_WHITE, PX_WHITE };
    static const uint8_t amber[16] = { 0, PX_G1, PX_G1, PX_G2, PX_AMBER2, PX_AMBER2, PX_AMBER2, PX_AMBER2,
                                       PX_AMBER, PX_AMBER, PX_AMBER, PX_AMBER, PX_AMBER, PX_AMBER, PX_AMBER, PX_AMBER };
    int w = fos_text_w(f, s, accent ? 40 : 0);
    if (align == 1) x -= w / 2;
    else if (align == 2) x -= w;
    fos_text_map(cv->px, cv->pitch, FOS_W, FOS_H, f, x, y, s, accent ? amber : grey, accent ? 40 : 0);
    return w;
}

void ui_dots(int x, int y, int n, int filled, fcol on)
{
    for (int i = 0; i < n; i++)
    {
        if (i < filled)
            fos_rrect(cv, x + i * 13, y, 10, 4, 2, on, OPAQUE);
        else
            fos_rrect(cv, x + i * 13, y, 10, 4, 2, FC(RAMP_GRAY, 46), OPAQUE);
    }
}

void ui_toast(const char *msg)
{
    snprintf(toast_msg, sizeof(toast_msg), "%s", msg);
    toast_t = 0;
}

int ui_toast_up()
{
    return toast_t < TOAST_LEN;
}

static void toast_draw()
{
    if (toast_t >= TOAST_LEN) return;
    float in = clampf(toast_t / 180.0f, 0, 1), out = clampf((TOAST_LEN - toast_t) / 300.0f, 0, 1);
    float k = ease_out(in) * out;
    int a = (int) (k * 256);
    int w = fos_text_w(&fos_font_sm, toast_msg, 0) + 36;
    int y = HINTS_Y - 38 + (int) ((1 - ease_out(in)) * 10);
    fos_rrect(cv, 360 - w / 2, y, w, 32, 16, FC(RAMP_GRAY, 34), a);
    fos_rrect_line(cv, 360 - w / 2, y, w, 32, 16, 1, FC(RAMP_GRAY, 58), a);
    fos_text_c(cv, &fos_font_sm, 360, y + 21, toast_msg, C_TEXT, a, 0);
}

void ui_sheet(float s, const char *title, const char *body, const char *yes, const char *no)
{
    ui_sheet3(s, title, body, yes, 0, no);
}

void ui_sheet3(float s, const char *title, const char *body, const char *yes, const char *alt, const char *no)
{
    if (s <= 0.002f) return;
    int sh = 150;
    int y = FOS_H - (int) (sh * ease_out(s)) + 6;
    fos_scale_levels(cv, 0, 0, FOS_W, y + 18, (int) (256 - s * 130));      /* what shows above it */
    fos_rrect(cv, 12, y, FOS_W - 24, sh + 20, 18, FC(RAMP_GRAY, 24), OPAQUE);
    fos_rrect_line(cv, 12, y, FOS_W - 24, sh + 20, 18, 1, FC(RAMP_GRAY, 48), OPAQUE);
    fos_text(cv, &fos_font_lg, 40, y + 44, title, C_TEXT, OPAQUE, 0);
    fos_text(cv, &fos_font_sm, 40, y + 74, body, C_TEXT2, OPAQUE, 0);
    int base = y + 118;
    int x = 40;
    x += ui_keycap(x, base, "SET") + 8;
    x += fos_text(cv, &fos_font_md, x, base, yes, C_ACCENT, OPAQUE, 0) + 28;
    if (alt)
    {
        x += ui_keycap(x, base, "INFO") + 8;
        x += fos_text(cv, &fos_font_md, x, base, alt, C_TEXT, OPAQUE, 0) + 28;
    }
    x += ui_keycap(x, base, "MENU") + 8;
    fos_text(cv, &fos_font_md, x, base, no, C_TEXT2, OPAQUE, 0);
}

/* small line icons, about 26 px */
void ui_icon(int kind, int cx, int cy, fcol c, int a)
{
    int X = cx * 256, Y = cy * 256;
    int w = 2 * 256;
    switch (kind)
    {
        case ICON_FILM:         /* a canister from the side, leader out */
            fos_rrect_line(cv, cx - 8, cy - 10, 13, 21, 3, 2, c, a);
            fos_fill(cv, cx - 5, cy - 13, 7, 3, c);
            fos_rrect(cv, cx + 5, cy - 4, 8, 9, 1, c, a);
            break;
        case ICON_ROLL:         /* a strip of two frames */
            fos_rrect_line(cv, cx - 12, cy - 9, 24, 18, 2, 2, c, a);
            for (int i = 0; i < 4; i++)
            {
                fos_fill(cv, cx - 9 + i * 6, cy - 7, 3, 2, c);
                fos_fill(cv, cx - 9 + i * 6, cy + 5, 3, 2, c);
            }
            fos_fill(cv, cx - 1, cy - 4, 2, 8, c);
            break;
        case ICON_FINDER:       /* the focusing screen */
            fos_rrect_line(cv, cx - 12, cy - 9, 24, 18, 2, 2, c, a);
            fos_ring(cv, X, Y, 5 * 256, w, c, a);
            fos_line(cv, X - 5 * 256, Y, X + 5 * 256, Y, 300, c, a);
            break;
        case ICON_LENS:         /* an aperture */
            for (int i = 0; i < 6; i++)
            {
                int t = i * 65536 / 6 + 4000;
                fos_line(cv, X + fos_cos(t) * 11 / 16, Y + fos_sin(t) * 11 / 16,
                         X + fos_cos(t + 65536 / 6) * 11 / 16, Y + fos_sin(t + 65536 / 6) * 11 / 16, w, c, a);
                fos_line(cv, X + fos_cos(t) * 11 / 16, Y + fos_sin(t) * 11 / 16,
                         X + fos_cos(t + 65536 / 3) * 5 / 16, Y + fos_sin(t + 65536 / 3) * 5 / 16, 350, c, a);
            }
            break;
        case ICON_PRINTS:       /* two prints, the front one with a hill and a sun */
            fos_fill(cv, cx - 13, cy - 11, 2, 14, c);
            fos_fill(cv, cx - 13, cy - 11, 18, 2, c);
            fos_rrect_line(cv, cx - 8, cy - 6, 21, 16, 2, 2, c, a);
            fos_tri(cv, cx + 1, cy + 5, 7, 3, c, a);
            fos_disc(cv, (cx + 8) * 256, (cy - 1) * 256, 2 * 256, c, a);
            break;
        case ICON_SYSTEM:       /* a gear */
            fos_ring(cv, X, Y, 7 * 256, 3 * 256, c, a);
            for (int i = 0; i < 8; i++)
            {
                int t = i * 65536 / 8;
                fos_line(cv, X + fos_cos(t) * 8 / 16, Y + fos_sin(t) * 8 / 16,
                         X + fos_cos(t) * 12 / 16, Y + fos_sin(t) * 12 / 16, 3 * 256, c, a);
            }
            break;
    }
}

/* ---------------------------------------------------------------- the aperture */

#define BLADES 7

/* the blades around an opening of radius r px centred on (cx8, cy8):
 * each blade is the region beyond one edge of the opening, wedged between
 * the extended edges of its neighbours */
static void blades(int cx8, int cy8, int r, int angle, int reach, int shade0)
{
    int vx[BLADES], vy[BLADES], dx[BLADES], dy[BLADES];
    for (int k = 0; k < BLADES; k++)
    {
        int a = angle + k * 65536 / BLADES;
        vx[k] = cx8 + fos_cos(a) * r / 16;
        vy[k] = cy8 + fos_sin(a) * r / 16;
    }
    for (int k = 0; k < BLADES; k++)
    {
        int j = (k + 1) % BLADES;
        int ex = (vx[j] - vx[k]) >> 8, ey = (vy[j] - vy[k]) >> 8;
        int len = fos_isqrt((unsigned) (ex * ex + ey * ey));
        if (!len)
        {
            /* fully closed: edge directions from the angle alone */
            int a = angle + k * 65536 / BLADES + 65536 / 4 + 65536 / (2 * BLADES);
            dx[k] = fos_cos(a); dy[k] = fos_sin(a);
        }
        else { dx[k] = ex * 4096 / len; dy[k] = ey * 4096 / len; }
    }
    for (int k = 0; k < BLADES; k++)
    {
        int p = (k + BLADES - 1) % BLADES;
        int pts[6] = {
            vx[k], vy[k],
            vx[k] + dx[k] * reach / 16, vy[k] + dy[k] * reach / 16,
            vx[k] + dx[p] * reach / 16, vy[k] + dy[p] * reach / 16,
        };
        int shade = shade0 + (k * 37 % 7) * 2;
        fos_convex(cv, pts, 3, FC(RAMP_METAL, shade), OPAQUE);
    }
    /* a lit edge on every blade, and a shadow where blades overlap */
    for (int k = 0; k < BLADES; k++)
    {
        int j = (k + 1) % BLADES;
        int p = (k + BLADES - 1) % BLADES;
        if (r > 2)
            fos_line(cv, vx[k], vy[k], vx[j], vy[j], 384, FC(RAMP_METAL, shade0 + 72), OPAQUE);
        /* the shadow fades into the blades: only near the opening */
        int sl = reach < 2 * r + 60 ? reach : 2 * r + 60;
        fos_line(cv, vx[k], vy[k], vx[k] + dx[p] * sl / 16, vy[k] + dy[p] * sl / 16,
                 300, FC(RAMP_METAL, shade0 / 3), OPAQUE);
    }
}

void ui_iris(int open, int angle)
{
    if (open >= 4096) return;
    /* opening radius: 0 .. a bit past the corners */
    blades(360 * 256, 240 * 256, (int) ((int64_t) 450 * open / 4096), angle, 1400, 20);
}

void ui_aperture(int cx, int cy, int R, int open, int angle)
{
    struct fos_canvas save = *cv;
    fos_clip(cv, cx - R - 2, cy - R - 2, cx + R + 3, cy + R + 3);
    blades(cx * 256 + 128, cy * 256 + 128, R * open / 4096, angle, 2 * R + 24, 24);
    /* outside the barrel the background comes back, with a soft edge */
    int in = (R * 16 - 8) * (R * 16 - 8), out = (R * 16 + 8) * (R * 16 + 8);
    for (int y = cv->cy0; y < cv->cy1; y++)
    {
        int dy = (y - cy) * 16;
        /* |x - cx| <= xi: inside the opening; >= xo: background only */
        int xi = dy * dy < in ? fos_isqrt(in - dy * dy) >> 4 : -1;
        int xo = dy * dy < out ? (fos_isqrt(out - dy * dy) >> 4) + 1 : 0;
        for (int x = cv->cx0; x < cv->cx1; x++)
        {
            int ax = x < cx ? cx - x : x - cx;
            if (ax <= xi) { x = cx + xi; continue; }
            if (ax >= xo) { cv->px[y * cv->pitch + x] = ui_bg[y * FOS_W + x]; continue; }
            int dx = (x - cx) * 16;
            int d2 = dx * dx + dy * dy;
            if (d2 <= in) continue;
            int d = fos_isqrt(d2) - R * 16;                 /* 1/16 px outside */
            int a = fos_clamp(128 + d * 16, 0, 256);
            uint8_t bg = ui_bg[y * FOS_W + x];
            uint8_t *p = &cv->px[y * cv->pitch + x];
            if (a >= 256) *p = bg;
            else
            {
                fcol c = fos_colour_of(bg);
                *p = fos_mix(*p, FC_RAMP(c), LV12(FC_LEVEL(c)), a);
            }
        }
    }
    *cv = save;
    /* the barrel */
    fos_ring(cv, cx * 256 + 128, cy * 256 + 128, R * 256 + 256, 3 * 256, FC(RAMP_METAL, 70), OPAQUE);
    fos_ring(cv, cx * 256 + 128, cy * 256 + 128, R * 256 + 3 * 256, 2 * 256, FC(RAMP_METAL, 28), OPAQUE);
}

/* ---------------------------------------------------------------- boot */

/* power on: a lens aperture opens on the name */
static void boot_draw(int t)
{
    if (t <= 40)
        memset(ui_bg, fos_ix(FC(RAMP_GRAY, 6)), FOS_W * FOS_H);
    fos_fill(cv, 0, 0, FOS_W, FOS_H, FC(RAMP_GRAY, 6));
    float k = clampf((t - 150) / 900.0f, 0, 1);
    float e = ease_out(k);
    int open = (int) (60 + e * 2200);
    ui_aperture(360, 186, 50, open, 2000 + (int) (e * 7000));
    /* a warm glint through the opening */
    fos_disc(cv, 360 * 256, 186 * 256, (int) (open * 50 / 4096) * 200, C_ACCENT, (int) (e * 120));
    float tk = clampf((t - 450) / 700.0f, 0, 1);
    int a = (int) (ease_out(tk) * 256);
    int track = (int) (160 - 96 * ease_out(tk));
    fos_text_c(cv, &fos_font_lg, 360, 290, FOS_NAME, C_TEXT, a, track);
    fos_text_c(cv, &fos_font_xs, 360, 318, "35 MM FILM SYSTEM", C_TEXT3, (int) (ease_out(clampf((t - 750) / 600.0f, 0, 1)) * 256), 48);
    ui_damage_all();
}

/* ---------------------------------------------------------------- navigation */

void ui_go(int to, int arg, int how)
{
    /* keep what is on screen now; the new screen slides over it */
    memcpy(buf_snap, cv->px, FOS_W * FOS_H);
    screen_from = screen;
    if (screen == SCR_SHELF) shelf_leave();
    if (screen == SCR_DEVELOP) develop_leave();
    if (screen == SCR_PRINTS) prints_leave();
    screen = to;
    tr_how = how;
    tr_t = 0;
    tr_len = how == GO_FADE ? 520 : 340;
    ui_clobbered = 1;               /* the new screen starts from nothing */
    switch (to)
    {
        case SCR_SHELF: shelf_enter(arg); break;
        case SCR_HOME:  home_enter(arg);  break;
        case SCR_PAGE:  page_enter(arg);  break;
        case SCR_ROLL:  roll_enter(arg);  break;
        case SCR_DARK:  dark_enter(arg);  break;
        case SCR_DEVELOP: develop_enter(arg); break;
        case SCR_PRINTS: prints_enter(arg); break;
    }
    ui_damage_all();
}

int ui_settled()
{
    return !(tr_len && tr_t < tr_len) && !leaving && enter_t >= ENTER_LEN;
}

int ui_from()
{
    return screen_from;
}

void ui_leave()
{
    if (leaving) return;
    leaving = 1;
    leave_t = 0;
}

/* ---------------------------------------------------------------- the loop */

static void screen_key(int k)
{
    switch (screen)
    {
        case SCR_BOOT:
            if (boot_then_leave) ui_leave();
            else ui_go(SCR_SHELF, 0, GO_FADE);
            break;
        case SCR_SHELF: shelf_key(k); break;
        case SCR_HOME:  home_key(k);  break;
        case SCR_PAGE:  page_key(k);  break;
        case SCR_ROLL:  roll_key(k);  break;
        case SCR_DARK:  dark_key(k);  break;
        case SCR_DEVELOP: develop_key(k); break;
        case SCR_PRINTS: prints_key(k); break;
    }
}

static int screen_update(int dt)
{
    switch (screen)
    {
        case SCR_SHELF: return shelf_update(dt);
        case SCR_HOME:  return home_update(dt);
        case SCR_PAGE:  return page_update(dt);
        case SCR_ROLL:  return roll_update(dt);
        case SCR_DARK:  return dark_update(dt);
        case SCR_DEVELOP: return develop_update(dt);
        case SCR_PRINTS: return prints_update(dt);
    }
    return 0;
}

static void screen_draw()
{
    switch (screen)
    {
        case SCR_SHELF: shelf_draw(); break;
        case SCR_HOME:  home_draw();  break;
        case SCR_PAGE:  page_draw();  break;
        case SCR_ROLL:  roll_draw();  break;
        case SCR_DARK:  dark_draw();  break;
        case SCR_DEVELOP: develop_draw(); break;
        case SCR_PRINTS: prints_draw(); break;
    }
}

/* the screen change: the old frame (snapshot) and the new one, composed a
 * row at a time straight into the presented rows */
static uint8_t row_buf[FOS_W];
static uint8_t dim_lut[256];

/* row_buf[x0..x1): the dimmed layer where it has pixels (u0..u1), else bg */
static void dim_run(uint8_t *row, const uint8_t *urow, int x0, int x1, int u0, int u1, uint8_t bg)
{
    int a = fos_clamp(u0, x0, x1), b = fos_clamp(u1, a, x1);
    memset(row + x0, bg, a - x0);
    int x = a;
    for (; x + 4 <= b; x += 4)
    {
        row[x] = dim_lut[urow[x]];
        row[x + 1] = dim_lut[urow[x + 1]];
        row[x + 2] = dim_lut[urow[x + 2]];
        row[x + 3] = dim_lut[urow[x + 3]];
    }
    for (; x < b; x++)
        row[x] = dim_lut[urow[x]];
    memset(row + b, bg, x1 - b);
}

static void present_transition(float t)
{
    float e = ease_out(t);
    int how = tr_how;
    int shift_new, shift_old, dim;
    if (how == GO_PUSH)      { shift_new = (int) ((1 - e) * FOS_W); shift_old = -(int) (e * FOS_W * 0.3f); dim = (int) (256 - e * 150); }
    else if (how == GO_POP)  { shift_new = -(int) ((1 - e) * FOS_W * 0.3f); shift_old = (int) (e * FOS_W); dim = (int) (106 + e * 150); }
    else                     { shift_new = 0; shift_old = 0; dim = 0; }

    if (how == GO_FADE)
    {
        /* dip: the old screen sinks into the dark, the new one rises from it */
        int k = t < 0.45f ? (int) ((1 - t / 0.45f) * 256) : (int) (ease_out((t - 0.45f) / 0.55f) * 256);
        const uint8_t *src = t < 0.45f ? buf_snap : cv->px;
        for (int i = 0; i < 256; i++)
        {
            fcol c = fos_colour_of(i);
            dim_lut[i] = fos_ix(FC(FC_RAMP(c), FC_LEVEL(c) * k / 256));
        }
        for (int y = 0; y < FOS_H; y++)
        {
            for (int x = 0; x < FOS_W; x++)
                row_buf[x] = dim_lut[src[y * FOS_W + x]];
            fos_port_present(row_buf, FOS_W, y, y + 1);
        }
        return;
    }

    for (int i = 0; i < 256; i++)
    {
        fcol c = fos_colour_of(i);
        dim_lut[i] = fos_ix(FC(FC_RAMP(c), FC_LEVEL(c) * dim / 256));
    }
    const uint8_t *top = how == GO_PUSH ? cv->px : buf_snap;      /* the sliding layer */
    const uint8_t *under = how == GO_PUSH ? buf_snap : cv->px;
    int shift_top = how == GO_PUSH ? shift_new : shift_old;
    int shift_under = how == GO_PUSH ? shift_old : shift_new;
    uint8_t bg = fos_ix(C_BG);
    /* the sliding layer covers [t0, t1); the dimmed one shows elsewhere
     * where it has pixels, [u0, u1) */
    int t0 = fos_clamp(shift_top, 0, FOS_W), t1 = fos_clamp(shift_top + FOS_W, 0, FOS_W);
    int u0 = fos_clamp(shift_under, 0, FOS_W), u1 = fos_clamp(shift_under + FOS_W, 0, FOS_W);
    for (int y = 0; y < FOS_H; y++)
    {
        const uint8_t *trow = top + y * FOS_W, *urow = under + y * FOS_W - shift_under;
        dim_run(row_buf, urow, 0, t0, u0, u1, bg);
        dim_run(row_buf, urow, t1, FOS_W, u0, u1, bg);
        if (t1 > t0)
            memcpy(row_buf + t0, trow + (t0 - shift_top), t1 - t0);
        /* a soft shadow on the edge of the sliding layer */
        for (int s = 1; s <= 10; s++)
        {
            int x = shift_top - s;
            if (x >= 0 && x < FOS_W)
            {
                fcol c = fos_colour_of(row_buf[x]);
                row_buf[x] = fos_ix(FC(FC_RAMP(c), FC_LEVEL(c) * (156 + s * 10) / 256));
            }
        }
        fos_port_present(row_buf, FOS_W, y, y + 1);
    }
}

static int ui_alloc()
{
    buf_cv = fos_port_alloc(FOS_W * FOS_H);
    buf_bg = fos_port_alloc(FOS_W * FOS_H);
    buf_snap = fos_port_alloc(FOS_W * FOS_H);
    if (!buf_cv || !buf_bg || !buf_snap)
    {
        fos_port_free(buf_cv); fos_port_free(buf_bg); fos_port_free(buf_snap);
        buf_cv = buf_bg = buf_snap = 0;
        return 0;
    }
    fos_canvas_init(&canvas, buf_cv, FOS_W, FOS_W, FOS_H);
    cv = &canvas;
    ui_bg = buf_bg;
    return 1;
}

static void ui_free()
{
    ui_cans_free();
    fos_port_free(buf_plain);
    buf_plain = 0;
    fos_port_free(buf_cv); fos_port_free(buf_bg); fos_port_free(buf_snap);
    buf_cv = buf_bg = buf_snap = 0;
}

void fos_ui_init()
{
    fos_gfx_init();
}

/* how long frames take: drawing, copying to the screen, and the rest
 * between them. A slow camera gets simpler canisters rather than a
 * stuttering shelf. */
static struct fos_stat stat;
static int stat_since, frame_end;

static void frame_stats(int draw_ms, int present_ms)
{
    static int slow, quick;
    stat.frames++;
    stat.cans_ms += ui_cans_ms;
    ui_cans_ms = 0;
    stat.draw_ms += draw_ms;
    stat.present_ms += present_ms;
    if (draw_ms + present_ms > stat.worst_ms) stat.worst_ms = draw_ms + present_ms;
    frame_end = fos_port_ms();
    if (ui_now - stat_since < 1000) return;
    fos_port_stat(&stat);
    int avg = (stat.draw_ms + stat.present_ms) / stat.frames;
    if (avg > 50) { slow++; quick = 0; }
    else if (avg < 26) { quick++; slow = 0; }
    else slow = quick = 0;
    if (slow >= 1 && fos_can_quality > 0) { fos_can_quality--; slow = 0; }
    if (quick >= 3 && fos_can_quality < 2) { fos_can_quality++; quick = 0; }
    memset(&stat, 0, sizeof(stat));
    stat_since = ui_now;
}

/* while animating: rest what is left of a frame (about 35 a second), but
 * always a little, so the camera's own tasks get to run */
#define FRAME_PERIOD 28
static int frame_rest(int frame_start)
{
    return fos_clamp(FRAME_PERIOD - (fos_port_ms() - frame_start), 1, 10);
}

static int is_repeating(int k)
{
    return k == FK_LEFT || k == FK_RIGHT || k == FK_UP || k == FK_DOWN;
}

void fos_ui_run(int open)
{
    int t_open = fos_port_ms();
    if (!ui_alloc())
        return;
    fos_port_roll(&ui_roll);
    quit = 0;
    leaving = 0;
    held_key = 0;
    toast_t = TOAST_LEN;
    screen = SCR_NONE;
    screen_from = SCR_NONE;
    enter_t = 0;

    /* what is under the shutter while it closes: LiveView */
    memset(buf_cv, IX_CLEAR, FOS_W * FOS_H);
    ui_clobbered = 1;
    cv_on_screen = 0;
    fos_port_palette();

    boot_then_leave = open == FOS_OPEN_BOOT_LV;
    if (open == FOS_OPEN_BOOT || open == FOS_OPEN_BOOT_LV)
        screen = SCR_BOOT, enter_t = ENTER_LEN;
    else
    {
        switch (open)
        {
            case FOS_OPEN_HOME:     screen = SCR_HOME; home_enter(0); break;
            case FOS_OPEN_PRINTS:   screen = SCR_PRINTS; prints_enter(PRINTS_ALONE); break;
            case FOS_OPEN_END:      screen = SCR_ROLL; roll_enter(1); break;
            case FOS_OPEN_DARKROOM: screen = SCR_DARK; dark_enter(0); break;
            default:                screen = SCR_SHELF; shelf_enter(0); break;
        }
    }
    tr_how = 0; tr_t = tr_len = 0;

    int last = fos_port_ms();
    int boot_t = 0;
    int opened = 0;
    memset(&stat, 0, sizeof(stat));
    stat_since = frame_end = 0;
    while (!quit)
    {
        int now = fos_port_ms();
        int dt = fos_clamp(now - last, 0, 60);
        last = now;
        /* the rest after the last animated frame, slept or taken by others */
        if (frame_end) { stat.rest_ms += now - frame_end; frame_end = 0; }
        ui_now = now;

        if (fos_port_abort())
            break;

        /* keys, with auto-repeat for held arrows */
        int k;
        while ((k = fos_port_key()) != FK_NONE)
        {
            if (k & FK_RELEASE)
            {
                if ((k & 0xFF) == held_key) held_key = 0;
                continue;
            }
            if (is_repeating(k)) { held_key = k; held_since = now; held_last = now; }
            if (leaving || enter_t < ENTER_LEN) continue;
            screen_key(k);
        }
        /* a release that never came must not scroll forever */
        if (held_key && now - held_since > 3000)
            held_key = 0;
        if (held_key && now - held_since > 420 && now - held_last > 95 && !leaving)
        {
            held_last = now;
            screen_key(held_key);
        }

        int moving = 0;
        if (screen == SCR_BOOT)
        {
            boot_t += dt;
            boot_draw(boot_t);
            moving = 1;
            if (boot_t > 1700 && boot_then_leave)
            {
                ui_leave();
                boot_t = -100000;           /* once */
            }
            else if (boot_t > 1700)
                ui_go(SCR_SHELF, 0, GO_FADE);
        }
        else
        {
            moving |= screen_update(dt);
            if (tr_len && tr_t < tr_len) { tr_t += dt; moving = 1; }
        }

        int in_tr = tr_len && tr_t < tr_len;
        int t_draw = fos_port_ms();
        if (enter_t < ENTER_LEN || leaving)
        {
            /* the shutter: closes over LiveView, opens on us (and the
             * reverse when leaving) */
            int t;
            if (leaving) { leave_t += dt; t = leave_t; }
            else         { enter_t += dt; t = enter_t; }
            if (screen != SCR_BOOT) screen_draw();
            int open_k;
            int ang = (int) (t * 9);
            if (leaving)
            {
                float f = clampf((float) t / LEAVE_LEN, 0, 1);
                if (f < 0.35f) open_k = (int) ((1 - ease_out(f / 0.35f)) * 4096);
                else
                {
                    memset(buf_cv, IX_CLEAR, FOS_W * FOS_H);
                    open_k = (int) (ease_out((f - 0.35f) / 0.65f) * 4096);
                }
                if (t >= LEAVE_LEN) quit = 1;
            }
            else
            {
                float f = clampf((float) t / ENTER_LEN, 0, 1);
                if (f < 0.4f)
                {
                    memset(buf_cv, IX_CLEAR, FOS_W * FOS_H);
                    open_k = (int) ((1 - ease_out(f / 0.4f)) * 4096);
                }
                else open_k = (int) (ease_out((f - 0.4f) / 0.6f) * 4096);
            }
            ui_iris(open_k, ang);
            ui_clobbered = 1;
            fos_port_present(cv->px, FOS_W, 0, FOS_H);
            cv_on_screen = 1;
            if (!opened) { opened = 1; stat.open_ms = fos_port_ms() - t_open; }
            dmg_y0 = FOS_H; dmg_y1 = 0;
            fos_port_sleep(frame_rest(now));
            continue;
        }

        int t_present;
        if (in_tr)
        {
            screen_draw();
            t_present = fos_port_ms();
            present_transition((float) tr_t / tr_len);
            ui_clobbered = 1;       /* the screen only reached the panel composed */
            cv_on_screen = 0;
            dmg_y0 = FOS_H; dmg_y1 = 0;
        }
        else
        {
            if (toast_t < TOAST_LEN)
            {
                toast_t += dt;
                moving = 1;
                ui_damage(HINTS_Y - 50, HINTS_Y);
                ui_clobbered = 1;       /* the screen must paint under it again */
            }
            if (screen != SCR_BOOT && (dmg_y1 > dmg_y0 || moving))
            {
                screen_draw();
                toast_draw();
            }
            t_present = fos_port_ms();
            if (dmg_y1 > dmg_y0)
            {
                fos_port_present(cv->px + dmg_y0 * FOS_W, FOS_W, dmg_y0, dmg_y1);
                if (dmg_y0 == 0 && dmg_y1 == FOS_H) cv_on_screen = 1;
                if (!opened) { opened = 1; stat.open_ms = fos_port_ms() - t_open; }
            }
            dmg_y0 = FOS_H; dmg_y1 = 0;
        }
        if (moving || in_tr)
            frame_stats(t_present - t_draw, fos_port_ms() - t_present);
        fos_port_sleep(moving || in_tr ? frame_rest(now) : 40);
    }
    if (stat.frames || stat.open_ms) fos_port_stat(&stat);     /* the last part second */
    if (screen == SCR_SHELF) shelf_leave();
    if (screen == SCR_DEVELOP) develop_leave();
    if (screen == SCR_PRINTS) prints_leave();
    dark_free();
    ui_free();
}
