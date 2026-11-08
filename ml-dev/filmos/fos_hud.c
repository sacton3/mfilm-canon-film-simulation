/* fos_hud: the SLR viewfinder over LiveView - an aperture scale with the
 * meter needle on the right, a strip of red seven-segment LEDs below.
 *
 * Unlike the film OS screens this draws with Canon's palette (LiveView
 * shows through index 0, and Canon keeps drawing its own bits), so it
 * takes the indices it may use from the caller. The camera puts it
 * together off-screen a band of rows at a time (fos_hud_draw_rows,
 * fos_hud_plate_rows) and copies to the screen only the rows that changed:
 * drawn straight onto the screen, each redraw showed for a moment what it
 * starts with (#44, #64).
 */

#include "fos.h"

#define W        720
#define H        480
#define STRIP_W  64                     /* meter strip on the right */
#define BAR_H    44                     /* LED readout at the bottom */
#define VIEW_W   (W - STRIP_W)
#define VIEW_H   (H - BAR_H)
#define THIRD_PX 14                     /* scale spacing per 1/3 stop */
#define SCALE_TOP 26                    /* y of f/22; f/1 sits 27 thirds below */

/* the focusing screen's box in the middle of the view */
#define BOX_N    129
#define BOX_X    (VIEW_W / 2 - 64)
#define BOX_Y    (VIEW_H / 2 - 64)

static uint8_t *vr;
static int vp;
static const struct fos_hud_palette *pal;
/* the band being drawn: screen rows org_y .. org_y + band_h, vr is org_y */
static int org_y = 0, band_h = H;

static void fill(int x, int y, int w, int h, uint8_t c)
{
    y -= org_y;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > band_h) h = band_h - y;
    for (int j = 0; j < h; j++)
        memset(vr + (y + j) * vp + x, c, w > 0 ? w : 0);
}

static void band(uint8_t *buf, int pitch, int y0, int n, const struct fos_hud_palette *p)
{
    vr = buf; vp = pitch; pal = p;
    org_y = y0; band_h = n;
}

static int fstop_y(int index)
{
    return SCALE_TOP + (fos_nfstops - 1 - index) * THIRD_PX;
}

/* text through a coverage map: grey on black, or red LEDs (two levels) */
static int text_gray(const struct fos_font *f, int x, int y, const char *s, int pct, int track)
{
    uint8_t map[16];
    map[0] = 0;
    for (int i = 1; i < 16; i++)
        map[i] = i < 3 ? 0 : pal->gray[pct * i / 15];
    return fos_text_map(vr, vp, W, band_h, f, x, y - org_y, s, map, track);
}

static int text_led(const struct fos_font *f, int x, int y, const char *s, int on, int track)
{
    uint8_t map[16];
    for (int i = 0; i < 16; i++)
        map[i] = i < 5 ? 0 : !on ? pal->gray[i < 10 ? 6 : 9] : i < 10 ? pal->dark_red : pal->red;
    return fos_text_map(vr, vp, W, band_h, f, x, y - org_y, s, map, track);
}

/* ---------------------------------------------------------------- seven segments */

/* segments a..g as bits 0..6 */
static int seg_bits(int ch)
{
    static const uint8_t digits[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    if (ch >= '0' && ch <= '9') return digits[ch - '0'];
    switch (ch)
    {
        case '-': return 0x40;
        case 'E': return 0x79;
        case 'n': return 0x54;
        case 'd': return 0x5E;
        case 'F': return 0x71;
        case 'b': return 0x7C;
        case 'u': return 0x1C;
        case 'L': return 0x38;
        case 'S': return 0x6D;
    }
    return 0;
}

/* the digits' size: the HUD's, or the top plate's bigger ones (seg_size) */
static int DIG_W = 15, DIG_H = 26, SEG_T = 3, DIG_GAP = 6;

static void seg_size(int w, int h, int t, int gap)
{
    DIG_W = w; DIG_H = h; SEG_T = t; DIG_GAP = gap;
}

/* one slanted digit; off segments are drawn faintly, like the unlit LEDs
 * you can just see behind a real viewfinder mask */
static void seg_digit(int x, int y, int bits, uint8_t on, uint8_t off)
{
    if (y - org_y >= band_h || y + DIG_H - org_y <= 0) return;     /* not in this band */
    int half = DIG_H / 2;
    for (int r = 0; r < DIG_H; r++)
    {
        int slant = (DIG_H - 1 - r) * (DIG_H * 3 / 26) / DIG_H;    /* leans right like the real ones */
        int yy = y + r;
        /* horizontals: a at the top, g in the middle, d at the bottom */
        int hseg = -1;
        if (r < SEG_T) hseg = 0;
        else if (r >= half - 1 && r < half - 1 + SEG_T) hseg = 6;
        else if (r >= DIG_H - SEG_T) hseg = 3;
        if (hseg >= 0)
        {
            /* chamfered ends: shorter where the segment meets the verticals */
            int inset = (hseg == 6 ? 2 : (r == 0 || r == DIG_H - 1) ? 2 : 1) * SEG_T / 3;
            fill(x + slant + inset + 1, yy, DIG_W - 2 * inset - 2, 1, (bits >> hseg) & 1 ? on : off);
        }
        /* verticals: f and b above the middle, e and c below */
        int upper = r < half;
        if (r > 1 && r < DIG_H - 2 && r != half - 1 + SEG_T / 2)
        {
            int left = upper ? 5 : 4, right = upper ? 1 : 2;
            fill(x + slant, yy, SEG_T - (r == 2 || r == DIG_H - 3 ? 1 : 0), 1, (bits >> left) & 1 ? on : off);
            fill(x + slant + DIG_W - SEG_T, yy, SEG_T, 1, (bits >> right) & 1 ? on : off);
        }
    }
}

/* a string of digits; '.' is a dot after the previous digit, '"' the
 * seconds mark; returns the width */
static int seg_text(int x, int y, const char *s, int lit)
{
    uint8_t on = lit ? pal->red : pal->gray[9], off = pal->gray[4];
    int x0 = x;
    for (; *s; s++)
    {
        if (*s == '.')
        {
            fill(x - DIG_GAP + 1, y + DIG_H - SEG_T, SEG_T, SEG_T, on);
            x += 2;
            continue;
        }
        if (*s == '"')
        {
            fill(x - 2, y, SEG_T * 2 / 3, DIG_H / 4, on);
            fill(x + SEG_T * 2 / 3, y, SEG_T * 2 / 3, DIG_H / 4, on);
            x += DIG_H / 3;
            continue;
        }
        seg_digit(x, y, seg_bits(*s), on, off);
        x += DIG_W + DIG_GAP;
    }
    return x - x0;
}

/* the meter's pointer: a red wedge pointing at the scale */
static void needle(int y, uint8_t c)
{
    int x0 = W - STRIP_W + 4;
    for (int r = -7; r <= 7; r++)
        fill(x0, y + r, 16 - 2 * (r < 0 ? -r : r), 1, c);
}

/* ---------------------------------------------------------------- the HUD */

void fos_hud_draw(uint8_t *vram, int pitch, const struct fos_hud *h, const struct fos_hud_palette *p)
{
    fos_hud_draw_rows(vram, pitch, 0, H, h, p);
}

#ifndef CONFIG_EOSM
/* the parts of screen row y the HUD draws on, as x0, x1 pairs: the meter
 * strip, the focusing screen's box and the LED bar (what fos_hud_clear
 * wipes). Returns how many numbers it put in x: 2 or 4. (The EOS M draws
 * its HUD straight onto the screen: filmroll.c) */
int fos_hud_spans(int y, int x[4])
{
    int n = 0;
    if (y >= VIEW_H) { x[0] = 0; x[1] = W; return 2; }
    if (y >= BOX_Y && y < BOX_Y + BOX_N) { x[n++] = BOX_X; x[n++] = BOX_X + BOX_N; }
    x[n++] = W - STRIP_W; x[n++] = W;
    return n;
}
#endif

void fos_hud_draw_rows(uint8_t *buf, int pitch, int y0, int rows, const struct fos_hud *h, const struct fos_hud_palette *p)
{
    band(buf, pitch, y0, rows, p);
    uint8_t bg = p->black;
    char label[8];

    /* the meter strip */
    int sx = W - STRIP_W;
    fill(sx, 0, STRIP_W, VIEW_H, bg);
    fill(sx, 0, 1, VIEW_H, p->gray[18]);
    /* red zones: more light than f/22 can take, less than the lens can open to */
    fill(sx + 27, 4, 4, SCALE_TOP - 11, p->dark_red);
    fill(sx + 27, fstop_y(h->widest) + 9, 4, 20, p->dark_red);
    for (int i = 1; i < fos_nfstops; i++)
    {
        int y = fstop_y(i);
        int usable = i >= h->widest;
        if ((i - 1) % 3)
        {
            fill(sx + 27, y, 4, 1, p->gray[usable ? 30 : 14]);      /* thirds */
            continue;
        }
        fos_format_fstop(label, sizeof(label), i);
        fill(sx + 24, y, 8, 1, p->gray[usable ? 55 : 22]);
        text_gray(&fos_font_xs, sx + 36, y + 5, label, i == h->fstop ? 100 : usable ? 62 : 26, 0);
    }
    /* the aperture you set: white tick */
    fill(sx + 20, fstop_y(h->fstop) - 1, 14, 3, p->white);
    /* the needle points at the aperture the meter asks for, and blinks
     * at either end when that is out of reach */
    if (h->lit && h->meter)
    {
        int y;
        int out = h->want > fos_nfstops - 1 || h->want < h->widest;
        if (h->want > fos_nfstops - 1) y = SCALE_TOP - 12;
        else if (h->want < h->widest) y = fstop_y(h->widest) + 18;
        else y = fstop_y(h->want);
        if (!out || h->blink)
            needle(y, p->red);
    }

    /* the LED strip */
    int by = VIEW_H;
    fill(0, by, W, BAR_H, bg);
    fill(0, by, W, 1, p->gray[18]);
    int ty = by + (BAR_H - DIG_H) / 2 + 1;
    int base = ty + DIG_H;

    /* wind reminder: blinks whether or not the meter is on */
    text_led(&fos_font_mdb, 20, base - 3, "WIND", h->need_wind && h->blink, 24);

    /* shutter speed: 1/125 shows as 125, slow speeds keep their seconds mark */
    seg_text(132, ty, h->shutter, h->lit);

    /* aperture */
    text_gray(&fos_font_xs, 262, base - 4, "f", h->lit ? 70 : 30, 0);
    fos_format_fstop(label, sizeof(label), h->fstop);
    seg_text(274, ty, label, h->lit);

    /* meter on */
    for (int r = -6; r <= 6; r++)
    {
        int w2 = fos_isqrt(36 - r * r);
        fill(392 - w2, by + BAR_H / 2 + r, 2 * w2 + 1, 1, h->lit ? p->red : p->gray[9]);
    }

    /* the frame counter, END at the end */
    int lit_count = h->lit || h->finished;
    if (h->finished)
        seg_text(452, ty, h->blink ? "End" : "   ", 1);
    else
    {
        char n[8];
        snprintf(n, sizeof(n), "%d", h->frame);
        int w = seg_text(452, ty, n, lit_count);
        if (!h->no_rolls)           /* no rolls: a count, no "of" */
        {
            snprintf(n, sizeof(n), "/%d", h->exposures);
            text_gray(&fos_font_sm, 452 + w + 2, base - 2, n, lit_count ? 55 : 28, 0);
        }
    }

    /* which film, quietly */
    if (h->film)
    {
        int w = fos_text_w(&fos_font_xs, h->film, 32);
        text_gray(&fos_font_xs, W - 22 - w, base - 6, h->film, 40, 32);
    }

    /* focusing screen: microprism collar and split-image circle */
    if (h->focus_screen)
    {
        int cx = VIEW_W / 2, cy = VIEW_H / 2;
        for (int a = 0; a < 360; a += 2)
        {
            int c = fos_cos(a * 65536 / 360), s = fos_sin(a * 65536 / 360);
            int x = cx + c * 62 / 4096, y = cy + s * 62 / 4096;
            fill(x, y, 1, 1, p->gray[70]);
            x = cx + c * 28 / 4096; y = cy + s * 28 / 4096;
            fill(x, y, 1, 1, p->white);
        }
        fill(cx - 28, cy, 57, 1, p->white);
    }
    band(vr, vp, 0, H, p);
}

/* make our strips transparent again so LiveView shows through */
void fos_hud_clear(uint8_t *vram, int pitch, const struct fos_hud_palette *p)
{
    band(vram, pitch, 0, H, p);
    fill(W - STRIP_W, 0, STRIP_W, VIEW_H, p->clear);
    fill(0, VIEW_H, W, BAR_H, p->clear);
    fill(BOX_X, BOX_Y, BOX_N, BOX_N, p->clear);
}

/* ---------------------------------------------------------------- the top plate */

/* not on the EOS M: it has no viewfinder to shoot through, and its boot
 * memory is short (MEMORY.md) */
#ifndef CONFIG_EOSM

/* What a DSLR shows when you shoot through the viewfinder and the screen
 * on the back is Canon's settings display: a film camera's top plate in
 * its place, lit like the HUD's LEDs. Shutter, aperture and the frame
 * counter in big red digits, the meter's needle on a scale of stops, the
 * film and its speed. */
void fos_hud_plate(uint8_t *vram, int pitch, const struct fos_hud *h, const struct fos_hud_palette *p)
{
    fos_hud_plate_rows(vram, pitch, 0, H, h, p);
}

void fos_hud_plate_rows(uint8_t *buf, int pitch, int y0, int rows, const struct fos_hud *h, const struct fos_hud_palette *p)
{
    band(buf, pitch, y0, rows, p);
    char b[24];
    fill(0, 0, W, H, p->black);

    /* the maker's plate: name, and the film loaded */
    text_gray(&fos_font_sm, 28, 40, "M" "\x80" "FILM", 46, 40);
    if (h->film)
    {
        int w = fos_text_w(&fos_font_lg, h->film, 0);
        text_gray(&fos_font_lg, W - 28 - w, 46, h->film, 100, 0);
    }
    if (h->iso)
    {
        snprintf(b, sizeof(b), "ISO %d", h->iso);
        int w = fos_text_w(&fos_font_sm, b, 24);
        text_gray(&fos_font_sm, W - 28 - w, 74, b, 55, 24);
    }
    fill(28, 92, W - 56, 1, p->gray[18]);

    /* three windows: shutter, aperture, frames */
    static const char *const names[3] = { "SHUTTER", "APERTURE", "FRAME" };
    const int col[3] = { 28, 268, 488 };
    for (int i = 0; i < 3; i++)
        text_gray(&fos_font_xs, col[i], 132, names[i], 40, 48);
    seg_size(36, 64, 7, 12);
    int ty = 150;
    seg_text(col[0], ty, h->shutter, h->lit);
    fos_format_fstop(b, sizeof(b), h->fstop);
    int w = seg_text(col[1] + 22, ty, b, h->lit);
    text_gray(&fos_font_md, col[1], ty + 62, "f", h->lit ? 70 : 30, 0);
    (void) w;
    if (h->finished)
        seg_text(col[2], ty, h->blink ? "End" : "   ", 1);
    else
    {
        snprintf(b, sizeof(b), "%d", h->frame);
        int fw = seg_text(col[2], ty, b, 1);
        if (!h->no_rolls)           /* no rolls: a count, no "of" (four digits still fit without it) */
        {
            snprintf(b, sizeof(b), "/%d", h->exposures);
            text_gray(&fos_font_md, col[2] + fw + 4, ty + 62, b, 55, 0);
        }
    }
    seg_size(15, 26, 3, 6);

    /* the meter: -2 .. +2 stops in thirds, the needle where the light is */
    int my = 286, mx0 = 120, mx1 = W - 120, mc = (mx0 + mx1) / 2, step = (mx1 - mx0) / 12;
    fill(mx0, my, mx1 - mx0, 1, p->gray[30]);
    for (int t = -6; t <= 6; t++)
    {
        int x = mc + t * step, big = t % 3 == 0;
        fill(x, my - (big ? 10 : 5), t == 0 ? 3 : 1, big ? 20 : 10, p->gray[t == 0 ? 70 : big ? 45 : 25]);
        if (big && t)
        {
            snprintf(b, sizeof(b), "%+d", t / 3);
            int tw = fos_text_w(&fos_font_xs, b, 0);
            text_gray(&fos_font_xs, x - tw / 2, my + 30, b, 40, 0);
        }
    }
    text_gray(&fos_font_xs, mx0 - 70, my + 5, "UNDER", 30, 24);
    text_gray(&fos_font_xs, mx1 + 16, my + 5, "OVER", 30, 24);
    if (h->lit && h->meter)
    {
        int thirds = h->want - h->fstop;            /* + : brighter than the aperture set wants (over) */
        int out = thirds > 6 || thirds < -6;
        thirds = thirds > 6 ? 6 : thirds < -6 ? -6 : thirds;
        if (!out || h->blink)
        {
            int x = mc + thirds * step;
            for (int r = 0; r < 14; r++)
                fill(x - (14 - r) / 2, my - 26 + r, 14 - r + 1, 1, p->red);
        }
    }

    /* the frames left, like the marks on a film counter (no rolls: there is no end to count to) */
    if (!h->no_rolls)
    {
        int n = h->exposures > 0 ? h->exposures : 36, done = h->finished ? n : h->frame - 1;
        int fx = 28, fw = (W - 56) / n;
        for (int i = 0; i < n; i++)
            fill(fx + i * fw, 364, fw - 3 > 1 ? fw - 3 : 1, 10, i < done ? p->gray[60] : i == done ? p->red : p->gray[14]);
    }
    if (h->need_wind)
        text_led(&fos_font_mdb, 28, 420, "WIND ON", h->blink, 24);

    /* the way back to the film OS */
    const char *hint = "TRASH  Home     PLAY  Prints";
    int hw = fos_text_w(&fos_font_xs, hint, 24);
    text_gray(&fos_font_xs, W - 28 - hw, H - 22, hint, 36, 24);
    band(vr, vp, 0, H, p);
}

/* the plate off: Canon's screen shows again */
void fos_hud_plate_clear(uint8_t *vram, int pitch, const struct fos_hud_palette *p)
{
    band(vram, pitch, 0, H, p);
    fill(0, 0, W, H, p->clear);
}
#endif
