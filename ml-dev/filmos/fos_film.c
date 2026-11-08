/* fos_film: the film stocks, and their 35 mm canisters drawn as small 3D
 * objects - a lithographed steel cylinder with crimped caps, the spool end
 * on top, the felt light trap and the film leader sticking out of it.
 *
 * Each stock's label is painted once into a texture that wraps around the
 * cylinder, a ramp and a level per texel with the anti-aliased edges
 * already blended. Light on a cylinder only scales and lifts levels, so
 * each column of the drawing folds its shading into a gain and an offset
 * per ramp, and a label pixel costs a lookup, a multiply and a dither.
 */

#include "fos.h"
#include "fos_dev.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------- the stocks */

/* Picture style values are starting points for the in-camera JPEG preview
 * only; the real look comes from the lab. Mono stocks leave saturation and
 * tone alone because the Monochrome style uses those for filter / toning. */
static const struct fos_film builtin[] =
{
    { "portra400", "KODAK", "Portra 400", "C-41", "Soft warm skin, gentle highlights",
      400, 400, PS_STANDARD, -1, -1, 1, 2, 5500, 0, 1,     2, 2, 2, 4, "Portra400", FOS_GROUP_COLOUR, 0 },
    { "cinestill800t", "CINESTILL", "800T", "C-41", "Tungsten cine film, night goes blue",
      800, 800, PS_STANDARD, 1, 1, -1, 2, 3200, 0, 0,      4, 3, 3, 1, "CineStill800T", FOS_GROUP_CINE, 0 },
    { "ektar100", "KODAK", "Ektar 100", "C-41", "Punchy colour, ultra fine grain",
      100, 100, PS_LANDSCAPE, 1, 2, 0, 3, 5200, 0, 0,      1, 4, 5, 3, "Ektar100", FOS_GROUP_COLOUR, 0 },
    { "gold200", "KODAK", "Gold 200", "C-41", "Golden hour, all day long",
      200, 200, PS_STANDARD, 1, 1, 2, 2, 5600, 0, 2,       3, 3, 4, 5, "Gold200", FOS_GROUP_COLOUR, 0 },
    { "superia400", "FUJIFILM", "Superia 400", "C-41", "Green shadows, everyday snaps",
      400, 400, PS_STANDARD, 1, 1, 0, 2, 5200, 1, 0,       3, 3, 3, 2, "Superia400", FOS_GROUP_COLOUR, 0 },
    { "velvia50", "FUJIFILM", "Velvia 50", "E-6", "Vivid slide film, hard contrast",
      50, 100, PS_LANDSCAPE, 3, 4, 0, 3, 5200, 0, 0,       1, 5, 5, 3, "Velvia50", FOS_GROUP_SLIDE, 0 },
    { "hp5", "ILFORD", "HP5 Plus", "B&W", "Classic B&W, forgiving latitude",
      400, 400, PS_MONO, 1, 0, 0, 2, 5200, 0, 0,           4, 2, 0, 0, "HP5", FOS_GROUP_BW, 0 },
    { "trix400", "KODAK", "Tri-X 400", "B&W", "Gritty contrast, street classic",
      400, 400, PS_MONO, 3, 0, 0, 2, 5200, 0, 0,           5, 4, 0, 0, "TriX400", FOS_GROUP_BW, 0 },
};
#define NBUILTIN ((int) (sizeof(builtin) / sizeof(builtin[0])))

const struct fos_film *fos_films = builtin;
int fos_nfilms = NBUILTIN;

const char *const fos_group_names[FOS_NGROUPS] = { "COLOUR", "SLIDE", "B&W", "CINE" };

const struct fos_look *fos_more_looks;
int fos_nmore_looks;

/* A film in ML/DATA/FOSFILMS.BIN (written by tools/genfilms.py, which
 * packs the same layout): after a 12-byte header ("FOSM", version, record
 * size, count), one of these each. The look is struct fos_look from its
 * balance on, as the camera lays it out. */
#define LOOK_FROM offsetof(struct fos_look, balance)
#define LOOK_BYTES (offsetof(struct fos_look, mono_mix) + sizeof(((struct fos_look *) 0)->mono_mix) - LOOK_FROM)
struct film_rec
{
    char key[16], brand[16], name[24], process[8], desc[48], shortname[24];
    int32_t box_iso, iso, picstyle, contrast, saturation, color_tone, sharpness, kelvin, wbs_gm, wbs_ba;
    uint8_t grain, punch, colour, warmth;
    int32_t group;
    struct fos_label label;
    uint8_t look[LOOK_BYTES];
};

int fos_films_load(const uint8_t *blob, int size)
{
    if (!blob || size < 12 || memcmp(blob, "FOSM", 4) || blob[4] != 1) return 0;
    int rs = blob[6] | blob[7] << 8, n = (int) (blob[8] | blob[9] << 8);
    if (rs != (int) sizeof(struct film_rec) || size < 12 + n * rs || n <= 0) return 0;
    struct fos_film *all = fos_port_alloc((NBUILTIN + n) * (int) sizeof(*all));
    struct fos_look *looks = fos_port_alloc(n * (int) sizeof(*looks));
    if (!all || !looks) { fos_port_free(all); fos_port_free(looks); return 0; }
    memcpy(all, builtin, sizeof(builtin));
    for (int i = 0; i < n; i++)
    {
        struct film_rec *r = (struct film_rec *) (blob + 12 + i * rs);
        r->key[15] = r->brand[15] = r->name[23] = r->process[7] = r->desc[47] = r->shortname[23] = 0;
        r->label.top[19] = r->label.word[13] = r->label.num_s[7] = r->label.sub[17] = 0;
        struct fos_film *f = &all[NBUILTIN + i];
        *f = (struct fos_film) { r->key, r->brand, r->name, r->process, r->desc, r->box_iso, r->iso, r->picstyle,
                                 r->contrast, r->saturation, r->color_tone, r->sharpness, r->kelvin, r->wbs_gm, r->wbs_ba,
                                 r->grain, r->punch, r->colour, r->warmth, r->shortname,
                                 fos_clamp(r->group, 0, FOS_NGROUPS - 1), &r->label };
        memset(&looks[i], 0, sizeof(looks[i]));
        looks[i].key = r->key;
        memcpy((uint8_t *) &looks[i] + LOOK_FROM, r->look, LOOK_BYTES);
        looks[i].nhues = fos_clamp(looks[i].nhues, 0, 4);
    }
    fos_films = all;
    fos_nfilms = NBUILTIN + n;
    fos_more_looks = looks;
    fos_nmore_looks = n;
    return 1;
}

const int fos_exposure_choices[FOS_NEXP] = { 36, 24, 12 };

/* f-numbers x100 in 1/3 stops; index i sits (i - 1) thirds above f/1
 * (f/0.95 is close enough to f/1 to share the bottom of the scale) */
const int fos_fstops[] =
{
     95,  100,  110,  120,  140,  160,  180,  200,  220,  250,
    280,  320,  350,  400,  450,  500,  560,  630,  710,  800,
    900, 1000, 1100, 1300, 1400, 1600, 1800, 2000, 2200,
};
const int fos_nfstops = sizeof(fos_fstops) / sizeof(fos_fstops[0]);

void fos_format_fstop(char *buf, int size, int index)
{
    int v = fos_fstops[fos_clamp(index, 0, fos_nfstops - 1)];
    if (v < 100)
        snprintf(buf, size, "0.%02d", v);
    else if (v % 100 == 0 || v >= 1000)
        snprintf(buf, size, "%d", v / 100);
    else
        snprintf(buf, size, "%d.%d", v / 100, (v / 10) % 10);
}

/* ---------------------------------------------------------------- geometry */

/* full-size canister, px */
#define R       50      /* body radius */
#define RC      53      /* caps stick out a little */
#define RS      19      /* spool end */
#define HB      150     /* body (label) height */
#define TB      9       /* top crimp band */
#define BB      8       /* bottom crimp band */
#define HS      12      /* spool end above the cap */
#define TILT    56      /* ellipse height / width, 1/256: we look down ~13 degrees */

/* label texture: 360 degrees across, body height down */
#define TW      320
#define TH      HB
#define U_FRONT (TW / 2)            /* texture column facing us at rot 0 */
#define U_LIP   (U_FRONT + TW * 82 / 360)   /* the felt lip, on the right */

int fos_can_quality = 2;

struct fos_can
{
    int film;
    int exposures;
    uint16_t *tex;              /* ramp << 12 | 12-bit level, column by column */
    int ramps;                  /* bit mask of the ramps in it */
    int gloss;                  /* 0..256 */
    int leader_ramp, leader_lv;
};

int fos_can_height(int scale)
{
    int e = RC * TILT / 256;
    return (HS + RS * TILT / 256 + e + TB + HB + BB + e) * scale / 256;
}

int fos_can_width(int scale)
{
    return 2 * RC * scale / 256;
}

/* ---------------------------------------------------------------- shading profiles */

/* sampled across the cylinder, x / radius from -1 to +1 */
#define NPROF 256
static int16_t prof_ang[NPROF];     /* surface angle, 1/65536 turn (+ = right) */
static int16_t prof_light[NPROF];   /* printed label: diffuse, 0..256 */
static int16_t prof_spec[NPROF];    /* printed label: gloss highlight, 0..256 */
static int16_t prof_metal[NPROF];   /* steel caps: 12-bit level */
static int16_t prof_dark[NPROF];    /* black plastic spool: 12-bit level */
static int prof_ready = 0;

static float smooth(float a, float b, float x)
{
    float t = (x - a) / (b - a);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

static float lobe(float th, float centre, float width)
{
    float d = (th - centre) / width;
    return expf(-d * d);
}

static void build_profiles()
{
    const float deg = 3.14159265f / 180;
    for (int i = 0; i < NPROF; i++)
    {
        float xn = (i + 0.5f) / NPROF * 2 - 1;
        float th = asinf(xn);                   /* -90..90 degrees */
        prof_ang[i] = (int16_t) (th / (2 * 3.14159265f) * 65536);

        /* softbox up-left in front, a rim light behind on the right */
        float key = cosf(th + 38 * deg);
        key = (key + 0.35f) / 1.35f;
        key = key < 0 ? 0 : key;
        float rim = smooth(55 * deg, 86 * deg, th) * 0.55f;
        float edge = 0.62f + 0.38f * sqrtf(cosf(th));
        float light = (0.16f + 0.84f * key) * edge + rim;
        prof_light[i] = (int16_t) (light * 256);

        float sheen = lobe(th, -24 * deg, 16 * deg) * 0.22f + lobe(th, -21 * deg, 3.0f * deg) * 0.55f
                    + lobe(th, 74 * deg, 6 * deg) * 0.25f;
        prof_spec[i] = (int16_t) (sheen * 256);

        /* brushed steel: dark body with sharp reflections of the studio */
        float m = 0.24f + 0.30f * key
                + 0.55f * lobe(th, -22 * deg, 9 * deg)
                + 0.30f * lobe(th, -55 * deg, 14 * deg)
                + 0.18f * lobe(th, 30 * deg, 18 * deg)
                + 0.45f * lobe(th, 80 * deg, 7 * deg);
        m *= edge;
        prof_metal[i] = (int16_t) fos_clamp((int) (m * LV_MAX), 0, LV_MAX);

        float p = 0.06f + 0.10f * key + 0.22f * lobe(th, -25 * deg, 8 * deg) + 0.12f * lobe(th, 78 * deg, 8 * deg);
        prof_dark[i] = (int16_t) fos_clamp((int) (p * LV_MAX), 0, LV_MAX);
    }
    prof_ready = 1;
}

/* profile index for d (1/256 px from the left edge) with inv = NPROF * 65536 / diameter */
#define PROF(d, inv) fos_clamp(((d) * (inv)) >> 16, 0, NPROF - 1)

/* ---------------------------------------------------------------- label painting */

/* an ink is the texel it paints */
static int ink(struct fos_can *c, int ramp, int level)
{
    c->ramps |= 1 << ramp;
    return ramp << 12 | LV12(level);
}

/* texture column for u, which is never more than a turn out */
FOS_INLINE int wrap_u(int u)
{
    return u < 0 ? u + TW : u >= TW ? u - TW : u;
}

/* put ink k with coverage cov (0..15) on texel (u, v) */
static void tex_put(struct fos_can *c, int u, int v, int k, int cov)
{
    if (cov <= 0 || v < 0 || v >= TH) return;
    uint16_t *t = &c->tex[wrap_u(u) * TH + v];
    if (cov >= 15) { *t = k; return; }
    int r, lv;
    fos_lerp2(*t >> 12, *t & 0xFFF, k >> 12, k & 0xFFF, cov * 17, &r, &lv);
    *t = r << 12 | lv;
}

static void tex_rect(struct fos_can *c, int u, int v, int w, int h, int k)
{
    if (v < 0) { h += v; v = 0; }
    if (v + h > TH) h = TH - v;
    for (int i = u; i < u + w && h > 0; i++)
    {
        uint16_t *t = &c->tex[wrap_u(i) * TH + v];
        for (int j = 0; j < h; j++)
            t[j] = k;
    }
}

/* glyph coverage straight from the font, optionally turned 90 degrees
 * counter-clockwise (text reading upwards) */
static void tex_glyph(struct fos_can *c, const struct fos_font *f, const struct fos_glyph *g,
                      int u, int v, int k, int vertical)
{
    const uint8_t *p = f->bits + g->off;
    int col = 0, row = 0;
    while (row < g->h)
    {
        int b = *p++;
        int run, val;
        if (b & 0x80) { run = (b & 0x7F) + 1; val = 0; }
        else          { run = (b >> 4) + 1;  val = b & 15; }
        while (run--)
        {
            if (val)
            {
                int gx = g->ox + col, gy = g->oy + row;
                if (vertical) tex_put(c, u + gy, v - gx, k, val);
                else          tex_put(c, u + gx, v + gy, k, val);
            }
            if (++col == g->w) { col = 0; row++; }
        }
    }
}

/* text along the label; align -1 left, 0 centre, 1 right; returns width */
static int tex_text(struct fos_can *c, const struct fos_font *f, int u, int base, const char *s,
                    int k, int track, int align)
{
    int w = fos_text_w(f, s, track);
    int pen = (u - (align + 1) * w / 2) * 16;
    for (const char *q = s; *q; q++)
    {
        int i = f->lut[(uint8_t) *q];
        if (i == 0xFF) continue;
        const struct fos_glyph *g = &f->glyphs[i];
        tex_glyph(c, f, g, (pen + 8) >> 4, base, k, 0);
        pen += g->adv + track;
    }
    return w;
}

/* text reading upwards, centred on (u, v); the baseline is at u */
static void tex_vtext(struct fos_can *c, const struct fos_font *f, int u, int v, const char *s, int k, int track)
{
    int w = fos_text_w(f, s, track);
    int pen = (v + w / 2) * 16;
    for (const char *q = s; *q; q++)
    {
        int i = f->lut[(uint8_t) *q];
        if (i == 0xFF) continue;
        const struct fos_glyph *g = &f->glyphs[i];
        tex_glyph(c, f, g, u, (pen + 8) >> 4, k, 1);
        pen -= g->adv + track;
    }
}

/* DX CAS code: two rows of six cells, bare steel (conducts) or painted.
 * The first cell of each row is always bare; the rest come from the box
 * speed and the exposure count, like the real thing (close enough). */
static void tex_dx(struct fos_can *c, int u, int v, int iso, int exposures, int steel, int paint)
{
    static const int isos[] = { 25, 50, 64, 100, 125, 160, 200, 400, 800, 1600, 3200 };
    int code = 0;
    for (int i = 0; i < (int) (sizeof(isos) / sizeof(isos[0])); i++)
        if (isos[i] == iso) code = i + 3;
    int row1 = 1 | (code << 1);
    int row2 = 1 | ((exposures == 36 || exposures <= 0 ? 5 : exposures == 24 ? 3 : 2) << 1) | 0x20;     /* no roll size (no rolls): as 36 */
    for (int i = 0; i < 6; i++)
    {
        tex_rect(c, u + i * 7, v, 6, 8, (row1 >> i) & 1 ? steel : paint);
        tex_rect(c, u + i * 7, v + 9, 6, 8, (row2 >> i) & 1 ? steel : paint);
    }
}

/* "135-36" on the canister; with no roll size (no rolls) just the format, "135" */
static void format_135(char *buf, int size, int exposures)
{
    if (exposures > 0) snprintf(buf, size, "135-%d", exposures);
    else snprintf(buf, size, "135");
}

/* a UPC-ish barcode, the same for the same stock, about 50 texels wide */
static void tex_barcode(struct fos_can *c, int u, int v, int h, const char *seed, int ink_bar)
{
    unsigned int x = 2166136261u;
    for (const char *s = seed; *s; s++) x = (x ^ (uint8_t) *s) * 16777619u;
    int pos = u;
    for (int i = 0; i < 18; i++)
    {
        x = x * 1103515245u + 12345u;
        int bar = 1 + (x >> 16) % 2, gap = 1 + (x >> 20) % 2;
        int hh = (i == 0 || i == 9 || i == 17) ? h + 3 : h;
        tex_rect(c, pos, v, bar, hh, ink_bar);
        pos += bar + gap;
    }
}

/* the parts every canister shares: the felt lip, and the back with the
 * DX code, the barcode and the small print. Going round from the lip:
 * DX code and exposures, barcode, then "PROCESS" reading upwards, all
 * clear of the front panel (about U_FRONT +- 65). */
static void paint_common(struct fos_can *c, int body, int text, const char *process)
{
    const struct fos_film *f = &fos_films[c->film];
    int steel = ink(c, RAMP_METAL, 205);
    int black = ink(c, RAMP_GRAY, 16);
    int felt = ink(c, RAMP_GRAY, 9);

    /* the light trap: a strip of black velvet between two folded edges */
    tex_rect(c, U_LIP - 5, 0, 10, TH, felt);
    tex_rect(c, U_LIP - 7, 0, 2, TH, steel);
    tex_rect(c, U_LIP + 5, 0, 2, TH, steel);

    char buf[32];
    int u_dx = U_LIP + 22;                  /* 255 */
    tex_rect(c, u_dx - 3, TH / 2 - 12, 48, 24, black);
    tex_dx(c, u_dx, TH / 2 - 9, f->box_iso, c->exposures, steel, black);
    format_135(buf, sizeof(buf), c->exposures);
    tex_text(c, &fos_font_lsm, u_dx + 21, TH / 2 - 20, buf, text, 16, 0);

    tex_barcode(c, U_LIP + 76, TH - 52, 26, f->key, text);     /* 309, round the seam */

    snprintf(buf, sizeof(buf), "PROCESS %s", process);
    tex_vtext(c, &fos_font_lsm, 64, TH / 2, buf, text, 16);
    (void) body;
}

/* text that fits the front of the canister: the widest tracking that does,
 * squeezed a little if even none is too wide */
static void tex_text_fit(struct fos_can *c, const struct fos_font *f, int u, int base, const char *s, int k, int track)
{
    const int room = 108;
    while (track > -32 && fos_text_w(f, s, track) > room) track -= 4;
    tex_text(c, f, u, base, s, k, track, 0);
}

static int label_ink(struct fos_can *c, const uint8_t ink2[2])
{
    return ink(c, ink2[0] < RAMP_COUNT ? ink2[0] : RAMP_GRAY, ink2[1]);
}

/* a label from its parts (struct fos_label): the films from the card */
static void paint_parts(struct fos_can *c, const struct fos_film *f)
{
    const struct fos_label *l = f->label;
    const int u = U_FRONT - 4;
    int bg = label_ink(c, l->bg), band = label_ink(c, l->band), acc = label_ink(c, l->accent);
    int txt = label_ink(c, l->text), num = label_ink(c, l->num);
    char bottom[40], fmt[16];
    format_135(fmt, sizeof(fmt), c->exposures);
    if (l->sub[0]) snprintf(bottom, sizeof(bottom), "%s  %s", l->sub, fmt);
    else snprintf(bottom, sizeof(bottom), "%s", fmt);
    tex_rect(c, 0, 0, TW, TH, bg);
    const struct fos_font *word = fos_text_w(&fos_font_lname, l->word, -32) <= 108 ? &fos_font_lname : &fos_font_lsm;
    switch (l->style)
    {
        case FOS_LABEL_CAP:             /* a coloured cap over the top (Portra) */
            tex_rect(c, 0, 0, TW, 24, band);
            tex_rect(c, 0, 24, TW, 4, acc);
            tex_text_fit(c, &fos_font_lsm, u, 17, l->top, acc, 24);
            tex_text_fit(c, word, u, 58, l->word, txt, 40);
            tex_text(c, &fos_font_lbig, u, 112, l->num_s, num, 0, 0);
            tex_rect(c, 0, 120, TW, 3, acc);
            tex_rect(c, 0, 125, TW, 9, band);
            tex_text_fit(c, &fos_font_lsm, u, 147, bottom, txt, 16);
            break;
        case FOS_LABEL_STRIPES:         /* thin stripes top and bottom (Ektar) */
            tex_rect(c, 0, 6, TW, 5, acc);
            tex_rect(c, 0, TH - 11, TW, 5, acc);
            tex_text_fit(c, &fos_font_lsm, u, 30, l->top, acc, 24);
            tex_text_fit(c, word, u, 60, l->word, txt, 40);
            tex_text(c, &fos_font_lbig, u, 116, l->num_s, num, 0, 0);
            tex_text_fit(c, &fos_font_lsm, u, 133, bottom, txt, 16);
            break;
        default:                        /* a band the name sits on (Gold) */
            tex_rect(c, 0, 36, TW, 34, band);
            tex_text_fit(c, &fos_font_lsm, u, 26, l->top, acc, 40);
            tex_text_fit(c, word, u, 63, l->word, txt, 48);
            tex_text(c, &fos_font_lbig, u, 122, l->num_s, num, 0, 0);
            tex_text_fit(c, &fos_font_lsm, u, 140, bottom, num, 16);
            break;
    }
    paint_common(c, bg, txt, f->process);
    if (l->leader)
    {
        c->leader_ramp = RAMP_GRAY;
        c->leader_lv = LV12(l->leader);
    }
}

static void paint_label(struct fos_can *c)
{
    const struct fos_film *f = &fos_films[c->film];
    const int u = U_FRONT - 4;           /* optical centre of the front panel */
    char exp[16];
    format_135(exp, sizeof(exp), c->exposures);
    c->ramps = 1 << RAMP_GRAY;          /* what an unpainted texel is */
    c->gloss = 220;
    c->leader_ramp = RAMP_LEADER;
    c->leader_lv = LV12(120);

    if (f->label)
        paint_parts(c, f);
    else if (!strcmp(f->key, "portra400"))
    {
        int cream = ink(c, RAMP_GRAY, 226), yel = ink(c, RAMP_AMBER, 188), org = ink(c, RAMP_ORANGE, 160);
        int red = ink(c, RAMP_RED, 150), blk = ink(c, RAMP_GRAY, 22);
        tex_rect(c, 0, 0, TW, TH, cream);
        tex_rect(c, 0, 0, TW, 24, yel);
        tex_rect(c, 0, 24, TW, 4, org);
        tex_text(c, &fos_font_lsm, u, 17, "KODAK PROFESSIONAL", red, 24, 0);
        tex_text(c, &fos_font_lname, u, 58, "PORTRA", blk, 40, 0);
        tex_text(c, &fos_font_lbig, u, 112, "400", blk, 0, 0);
        tex_rect(c, 0, 120, TW, 3, org);
        tex_rect(c, 0, 125, TW, 9, yel);
        tex_text(c, &fos_font_lsm, u, 147, exp, blk, 16, 0);
        tex_vtext(c, &fos_font_lname, u - 62, 76, "KODAK", red, 24);
        paint_common(c, cream, blk, f->process);
    }
    else if (!strcmp(f->key, "cinestill800t"))
    {
        int blk = ink(c, RAMP_GRAY, 20), red = ink(c, RAMP_RED, 150), wht = ink(c, RAMP_GRAY, 238);
        tex_rect(c, 0, 0, TW, TH, blk);
        tex_rect(c, 0, 10, TW, 3, red);
        tex_rect(c, 0, TH - 13, TW, 3, red);
        tex_text(c, &fos_font_lname, u, 44, "CINESTILL", wht, 24, 0);
        int w = fos_text_w(&fos_font_lbig, "800T", 0);
        int x0 = u - w / 2;
        int w8 = tex_text(c, &fos_font_lbig, x0, 104, "800", wht, 0, -1);
        tex_text(c, &fos_font_lbig, x0 + w8, 104, "T", red, 0, -1);
        tex_text(c, &fos_font_lsm, u, 124, "TUNGSTEN 3200K", red, 24, 0);
        tex_text(c, &fos_font_lsm, u, 139, exp, wht, 16, 0);
        paint_common(c, blk, wht, f->process);
    }
    else if (!strcmp(f->key, "ektar100"))
    {
        int crim = ink(c, RAMP_CRIMSON, 128), yel = ink(c, RAMP_AMBER, 188), wht = ink(c, RAMP_GRAY, 240);
        tex_rect(c, 0, 0, TW, TH, crim);
        tex_rect(c, 0, 6, TW, 5, yel);
        tex_rect(c, 0, TH - 11, TW, 5, yel);
        tex_text(c, &fos_font_lsm, u, 30, "KODAK PROFESSIONAL", yel, 24, 0);
        tex_text(c, &fos_font_lname, u, 60, "EKTAR", wht, 48, 0);
        tex_text(c, &fos_font_lbig, u, 116, "100", yel, 0, 0);
        tex_text(c, &fos_font_lsm, u, 132, exp, wht, 16, 0);
        paint_common(c, crim, wht, f->process);
    }
    else if (!strcmp(f->key, "gold200"))
    {
        int yel = ink(c, RAMP_AMBER, 190), red = ink(c, RAMP_RED, 148), blk = ink(c, RAMP_GRAY, 22);
        int wht = ink(c, RAMP_GRAY, 240);
        tex_rect(c, 0, 0, TW, TH, yel);
        tex_rect(c, 0, 36, TW, 32, red);
        tex_text(c, &fos_font_lsm, u, 24, "KODAK", red, 48, 0);
        tex_text(c, &fos_font_lname, u, 62, "GOLD", wht, 56, 0);
        tex_text(c, &fos_font_lbig, u, 122, "200", blk, 0, 0);
        tex_text(c, &fos_font_lsm, u, 140, exp, blk, 16, 0);
        tex_vtext(c, &fos_font_lsm, u - 60, 96, "COLOR", red, 24);
        paint_common(c, yel, blk, f->process);
    }
    else if (!strcmp(f->key, "superia400"))
    {
        int grn = ink(c, RAMP_GREEN, 140), wht = ink(c, RAMP_GRAY, 240), red = ink(c, RAMP_RED, 150);
        int dk = ink(c, RAMP_GREEN, 70);
        tex_rect(c, 0, 0, TW, TH, grn);
        tex_rect(c, 0, 72, TW, 52, dk);
        tex_rect(c, 0, 124, TW, 3, red);
        tex_text(c, &fos_font_lsm, u, 24, "FUJIFILM", wht, 48, 0);
        tex_text(c, &fos_font_lname, u, 58, "SUPERIA", wht, 24, 0);
        tex_text(c, &fos_font_lbig, u, 118, "400", wht, 0, 0);
        tex_text(c, &fos_font_lsm, u, 142, exp, wht, 16, 0);
        paint_common(c, grn, wht, f->process);
    }
    else if (!strcmp(f->key, "velvia50"))
    {
        int vio = ink(c, RAMP_VIOLET, 118), wht = ink(c, RAMP_GRAY, 240), lil = ink(c, RAMP_VIOLET, 225);
        tex_rect(c, 0, 0, TW, TH, vio);
        tex_rect(c, 0, 0, TW, 6, lil);
        tex_rect(c, 0, TH - 6, TW, 6, lil);
        tex_text(c, &fos_font_lsm, u, 28, "FUJICHROME", wht, 32, 0);
        tex_text(c, &fos_font_lname, u, 58, "VELVIA", wht, 40, 0);
        tex_text(c, &fos_font_lbig, u, 116, "50", lil, 0, 0);
        tex_text(c, &fos_font_lsm, u, 134, "RVP 50  " , wht, 24, 0);
        paint_common(c, vio, wht, f->process);
        c->leader_lv = LV12(70);
    }
    else if (!strcmp(f->key, "hp5"))
    {
        int wht = ink(c, RAMP_GRAY, 236), blk = ink(c, RAMP_GRAY, 20), blu = ink(c, RAMP_BLUE, 160);
        tex_rect(c, 0, 0, TW, TH, wht);
        tex_rect(c, 0, 40, TW, 76, blk);
        tex_rect(c, 0, 116, TW, 3, blu);
        tex_text(c, &fos_font_lname, u, 32, "ILFORD", blk, 48, 0);
        tex_text(c, &fos_font_lbig, u, 92, "HP5", wht, 0, 0);
        tex_text(c, &fos_font_lsm, u, 108, "PLUS", blu, 64, 0);
        tex_text(c, &fos_font_lname, u - 16, 142, "400", blk, 24, 0);
        tex_text(c, &fos_font_lsm, u + 30, 141, exp, blk, 0, 0);
        paint_common(c, wht, blk, f->process);
        c->leader_ramp = RAMP_GRAY;
        c->leader_lv = LV12(52);
    }
    else if (!strcmp(f->key, "trix400"))
    {
        int blk = ink(c, RAMP_GRAY, 18), yel = ink(c, RAMP_AMBER, 190), wht = ink(c, RAMP_GRAY, 238);
        int red = ink(c, RAMP_RED, 150);
        tex_rect(c, 0, 0, TW, TH, blk);
        tex_rect(c, 0, 14, TW, 50, yel);
        tex_text(c, &fos_font_lsm, u, 30, "KODAK", blk, 48, 0);
        tex_text(c, &fos_font_lname, u, 56, "TRI-X", blk, 40, 0);
        tex_text(c, &fos_font_lbig, u, 120, "400", wht, 0, 0);
        tex_text(c, &fos_font_lsm, u - 30, 138, "TX", red, 16, 0);
        tex_text(c, &fos_font_lsm, u + 14, 138, exp, wht, 16, 0);
        paint_common(c, blk, wht, f->process);
        c->leader_ramp = RAMP_GRAY;
        c->leader_lv = LV12(48);
    }
}

struct fos_can *fos_can_new(int film, int exposures)
{
    if (!prof_ready) build_profiles();
    struct fos_can *c = fos_port_alloc(sizeof(struct fos_can));
    if (!c) return 0;
    memset(c, 0, sizeof(*c));
    c->tex = fos_port_alloc(TW * TH * sizeof(uint16_t));
    if (!c->tex) { fos_port_free(c); return 0; }
    c->film = fos_clamp(film, 0, fos_nfilms - 1);
    c->exposures = exposures;
    memset(c->tex, 0, TW * TH * sizeof(uint16_t));
    paint_label(c);
    return c;
}

void fos_can_free(struct fos_can *c)
{
    if (!c) return;
    fos_port_free(c->tex);
    fos_port_free(c);
}

void fos_can_set_exposures(struct fos_can *c, int exposures)
{
    if (!c || c->exposures == exposures) return;
    c->exposures = exposures;
    memset(c->tex, 0, TW * TH * sizeof(uint16_t));
    paint_label(c);
}


/* ---------------------------------------------------------------- drawing */

/* ellipse half-height of a circle of radius r8 at offset dx8 (all 1/256 px) */
static int arc8(int dx8, int r8)
{
    int q = r8 * r8 - dx8 * dx8;
    if (q <= 0) return 0;
    return fos_isqrt((unsigned) q >> 8) * 16 * TILT / 256;
}

/* one column of the canister, worked out once per frame */
struct can_col
{
    int16_t body;       /* column crosses the body (not only the caps) */
    int16_t u, uf;      /* label texture column and its fraction (0..255) */
    int16_t cov;        /* silhouette coverage 0..256 */
    int16_t metal;      /* caps, 12-bit, lit */
    int16_t spool;      /* spool side, 12-bit, lit; -1 outside it */
    int16_t ex, sx;     /* x across the cap / the spool, 4096 = edge */
    int32_t ex2, sx2;   /* ... squared */
    int32_t gain, offs; /* the label's light here: lv * gain / 65536 + offs */
    int32_t top8;       /* y of the top band's upper edge, 1/256 px */
    int32_t cap8;       /* the highest the cap or the spool reaches */
    int32_t sp_top8, sp_bot8;   /* spool side, 1/256 px */
};

#define MAX_COLS 176
#define MAX_CAP_ROWS 64
static struct can_col cols[MAX_COLS];


FOS_INLINE int lit(int lv, int light)
{
    return (lv * light) >> 8;
}

FOS_INLINE void put(struct fos_canvas *cv, int x, int y, int ramp, int lv, int cov)
{
    uint8_t *px = &cv->px[y * cv->pitch + x];
    if (cov >= 256) *px = fos_ixd(ramp, fos_ramp_pos(ramp, lv), x, y);
    else if (cov > 0) *px = fos_mix(*px, ramp, lv, cov);
}

#define ABS16(v) ((v) < 0 ? -(v) : (v))

/* ramp position (1/16 entries) -> palette index, clamped at the light end */
static uint8_t ix_tab[RAMP_COUNT][256];
static int ix_tab_ready;

static void build_ix_tab()
{
    for (int r = 0; r < RAMP_COUNT; r++)
        for (int q = 0; q < 256; q++)
            ix_tab[r][q] = fos_ramp_base[r] + (q > fos_ramp_last[r] ? fos_ramp_last[r] : q);
    ix_tab_ready = 1;
}

/* n rows of the leader fully inside its edges; v (0..4096 across the
 * film) steps by dv, holes are the v ranges [h, h + 480) */
__attribute__((noinline))
static void leader_run(uint8_t *px, int pitch, int n, int v, int dv, int lv_along, int sheen_k, int lmul,
                       const uint8_t *tab, const uint8_t *bay, int h1, int h2)
{
    for (int i = 0; i < n; i++, v += dv, px += pitch)
    {
        if ((unsigned) (v - h1) < 480 || (unsigned) (v - h2) < 480) continue;
        /* glossy emulsion: a soft sheen a third of the way down */
        int sheen = 4096 - ABS16(v - 1300) * 5;
        int lv = lv_along + (sheen > 0 ? (sheen * sheen_k) >> 20 : 0);
        int q = (((lv * lmul) >> 16) + bay[i & 3]) >> 4;
        *px = tab[q & 255];
    }
}

/* the film leader coming out of the lip: full width at first, then the
 * cut-down tongue, perforated along its edges. It leaves the lip heading
 * away from us, so it tapers a little and rises with distance. */
static void draw_leader(struct fos_canvas *cv, const struct fos_can *c, const struct fos_can_pose *p,
                        int y_body_top, int s)
{
    int lip = (U_LIP - U_FRONT) * 65536 / TW - p->rot;     /* lip angle on screen */
    int sn = fos_sin(lip);                  /* 4096 = right edge */
    if (sn < 900) return;
    /* p->leader: 256 = the usual tongue, more = pulled out further */
    int len = (70 * s >> 8) * p->leader / 256 * sn / 4096;     /* foreshortened as it turns */
    int tongue = 50 * s / 256 * sn / 4096;
    if (len < 4) return;
    int x0 = (p->cx8 >> 8) + (R * s / 256) * sn / 4096 - 3 * s / 256;
    int fw8 = 112 * s;                      /* film width, 1/256 px */
    int mid8 = y_body_top * 256 + HB * s / 2;
    int full = len - tongue > len * 28 / 100 ? len - tongue : len * 28 / 100;   /* full width before the cut */
    int curve = 12 * s / 256 + 1;
    int perf_w = fos_clamp(8 * s / 256, 2, 20), pitch = fos_clamp(15 * s / 256, 4, 40);
    int end_r = 9 * s / 256 + 1;
    int ramp = c->leader_ramp;
    int lmul = (p->light * fos_ramp_mul[ramp]) >> 8;       /* light and ramp position in one */
    int inv_len = 65536 / len;
    if (!ix_tab_ready) build_ix_tab();
    int xa = fos_clamp(x0, cv->cx0, cv->cx1), xb = fos_clamp(x0 + len, cv->cx0, cv->cx1);

    for (int x = xa; x < xb; x++)
    {
        int t = x - x0;
        int depth = (t * inv_len) >> 8;                     /* 0 at the lip, 256 at the tip */
        int hw8 = (fw8 / 2) * (256 - depth * 40 / 256) >> 8;
        int cy8 = mid8 - depth * s / 64;
        int top8 = cy8 - hw8, bot8 = cy8 + hw8;
        if (t > full)
        {
            /* the tongue keeps the lower half; the cut curves down to it */
            int k = t - full;
            int f = k >= curve ? 4096 : (4096 - fos_cos(k * 32768 / curve)) / 2;
            top8 = cy8 - hw8 + ((hw8 * f) >> 12);
            int rem = len - t;
            if (rem < end_r)
            {
                int dy = fos_isqrt((unsigned) (end_r * end_r - (end_r - rem) * (end_r - rem))) * 256;
                int c8 = (top8 + bot8) / 2;
                if (top8 < c8 - dy) top8 = c8 - dy;
                if (bot8 > c8 + dy) bot8 = c8 + dy;
            }
        }
        if (bot8 <= top8) continue;
        int ph = (t + pitch / 2) % pitch;
        int perf_col = ph < perf_w && t > 2;
        /* darker where it disappears into the lip and towards the tip */
        int along = 3000 + 1100 * fos_sin(depth * 128 + 0x2000) / 4096 - (t < 3 ? 1400 : 0);
        int lv_along = lit(c->leader_lv, along * 256 / 4096);
        int sheen_k = (LV_MAX - lv_along) * 60;
        int ya = fos_clamp(top8 >> 8, cv->cy0, cv->cy1), yb = fos_clamp((bot8 >> 8) + 1, cv->cy0, cv->cy1);
        /* position across the film, 0..4096 from its top edge, stepped per row */
        int dv = 4096 * 256 / (2 * hw8 > 0 ? 2 * hw8 : 1);
        int v = ((ya * 256 + 128 - (cy8 - hw8)) * dv) >> 8;
        /* sprocket holes, as ranges of v: the background shows */
        int h1 = perf_col && t <= full ? 300 : 1 << 20, h2 = perf_col ? 3316 : 1 << 20;
        /* whole rows inside the edges go through the quick loop */
        int yf0 = fos_clamp((top8 + 255) >> 8, ya, yb), yf1 = fos_clamp(bot8 >> 8, yf0, yb);
        uint8_t *px = &cv->px[ya * cv->pitch + x];
        for (int y = ya; y < yb; y++, v += dv, px += cv->pitch)
        {
            if (y == yf0 && yf1 > yf0)
            {
                uint8_t bay[4];
                for (int j = 0; j < 4; j++) bay[j] = fos_bayer[(y + j) & 3][x & 3];
                leader_run(px, cv->pitch, yf1 - yf0, v, dv, lv_along, sheen_k, lmul, &ix_tab[ramp][0], bay, h1, h2);
                int n = yf1 - yf0 - 1;
                y += n; v += n * dv; px += n * cv->pitch;
                continue;
            }
            int yc = y * 256 + 128;
            int cov = fos_clamp(128 + (yc - top8), 0, 256) * fos_clamp(128 + (bot8 - yc), 0, 256) >> 8;
            if (cov <= 0) continue;
            if ((unsigned) (v - h1) < 480 || (unsigned) (v - h2) < 480) continue;
            /* glossy emulsion: a soft sheen a third of the way down */
            int sheen = 4096 - ABS16(v - 1300) * 5;
            int lv = lv_along + (sheen > 0 ? (sheen * sheen_k) >> 20 : 0);
            *px = fos_mix(*px, ramp, lit(lv, p->light), cov);
        }
    }
}

/* geometry of one pose, shared by the parts */
struct can_geo
{
    int s, r8, rc8, rs8, cx8;
    int e_c, e_s;           /* cap / spool ellipse half heights */
    int tb8, hb8, bb8, hs8;
    int yc8;                /* cap ellipse centre */
    int xl, xr;             /* the canister's columns */
    int x0, x1;             /* ... those inside the clip */
};

static int prepare(const struct fos_canvas *cv, const struct fos_can *c, const struct fos_can_pose *p,
                   struct can_geo *g)
{
    int s = p->scale;
    if (s <= 0) return 0;
    g->s = s;
    g->r8 = R * s; g->rc8 = RC * s; g->rs8 = RS * s;
    g->cx8 = p->cx8;
    g->e_c = g->rc8 * TILT / 256;
    g->e_s = g->rs8 * TILT / 256;
    g->tb8 = TB * s; g->hb8 = HB * s; g->bb8 = BB * s; g->hs8 = HS * s;
    g->yc8 = p->floor_y * 256 - (g->e_c + g->tb8 + g->hb8 + g->bb8);
    g->xl = (g->cx8 - g->rc8) >> 8;
    g->xr = (g->cx8 + g->rc8 + 255) >> 8;
    if (g->xr - g->xl > MAX_COLS) return 0;
    g->x0 = fos_clamp(g->xl, cv->cx0, cv->cx1);
    g->x1 = fos_clamp(g->xr, cv->cx0, cv->cx1);

    /* reciprocals instead of divisions in the loop */
    int inv_pb = (NPROF << 16) / (2 * g->r8), inv_pc = (NPROF << 16) / (2 * g->rc8);
    int inv_ps = (NPROF << 16) / (2 * g->rs8);
    int inv_ec = (1 << 28) / g->rc8, inv_es = (1 << 28) / g->rs8;
    int fade = p->light;
    for (int x = g->x0; x < g->x1; x++)
    {
        struct can_col *k = &cols[x - g->xl];
        int dx8 = x * 256 + 128 - g->cx8;
        int adx = dx8 < 0 ? -dx8 : dx8;
        k->body = adx < g->r8;
        int pi = k->body ? PROF(dx8 + g->r8, inv_pb) : PROF(dx8 + g->rc8, inv_pc);
        int u16 = ((prof_ang[pi] + p->rot) & 0xFFFF) * TW + U_FRONT * 65536;
        k->u = wrap_u(u16 >> 16);
        k->uf = (u16 >> 8) & 0xFF;
        k->cov = fos_clamp(128 + (g->rc8 - adx), 0, 256);
        k->metal = lit(prof_metal[PROF(dx8 + g->rc8, inv_pc)], fade);
        int arc_c = arc8(dx8, g->rc8);
        k->top8 = g->yc8 + arc_c;
        k->cap8 = g->yc8 - arc_c - 512;
        k->ex = fos_clamp((dx8 * inv_ec) >> 16, -8192, 8192);
        k->sx = fos_clamp((fos_clamp(dx8, -4 * g->rs8, 4 * g->rs8) * inv_es) >> 16, -16384, 16384);
        k->ex2 = k->ex * k->ex;
        k->sx2 = k->sx * k->sx;
        if (adx < g->rs8)
        {
            int arc_s = arc8(dx8, g->rs8);
            k->spool = lit(prof_dark[PROF(dx8 + g->rs8, inv_ps)], fade);
            k->sp_top8 = g->yc8 - g->hs8 + arc_s;
            k->sp_bot8 = g->yc8 + arc_s;
            if (g->yc8 - g->hs8 - arc_s - 256 < k->cap8) k->cap8 = g->yc8 - g->hs8 - arc_s - 256;
        }
        else k->spool = -1;
        /* diffuse light, then the gloss over it, all faded */
        int sp = (prof_spec[pi] * c->gloss) >> 8;
        k->gain = ((prof_light[pi] * (256 - sp)) >> 8) * fade;
        k->offs = (sp * LV_MAX * fade) >> 16;
    }
    return 1;
}

/* the cap seen from above: rolled rim, recessed disc, the spool end */
static void draw_cap(struct fos_canvas *cv, const struct can_geo *g, int rot, int fade)
{
    if (!ix_tab_ready) build_ix_tab();
    int ya = fos_clamp(((g->yc8 - g->hs8 - g->e_s) >> 8) - 2, cv->cy0, cv->cy1);
    int yb = fos_clamp(((g->yc8 + g->e_c) >> 8) + 2, ya, cv->cy1);
    if (yb - ya > MAX_CAP_ROWS) yb = ya + MAX_CAP_ROWS;
    /* per row: y across the cap and the spool top (4096 = edge), squared,
     * and the rim's and the disc's light falling off towards us */
    struct { int ey2, sy, sy2, rim, disc, inv; } rows[MAX_CAP_ROWS];
    int ec = g->e_c ? g->e_c : 1, es = g->e_s ? g->e_s : 1;
    int inv_e = (1 << 28) / ec, inv_s = (1 << 28) / es;
    for (int y = ya; y < yb; y++)
    {
        int yc = y * 256 + 128, j = y - ya;
        int ey = (fos_clamp(yc - g->yc8, -4 * ec, 4 * ec) * inv_e) >> 16;
        int sy = (fos_clamp(yc - (g->yc8 - g->hs8), -4 * es, 4 * es) * inv_s) >> 16;
        rows[j].ey2 = ey * ey;
        rows[j].sy = sy;
        rows[j].sy2 = sy * sy;
        rows[j].rim = LV12(150) - ey * LV12(60) / 4096;
        rows[j].disc = LV12(96) - ey * LV12(22) / 4096;
        /* how fast rho2 changes per px going down: the edge is that soft */
        int gy = 2 * (ey < 0 ? -ey : ey) * 256 / ec;
        rows[j].inv = (1 << 24) / (gy > 64 ? gy : 64);
    }
    int cs = fos_cos(rot * 2), sn = fos_sin(rot * 2);
    for (int x = g->x0; x < g->x1; x++)
    {
        const struct can_col *k = &cols[x - g->xl];
        if (k->cov <= 0) continue;
        /* ... and going across: the edge's anti-aliasing follows the steeper */
        int gx = 2 * (k->ex < 0 ? -k->ex : k->ex) * 256 / g->rc8;
        int inv_x = (1 << 24) / (gx > 64 ? gx : 64);
        int y0 = fos_clamp(k->cap8 >> 8, ya, yb);
        int rim_x = k->ex * LV12(40) / 4096, disc_x = k->ex * LV12(30) / 4096;
        int spool_top = LV12(34) - k->sx * LV12(14) / 4096, slot_x = cs * k->sx;
        uint8_t bay[4];
        for (int j = 0; j < 4; j++) bay[j] = fos_bayer[j][x & 3];
        uint8_t *px = &cv->px[y0 * cv->pitch + x];
        for (int y = y0; y < yb; y++, px += cv->pitch)
        {
            int yc = y * 256 + 128, j = y - ya;
            if (yc >= k->top8) break;
            int rho2 = (k->ex2 + rows[j].ey2) >> 12;    /* 4096 = edge */
            if (rho2 > 4096 + 400) continue;
            int inv = rows[j].inv < inv_x ? rows[j].inv : inv_x;
            int cov = 128 + (((4096 - rho2) * inv) >> 16);
            if (cov <= 0) continue;
            if (cov > 256) cov = 256;
            if (k->cov < 256) cov = cov * k->cov >> 8;
            int ramp = RAMP_METAL, lv;
            int srho2 = (k->sx2 + rows[j].sy2) >> 12;
            if (srho2 <= 4096)
            {
                /* top of the spool: black plastic with the key slot */
                ramp = RAMP_GRAY;
                int along = (slot_x + sn * rows[j].sy) >> 12;
                int slot = along > -1000 && along < 1000 && srho2 < 3000;
                lv = slot ? LV12(8) : srho2 > 3300 ? LV12(48) : spool_top;
            }
            else if (k->spool >= 0 && yc >= k->sp_top8 && yc < k->sp_bot8)
            {
                ramp = RAMP_GRAY;
                lv = k->spool;
                goto out;
            }
            else if (rho2 > 3250)
            {
                /* rolled rim: catches the light, brighter at the back */
                lv = rows[j].rim - rim_x - (rho2 > 3900 ? LV12(40) : 0);
            }
            else
            {
                /* recessed disc: a soft gradient, a ring round the spool */
                lv = srho2 < 4096 * 5 / 3 ? LV12(60) : rows[j].disc - disc_x;
            }
            lv = fos_clamp(lit(lv, fade), 0, LV_MAX);
        out:
            if (cov >= 256) *px = ix_tab[ramp][((fos_ramp_pos(ramp, lv) + bay[y & 3]) >> 4) & 255];
            else if (cov > 0) *px = fos_mix(*px, ramp, lv, cov);
        }
    }
}

/* Label pixels down a column (the canvas is FOS_W wide). v8 is the
 * texture row in 1/256, stepping by ds. For each ramp gb holds 8 words:
 * this column's gain, then its offset with the dither threshold of rows
 * 0..3 of the run already added. Kept apart from the rest of the drawing
 * so the loop has the registers to itself. */
#define LABEL_TEXEL(j)                                                          \
    {                                                                           \
        int t = *(const uint16_t *) ((const uint8_t *) b0 + ((v8 >> 7) & ~1));  \
        int r = t >> 12;                                                        \
        const int32_t *k = &gb[r * 8];                                          \
        int q = ((((t & 0xFFF) * k[0]) >> 16) + k[1 + (j)]) >> 4;               \
        *px = tab[r << 8 | (q & 255)];                                          \
        px += FOS_W; v8 += ds;                                                  \
    }

__attribute__((noinline))
static void label_run(uint8_t *px, int n, int v8, int ds, const uint16_t *b0, const int32_t *gb)
{
    const uint8_t *tab = &ix_tab[0][0];
    for (; n >= 4; n -= 4)
    {
        LABEL_TEXEL(0) LABEL_TEXEL(1) LABEL_TEXEL(2) LABEL_TEXEL(3)
    }
    if (n > 0) LABEL_TEXEL(0)
    if (n > 1) LABEL_TEXEL(1)
    if (n > 2) LABEL_TEXEL(2)
}

/* the same between two texture columns, a fraction uf of the way to b1:
 * levels of one ramp blend, different inks dither (thr: thresholds) */
#define LABEL_TEXEL2(j)                                                         \
    {                                                                           \
        int o = (v8 >> 7) & ~1;                                                 \
        int t = *(const uint16_t *) ((const uint8_t *) b0 + o);                 \
        int t1 = *(const uint16_t *) ((const uint8_t *) b1 + o);                \
        if (t1 != t)                                                            \
        {                                                                       \
            if ((t ^ t1) >> 12) { if (uf > thr[j]) t = t1; }                    \
            else t += (((t1 & 0xFFF) - (t & 0xFFF)) * uf) >> 8;                 \
        }                                                                       \
        int r = t >> 12;                                                        \
        const int32_t *k = &gb[r * 8];                                          \
        int q = ((((t & 0xFFF) * k[0]) >> 16) + k[1 + (j)]) >> 4;               \
        *px = tab[r << 8 | (q & 255)];                                          \
        px += FOS_W; v8 += ds;                                                  \
    }

__attribute__((noinline))
static void label_run_filtered(uint8_t *px, int n, int v8, int ds, const uint16_t *b0, const uint16_t *b1,
                               int uf, const int32_t *gb, const uint8_t *thr)
{
    const uint8_t *tab = &ix_tab[0][0];
    for (; n >= 4; n -= 4)
    {
        LABEL_TEXEL2(0) LABEL_TEXEL2(1) LABEL_TEXEL2(2) LABEL_TEXEL2(3)
    }
    if (n > 0) LABEL_TEXEL2(0)
    if (n > 1) LABEL_TEXEL2(1)
    if (n > 2) LABEL_TEXEL2(2)
}

/* first row whose centre is at least a below edge8 (1/256 px) */
FOS_INLINE int row_at(int edge8, int a)
{
    return (edge8 + a - 128 + 255) >> 8;
}

/* the bands and the label, a column at a time: the texture row just steps */
static void draw_body(struct fos_canvas *cv, const struct fos_can *c, const struct can_geo *g, int fade, int fast)
{
    int ds = 65536 / g->s;                      /* texels per row, 8.8 */
    int band_t = g->tb8, body_b = g->tb8 + g->hb8, band_b = body_b + g->bb8;
    int step_t = 65536 / (band_t ? band_t : 1), step_b = 65536 / (g->bb8 ? g->bb8 : 1);
    int lip_hi = lit(LV12(40), fade), groove = lit(LV12(70), fade), ridge = lit(LV12(30), fade);
    int filter = fos_can_quality > 1;
    int pitch = cv->pitch;
    if (!ix_tab_ready) build_ix_tab();

    for (int x = g->x0; x < g->x1; x++)
    {
        const struct can_col *k = &cols[x - g->xl];
        if (k->cov <= 0) continue;
        int ya = fos_clamp(row_at(k->top8, 0), cv->cy0, cv->cy1);
        int yb = fos_clamp(row_at(k->top8, band_t), cv->cy0, cv->cy1);
        int yc_ = fos_clamp(row_at(k->top8, body_b), cv->cy0, cv->cy1);
        int yd = fos_clamp(row_at(k->top8, band_b), cv->cy0, cv->cy1);
        int y;

        /* top crimp: a rolled lip above, a groove below; f = 0..256 down the band */
        int f = ((ya * 256 + 128 - k->top8) * step_t) >> 8;
        for (y = ya; y < yb; y++, f += step_t)
        {
            int lv = k->metal + (f < 70 ? lip_hi : 0) - (f > 210 ? groove : 0);
            put(cv, x, y, RAMP_METAL, fos_clamp(lv, 0, LV_MAX), k->cov);
        }

        /* the printed label */
        if (k->body && yb < yc_)
        {
            const uint16_t *b0 = &c->tex[k->u * TH];
            const uint16_t *b1 = &c->tex[(k->u + 1 < TW ? k->u + 1 : 0) * TH];
            int v8 = ((yb * 256 + 128 - k->top8 - band_t) * ds) >> 8;
            /* keep the texture row inside the label */
            int ye = yc_;
            if (v8 < 0) v8 = 0;
            if (((v8 + (ye - yb - 1) * ds) >> 8) >= TH) ye = yb + ((TH * 256 - 1 - v8) / ds) + 1;
            uint8_t *px = &cv->px[yb * pitch + x];
            if (k->cov >= 256 && ye > yb && pitch == FOS_W)
            {
                /* this column's light, folded into each ramp's position
                 * scale, with the dither of the run's rows 0..3 */
                int32_t gb[RAMP_COUNT * 8];
                uint8_t thr[4];
                int g4 = k->gain >> 4;
                int bay[4];
                for (int j = 0; j < 4; j++)
                {
                    int yy = (yb + j) & 3;
                    bay[j] = fos_bayer[yy][x & 3];
                    thr[j] = fos_bayer[(yy + 2) & 3][(x + 1) & 3] * 16 + 8;
                }
                for (int r = 0; r < RAMP_COUNT; r++)
                {
                    if (!(c->ramps & (1 << r))) continue;
                    int32_t *e = &gb[r * 8];
                    int o = (k->offs * fos_ramp_mul[r]) >> 16;
                    e[0] = (g4 * fos_ramp_mul[r]) >> 12;
                    e[1] = o + bay[0]; e[2] = o + bay[1]; e[3] = o + bay[2]; e[4] = o + bay[3];
                }
                if (filter && k->uf && !fast)
                    label_run_filtered(px, ye - yb, v8, ds, b0, b1, k->uf, gb, thr);
                else
                    label_run(px, ye - yb, v8, ds, b0, gb);
            }
            else
            {
                /* the silhouette's edge columns, blended with what is behind */
                for (y = yb; y < ye; y++, v8 += ds, px += pitch)
                {
                    int t = b0[v8 >> 8];
                    int lv = (((t & 0xFFF) * k->gain) >> 16) + k->offs;
                    *px = fos_mix(*px, t >> 12, lv > LV_MAX ? LV_MAX : lv, k->cov);
                }
            }
        }

        /* bottom crimp, its lower edge following the cap ellipse */
        int rel = yc_ * 256 + 128 - k->top8;
        f = ((rel - body_b) * step_b) >> 8;
        for (y = yc_; y < yd; y++, rel += 256, f += step_b)
        {
            int lv = k->metal - (f < 50 ? groove : 0) + (f > 140 && f < 200 ? ridge : 0);
            int cov = k->cov;
            int below = rel - (band_b - 256);
            if (below > 0) cov = cov * (256 - below) >> 8;
            put(cv, x, y, RAMP_METAL, fos_clamp(lv, 0, LV_MAX), cov);
        }
    }
}

/* the canister mirrored in the glossy floor, fading out below it: a little
 * of each pixel above the bottom edge glazed over the floor below it */
static void draw_reflection(struct fos_canvas *cv, const struct fos_can_pose *p, const struct can_geo *g)
{
    int depth = 38 * g->s / 256;
    if (depth <= 0) return;
    int y0 = fos_clamp(p->floor_y, cv->cy0, cv->cy1), y1 = fos_clamp(p->floor_y + depth, cv->cy0, cv->cy1);
    if (y1 - y0 > 64) y1 = y0 + 64;
    int kk[64];
    for (int y = y0; y < y1; y++)
    {
        int k = p->reflect * (depth - (y - p->floor_y)) / depth;
        kk[y - y0] = (k * k / 256) * 200 >> 8;          /* the print a little darker in the floor */
        if (kk[y - y0] < 2) { y1 = y; break; }          /* fainter than a level step */
    }
    int edge_off = g->tb8 + g->hb8 + g->bb8;            /* from the band's top edge to the bottom outline */
    int pitch = cv->pitch;
    for (int x = g->x0; x < g->x1; x++)
    {
        const struct can_col *col = &cols[x - g->xl];
        if (col->cov < 128) continue;
        int edge8 = col->top8 + edge_off;
        int reach = col->body ? 1 << 30 : g->bb8;      /* past the body only the band shows */
        for (int y = y0; y < y1; y++)
        {
            int up = y * 256 + 128 - edge8;             /* below the edge = above it in the mirror */
            if (up < 0) continue;
            if (up >= reach) break;
            int sy = (edge8 - up) >> 8;
            if (sy < cv->cy0) break;
            int k = kk[y - y0];
            if (k <= 0) break;
            uint8_t *px = &cv->px[y * pitch + x];
            *px = fos_glaze(*px, cv->px[sy * pitch + x], k, x, y);
        }
    }
}

void fos_can_draw(struct fos_canvas *cv, struct fos_can *c, const struct fos_can_pose *p)
{
    struct can_geo g;
    if (!c || !prepare(cv, c, p, &g)) return;
    if (g.xr <= cv->cx0 || g.xl >= cv->cx1) return;
    if (p->leader)
        draw_leader(cv, c, p, (g.yc8 + g.tb8) >> 8, g.s);
    draw_cap(cv, &g, p->rot, p->light);
    draw_body(cv, c, &g, p->light, p->fast);
    if (p->reflect && fos_can_quality > 0)
        draw_reflection(cv, p, &g);
}
