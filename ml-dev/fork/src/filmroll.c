/* filmroll: a Canon camera as a film camera (built into this ML fork).
 * Made on the EOS M; 100D, 650D, 700D and others build from the same code.
 *
 * The camera side of the film OS (ml-dev/filmos): rolls, the roll log the
 * PC lab reads, film settings, the viewfinder HUD, and the fos_port_*
 * functions through which the film OS takes the screen and the buttons.
 *
 *  - the film OS is the camera's own interface: the film shelf at start
 *    up, home and its settings pages instead of the Magic Lantern menu.
 *    The ML menu is gone: what used to open it (two-finger tap, long
 *    press on DOWN, modules) opens home.
 *  - an SLR-viewfinder HUD in LiveView: aperture scale with meter needle,
 *    red LED readout that lights on half-shutter, frame counter, wind
 *  - PLAY opens the prints the camera has developed, never Canon's
 *    playback of undeveloped frames
 *  - a finished roll is developed in the camera (fos_dev.c): its RAWs
 *    become prints in ML/DARKROOM, shown on their own palettes
 *  - manual lenses: the f-stop is dialled in and logged for the lab
 *
 * Loading a roll sets picture style, contrast / saturation / colour tone,
 * white balance and ISO for that stock, and starts a roll log next to the
 * images (DCIM/xxxCANON/ROLL_nnn.TXT) that the PC film lab reads
 * (the desktop darkroom, Darkrooms). Film keys must match films/films.py.
 *
 * Shots are detected by watching the file number, so every frame is
 * logged even in burst mode.
 */

#ifdef MODULE
/* ML/modules/filmos.mo is built for one camera (fork/module/Makefile), so it
 * takes that camera's definitions, as autoexec.bin does */
#include "internals.h"
#include "internals-common.h"
#include "features.h"
#include "consts.h"
#include "gui.h"        /* ahead of dryos.h's gui-common.h, as in autoexec.bin */
#endif
#include <dryos.h>
#include <module.h>
#include <property.h>
#include <menu.h>
#include <config.h>
#include <lens.h>
#include <picstyle.h>
#include <beep.h>
#include <bmp.h>
#include <fio-ml.h>
#include <fileprefix.h>
#include <notify_box.h>
#include <zebra.h>
#include <propvalues.h>
#include <gui-common.h>
#include <tasks.h>
#include <mem.h>
#include <powersave.h>
#ifdef MODULE
/* built as ML/modules/filmos.mo (build_fork.sh --module): the core keeps
 * these three names for its shim (filmhook.c), the module's own go by others */
#define handle_filmroll_keys     fr_handle_keys
#define filmroll_menu_requested  fr_menu_requested
#define filmroll_overlays_active fr_overlays_active
#endif
#include "filmroll.h"
#include "fos.h"
#include "fos_dev.h"

#include <stdarg.h>

extern int ml_started;
extern void CancelDateTimer();

/* user settings */
static CONFIG_INT("filmroll.film",      film_sel,         0);
static CONFIG_INT("filmroll.exp",       exp_sel,          0);
static CONFIG_INT("filmroll.rolllim",   roll_limit,       0);   /* FS_ROLLLIM: 0 Off (no rolls), 1 36, 2 24, 3 12 */
static CONFIG_INT("filmroll.hud",       hud_enabled,      1);
static CONFIG_INT("filmroll.focus",     focus_screen,     1);
static CONFIG_INT("filmroll.leds",      leds_on_half,     1);
static CONFIG_INT("filmroll.wind",      wind_mode,        1);   /* 0 off, 1 reminder, 2 block shutter */
static CONFIG_INT("filmroll.playlock",  play_lock,        1);
static CONFIG_INT("filmroll.startup",   startup_picker,   2);   /* 0 off, 1 when no film, 2 always */
static CONFIG_INT("filmroll.bootanim",  boot_anim,        1);
static CONFIG_INT("filmroll.debug",     debug_log,        1);
static CONFIG_INT("filmroll.mode",      film_mode,        0);   /* 0 Basic, 1 Realistic (FS_MODE) */
static CONFIG_INT("filmroll.lens_wide", lens_widest,      6);   /* index into fos_fstops: f/1.8 */
static CONFIG_INT("filmroll.fstop",     manual_fstop,     6);
static CONFIG_INT("filmroll.ap_keys",   aperture_keys,    0);   /* 0 wheel in Av, 1 up/down, 2 menu only */
static CONFIG_INT("filmroll.scr_std",   scr_std,          0);   /* screen colour: 0 Canon, 1 Rec.709 */
static CONFIG_INT("filmroll.scr_sat",   scr_sat,          0);   /* -6..6 */
static CONFIG_INT("filmroll.scr_warm",  scr_warm,         0);
static CONFIG_INT("filmroll.scr_bright", scr_bright,      0);
static CONFIG_INT("filmroll.scr_flip",  scr_flip,         0);   /* 0 Canon's hinge, 1 normal, 2 upside down, 3 mirrored */
static CONFIG_INT("filmroll.expsim",    expsim_set,      -1);   /* LiveView's exposure simulation: 1 on, 0 off, -1 as Canon has it */

/* roll state, kept across power cycles */
static CONFIG_INT("filmroll.loaded",    roll_loaded,      0);
static CONFIG_INT("filmroll.roll",      roll_number,      0);
static CONFIG_INT("filmroll.roll_film", roll_film,        0);
static CONFIG_INT("filmroll.roll_exp",  roll_exposures,   36);
static CONFIG_INT("filmroll.shot",      frames_shot,      0);
static CONFIG_INT("filmroll.lastfile",  last_file_number, -1);
static CONFIG_INT("filmroll.logdir",    roll_folder,      0);
static CONFIG_INT("filmroll.needwind",  need_wind,        0);
static CONFIG_INT("filmroll.run_shot",  run_shot,         0);   /* roll limit Off: frames shot on this film since it was chosen */

static char log_path[64];

static volatile int leds_until = 0;     /* keep LEDs lit until this ms clock */
static volatile int hud_dirty = 1;

#define ROLL_FINISHED (roll_loaded && frames_shot >= roll_exposures)
#define ROLL_IN_PROGRESS (roll_loaded && frames_shot < roll_exposures)

/* Roll limit Off (the default): no rolls for the person shooting. The camera is always loaded, with a roll
 * of FOS_SEGMENT frames behind the screens; at its last frame it closes the log and starts the next one,
 * same film, without a word. On the card that is back-to-back rolls of 36, as Basic mode makes them, so the
 * darkroom, the contact sheets and Darkrooms read them as they always did. */
#define FREE_ROLLS (roll_limit == 0)

static volatile int end_due = 0;        /* the roll was just finished: ms when to ask about it */
static volatile int dev_busy = 0;       /* developing: keep the camera awake */

/* the last frames, for the roll page (also read back from the log) */
static struct { int no; char time[6]; char fstop[6]; } recent[FOS_RECENT];
static int nrecent = 0;
static char loaded_at[20];
static int recent_valid = 0;
static int last_shot_ms = -100000;      /* when poll_frames last saw a new frame */
static volatile int last_key_ms = 0;    /* when a button other than the shutter was last pressed (handle_filmroll_keys) */

/* ---------------------------------------------------------------- debug log */

/* field-test trace in ML/FILMROLL.LOG; turn off in System once things work */
static void dbg(const char *fmt, ...)
{
    if (!debug_log)
        return;

    char buf[192];
    int n = snprintf(buf, sizeof(buf), "%7d  ", get_ms_clock());
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
    va_end(ap);

    FILE *f = FIO_CreateFileOrAppend("ML/FILMROLL.LOG");
    if (!f)
        return;
    my_fprintf(f, "%s\n", buf);
    FIO_CloseFile(f);
}

/* The camera waiting for a picture, the screen the film OS opens from.
 * On the EOS M that is LiveView; a DSLR (100D, 650D, 700D ...) is mostly
 * shot through its optical viewfinder, so there it is also Canon's shooting
 * screen with LiveView off. */
#ifdef CONFIG_EOSM
#define FOS_LV_ONLY 1
#else
#define FOS_LV_ONLY 0
#endif
static int shooting_screen()
{
    return (lv || !FOS_LV_ONLY) && display_idle() && !is_movie_mode();
}

#define DBG_STATE(what) dbg("%s: ml=%d lv=%d idle=%d movie=%d mode=%d gui=%d", what, \
                            ml_started, lv, display_idle(), is_movie_mode(), shooting_mode, CURRENT_GUI_MODE)

/* ---------------------------------------------------------------- log file */

static void update_log_path()
{
    snprintf(log_path, sizeof(log_path), "%s:/DCIM/%03d%s/ROLL_%03d.TXT",
             get_shooting_card()->drive_letter, roll_folder, get_dcim_dir_suffix(),
             roll_number);
}

static void log_append(const char *text)
{
    /* At start-up, filmroll_init runs before the settings are loaded (it logs
     * "roll 0 loaded 0"), so the path wasn't set, and frames shot after a
     * restart before any film OS page was opened had nowhere to go (#17). */
    if (!log_path[0] && roll_loaded)
        update_log_path();
    FILE *f = FIO_CreateFileOrAppend(log_path);
    if (!f)
    {
        NotifyBox(3000, "filmroll: can't write %s", log_path);
        return;
    }
    my_fprintf(f, "%s", text);
    FIO_CloseFile(f);
}

static void format_now(char *buf, int size)
{
    struct tm now;
    LoadCalendarFromRTC(&now);
    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d",
             now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
             now.tm_hour, now.tm_min, now.tm_sec);
}

static void write_header(const char *note)
{
    char when[32];
    char header[320];
    format_now(when, sizeof(when));
    snprintf(header, sizeof(header),
             "# ML film mode roll log (filmroll module)%s\n"
             "roll = %03d\nfilm = %s\nexposures = %d\nloaded = %s\n\n",
             note, roll_number, fos_films[COERCE(roll_film, 0, fos_nfilms - 1)].key, roll_exposures, when);
    log_append(header);
}

/* the roll page wants the loading time and the last frames: after a power
 * cycle they only exist in the log, so read them back once */
static void read_back_log()
{
    recent_valid = 1;
    nrecent = 0;
    loaded_at[0] = 0;
    int size = 0;
    char *buf = (char *) read_entire_file(log_path, &size);
    if (!buf)
        return;
    for (char *line = buf; line && *line; )
    {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strncmp(line, "loaded = ", 9))
            fos_copy(loaded_at, sizeof(loaded_at), line + 9, 16);
        else if (line[0] >= '0' && line[0] <= '9')
        {
            /* "12  IMG_0012.CR2  2026-08-08 09:15:15  f/2.8" */
            char *tok[5];
            int n = 0;
            for (char *p = line; *p && n < 5; )
            {
                while (*p == ' ') p++;
                if (!*p) break;
                tok[n++] = p;
                while (*p && *p != ' ') p++;
                if (*p) *p++ = 0;
            }
            if (n >= 4)
            {
                /* newest first, keep FOS_RECENT */
                for (int i = MIN(nrecent, FOS_RECENT - 1); i > 0; i--)
                    recent[i] = recent[i - 1];
                recent[0].no = atoi(tok[0]);
                fos_copy(recent[0].time, sizeof(recent[0].time), tok[3], 5);
                snprintf(recent[0].fstop, sizeof(recent[0].fstop), "%s",
                         n >= 5 && !strncmp(tok[4], "f/", 2) ? tok[4] + 2 : "?");
                nrecent = MIN(nrecent + 1, FOS_RECENT);
            }
        }
        line = next;
    }
    fio_free(buf);

    /* no rolls: the numbers on screen go on over the hidden rolls, and the log's own restart at 1 */
    if (FREE_ROLLS && run_shot > frames_shot)
        for (int i = 0; i < nrecent; i++)
            recent[i].no += run_shot - frames_shot;
}

/* ---------------------------------------------------------------- camera settings */

static int iso_to_raw(int iso)
{
    int raw = 72;                       /* ISO 100 */
    for (int i = 100; i < iso; i *= 2)
        raw += 8;                       /* one stop */
    return raw;
}

static void apply_film(int index)
{
    const struct fos_film *f = &fos_films[COERCE(index, 0, fos_nfilms - 1)];

    int ps = f->picstyle;
    prop_request_change_wait(PROP_PICTURE_STYLE, &ps, 4, 1000);
    msleep(100);

    /* these change the current picture style, so it must be selected first */
    lens_set_contrast(f->contrast);
    msleep(50);
    lens_set_sharpness(f->sharpness);
    msleep(50);
    if (f->picstyle != PS_MONO)
    {
        lens_set_saturation(f->saturation);
        msleep(50);
        lens_set_color_tone(f->color_tone);
        msleep(50);
    }

    lens_set_kelvin(f->kelvin);
    lens_set_wbs_gm(f->wbs_gm);
    lens_set_wbs_ba(f->wbs_ba);
    lens_set_rawiso(iso_to_raw(f->iso));
}

/* Exposure simulation (Viewfinder > Exposure simulation, #66). On, LiveView
 * shows the exposure the frame will get: at a slow film's ISO (loading a roll
 * sets it) and with a manual lens stopped down, too dark to focus. Off, Canon
 * shows LiveView at a brightness you can focus by, whatever the exposure. It
 * is Canon's own setting (PROP_LIVE_VIEW_VIEWTYPE), which Magic Lantern's
 * ExpSim item sets; that menu is gone (see the top), so it is here. Until it
 * is set here Canon's is left as it is; once it is, it goes back in when it
 * changes, when LiveView starts and after a roll is loaded. The HUD's meter
 * is Canon's reading of the exposure (get_ae_value), not of the picture on
 * the screen; that Canon keeps metering with it off wants checking on a
 * camera (the log has the reading each time it is set). Magic Lantern can't
 * set it on the 600D and 1100D (no CONFIG_EXPSIM): there the item isn't shown. */
static volatile int expsim_due = 0;     /* set it at the next chance */

static int expsim_get()
{
#ifdef CONFIG_EXPSIM
    return expsim_set >= 0 ? expsim_set : get_expsim() != 0;
#else
    return -1;
#endif
}

/* called from the main loop */
static void expsim_step()
{
#ifdef CONFIG_EXPSIM
    static int was_lv = 0, tries = 0, wait_until = 0;
    int now = get_ms_clock();
    if (lv && !was_lv)
    {
        expsim_due = 1;
        wait_until = now + 1000;        /* LiveView started: once it has settled */
    }
    was_lv = lv;
    if (!expsim_due) return;
    if (expsim_set < 0)
    {
        expsim_due = 0;
        return;
    }
    /* photo LiveView only (the 5D Mark II's movie display is its own), between pictures,
     * and not on a half-press: not while Canon focuses and meters (a roll just loaded,
     * the first thing done is often to half-press) */
    if (!lv || is_movie_mode() || get_halfshutter_pressed() || !job_state_ready_to_take_pic() || now - wait_until < 0)
        return;
    int want = expsim_set ? 1 : 0;
    if (get_expsim() != want && tries < 5)
    {
        tries++;
        set_expsim(want);
        dbg("expsim: %s, Canon's now %d (try %d), meter %d", want ? "on" : "off", get_expsim(), tries, get_ae_value());
        if (get_expsim() != want)
        {
            wait_until = get_ms_clock() + 1000;
            return;
        }
    }
    expsim_due = 0;
    tries = 0;
#endif
}

/* each frame is saved with a full-size RAW (RAW, or RAW+JPEG), so the
 * darkroom can develop it; a JPEG, mRAW or sRAW alone it can't */
static int saves_raw()
{
    return (pic_quality & 0x60000) && !(pic_quality & 0xF);     /* raw bits, raw size 0 */
}

/* Film is shot blind: the darkroom develops the RAWs, and there is no
 * image review to peek at. ML doesn't change these Canon settings itself
 * (writing the picture quality property can upset Canon's firmware), so
 * it asks for them. RAW+JPEG is fine: the darkroom uses the RAW. */
static void film_capture_check()
{
    if (!roll_loaded) return;
    int raw = saves_raw();
    int review_off = image_review_time == 0;
    if (raw && review_off) return;
    dbg("capture: quality %x, review %d", pic_quality, image_review_time);
    NotifyBox(6000, "Film: in Canon's menu set %s%s%s", raw ? "" : "Image quality RAW",
              !raw && !review_off ? " and " : "", review_off ? "" : "Image review Off");
}

/* ---------------------------------------------------------------- aperture */

/* manual (adapted) lenses report no aperture; the user dials it in instead */
static int lens_is_manual()
{
    return !lens_info.raw_aperture;
}

/* index into fos_fstops for the aperture in use */
static int current_fstop()
{
    if (lens_is_manual())
        return COERCE(manual_fstop, lens_widest, fos_nfstops - 1);

    int f100 = lens_info.aperture * 10;         /* lens_info.aperture is f x10 */
    int best = 0;
    for (int i = 1; i < fos_nfstops; i++)
        if (ABS(fos_fstops[i] - f100) < ABS(fos_fstops[best] - f100))
            best = i;
    return best;
}

static void step_manual_fstop(int dir)
{
    manual_fstop = COERCE(current_fstop() + dir, lens_widest, fos_nfstops - 1);
}

/* ---------------------------------------------------------------- rolls */

/* start a roll: state and log now, camera settings once the film OS has
 * let go of the screen (apply_film blocks for a while). continued_from is the
 * number of the roll this one follows by itself (no rolls: the last one just
 * filled); 0 for a roll that is loaded. */
static void load_roll(int film, int exposures, int continued_from)
{
    roll_film = COERCE(film, 0, fos_nfilms - 1);
    roll_exposures = exposures;
    roll_folder = get_shooting_card()->folder_number;

    /* next roll number whose log doesn't exist yet, so a reset counter
     * (new settings file, card from another camera) never mixes two rolls */
    for (int tries = 0; tries < 999; tries++)
    {
        roll_number = roll_number % 999 + 1;
        update_log_path();
        if (!is_file(log_path))
            break;
    }
    frames_shot = 0;
    need_wind = 0;
    /* a roll that follows another by itself is started while frames may still be
     * coming in (poll_frames is in the middle of a batch): the file number it
     * counts from must stay, or a frame Canon wrote meanwhile would be skipped */
    if (continued_from <= 0)
        last_file_number = get_shooting_card()->file_number;
    roll_loaded = 1;
    update_log_path();
    if (continued_from > 0)
    {
        char note[48];
        snprintf(note, sizeof(note), " - auto-continued from roll %03d", continued_from);
        write_header(note);
    }
    else
        write_header("");

    if (continued_from <= 0)
    {
        char when[32];
        format_now(when, sizeof(when));
        fos_copy(loaded_at, sizeof(loaded_at), when, 16);
        nrecent = 0;
        recent_valid = 1;
    }

    hud_dirty = 1;
    dbg("roll %03d loaded: %s, %d exp%s", roll_number, fos_films[COERCE(roll_film, 0, fos_nfilms - 1)].key, roll_exposures,
        continued_from > 0 ? " (by itself)" : "");
}

/* the line that ends a roll's log (Darkrooms reads it, so it stays as it was) */
static void close_log()
{
    char when[32], line[80];
    format_now(when, sizeof(when));
    snprintf(line, sizeof(line), "\n# rewound %s after %d of %d frames\n", when, frames_shot, roll_exposures);
    log_append(line);
}

static void rewind_roll()
{
    if (!roll_loaded) return;
    close_log();
    roll_loaded = 0;
    need_wind = 0;
    hud_dirty = 1;
    dbg("roll %03d rewound at %d/%d", roll_number, frames_shot, roll_exposures);
}

/* no rolls: the roll the camera keeps ends without a mark. One with nothing shot on it loses its log,
 * so choosing films leaves behind only the rolls that have frames. The caller loads the next one. */
static void end_roll_quietly()
{
    if (!roll_loaded) return;
    if (frames_shot == 0)
    {
        if (!log_path[0])
            update_log_path();
        FIO_RemoveFile(log_path);
        dbg("roll %03d: nothing shot, log removed", roll_number);
    }
    else
    {
        close_log();
        dbg("roll %03d closed at %d/%d", roll_number, frames_shot, roll_exposures);
    }
}

static void record_frame(int file_number)
{
    if (frames_shot >= roll_exposures)
    {
        beep();
        return;
    }

    frames_shot++;
    if (FREE_ROLLS)
        run_shot++;

    char when[32];
    char aperture[8];
    char line[112];
    format_now(when, sizeof(when));
    fos_format_fstop(aperture, sizeof(aperture), current_fstop());
    snprintf(line, sizeof(line), "%d  %s%04d.CR2  %s  f/%s\n",
             frames_shot, get_file_prefix(), file_number, when, aperture);
    log_append(line);

    /* the warning at loading is easy to miss, and then a whole roll can't
     * be developed: say it with each frame */
    if (!saves_raw())
    {
        dbg("frame %d: quality %x, no RAW", frames_shot, pic_quality);
        beep();
        NotifyBox(5000, "Frame %d is a JPEG: the darkroom can't develop it. Set Image quality RAW", frames_shot);
    }

    for (int i = MIN(nrecent, FOS_RECENT - 1); i > 0; i--)
        recent[i] = recent[i - 1];
    recent[0].no = FREE_ROLLS ? run_shot : frames_shot;      /* no rolls: the count goes on over the hidden rolls */
    fos_copy(recent[0].time, sizeof(recent[0].time), when + 11, 5);
    snprintf(recent[0].fstop, sizeof(recent[0].fstop), "%s", aperture);
    nrecent = MIN(nrecent + 1, FOS_RECENT);

    leds_until = get_ms_clock() + 2500;
    hud_dirty = 1;

    if (frames_shot == roll_exposures)
    {
        if (FREE_ROLLS)
        {
            /* no rolls: this one ends here without a beep or a question, and the next starts at once,
             * same film. Camera settings stay as they are (applying them takes a while) */
            int last = roll_number;
            close_log();
            load_roll(roll_film, FOS_SEGMENT, last);
            if (wind_mode)
                need_wind = 1;
            return;
        }
        need_wind = 0;
        beep_times(2);
        end_due = get_ms_clock() + 1500;        /* then ask: develop it now? */
        return;
    }

    if (wind_mode)
        need_wind = 1;
}

/* log every new file number since the last poll */
static void poll_frames()
{
    if (!roll_loaded)
        return;

    int now = get_shooting_card()->file_number;
    if (now == last_file_number)
        return;

    int prev = last_file_number;
    last_file_number = now;

    int count = now - prev;
    if (count < 0)
        count += 9999;                  /* IMG_9999 -> IMG_0001 */
    if (prev < 0 || count > 50)
        return;                         /* card swap or counter reset: just resync */

    /* Canon started a new folder; start a log there too so each folder
     * develops on its own (frame numbers keep counting). It does that when
     * the file number goes past 9999, so the files up to IMG_9999 of this
     * batch are still in the old folder, and go in its log (#17). */
    int folder = get_shooting_card()->folder_number;
    int first_new = folder == roll_folder ? count + 1   /* none */
                  : now < prev ? 9999 - prev + 1        /* the first after IMG_9999 */
                  : 1;

    last_shot_ms = get_ms_clock();
    dbg("frames: file %d -> %d, logging %d", prev, now, count);
    for (int i = 1; i <= count; i++)
    {
        /* (not when a roll that followed another by itself in this batch was already started in the new folder) */
        if (i == first_new && roll_folder != folder)
        {
            roll_folder = folder;
            update_log_path();
            write_header(" - continued from the previous DCIM folder");
        }
        record_frame((prev + i - 1) % 9999 + 1);
    }
}

/* ---------------------------------------------------------------- the film OS host */

static volatile int ui_running = 0;     /* the film OS owns the screen and the buttons */
static volatile int ui_abort = 0;
#if !FOS_LV_ONLY
static volatile int canon_handover = 0; /* the film OS closed with Canon's screen still held, for the HUD or the plate (hud_step) */
#endif
/* the bitmap buffer on the screen as we last drew in it (vram_moved); 0 once
 * Canon has its screen back, when its own flips are none of ours */
static const uint8_t *vram_shown = 0;
static int ui_after = 0;                /* what to do once it has let go */
static int ui_last_mode_poke = 0;
static int ui_last_palette = 0;

enum { AFTER_APPLY = 1, AFTER_CANON_MENU = 2 };

/* buttons, from the GUI task to the film OS task */
#define KEYQ 32
static volatile int keyq[KEYQ];
static volatile int keyq_in = 0, keyq_out = 0;

static void keyq_push(int k)
{
    int next = (keyq_in + 1) % KEYQ;
    if (next != keyq_out)
    {
        keyq[keyq_in] = k;
        keyq_in = next;
    }
}

/* Our palette is fos_pal (RGB); the bitmap overlay wants Y, Cb, Cr and
 * an opacity code, which we copy from Canon's own white. One entry is
 * remembered so we notice when other code puts Canon's palette back. */
#define PALETTE_PROBE 200
static uint32_t palette_probe = 0;

/* a photograph on the screen: its colours in 1..photo_n, at photo_k/256 */
static int photo_mode = 0, photo_n = 0, photo_k = 256;
static uint8_t photo_pal[FOS_PRINT_COLOURS][3];

static void palette_ours()
{
    if (!DISPLAY_IS_ON) return;
    /* Canon's white tells the opacity and where white sits in luma
     * (its own conversion puts it at 235, not 255) */
    uint32_t white = LCD_Palette[3 * COLOR_WHITE + 2];
    uint32_t opaque = white & 0xFF000000;
    if ((opaque >> 24) < 2) opaque = 0x03000000;
    int white_y = (white >> 16) & 0xFF;
    if (white_y < 180) white_y = 235;
    for (int i = 1; i < 255; i++)
    {
        uint32_t entry;
        if (!photo_mode)
            entry = opaque | fos_screen_yuv(i, white_y, scr_std, scr_sat, scr_warm, scr_bright);
        else
        {
            const uint8_t *c = i <= photo_n ? photo_pal[i - 1] :
                               i >= FOS_FIXED_BASE && i < FOS_FIXED_BASE + FOS_FIXED_N ? fos_fixed_rgb[i - FOS_FIXED_BASE] : 0;
            uint32_t rgb = c ? (uint32_t) (c[0] * photo_k >> 8) << 16 | (uint32_t) (c[1] * photo_k >> 8) << 8 | (uint32_t) (c[2] * photo_k >> 8) : 0;
            entry = opaque | fos_screen_yuv_rgb(rgb, white_y, scr_std, scr_sat, scr_warm, scr_bright);
        }
        if (i == PALETTE_PROBE) palette_probe = entry;
        EngDrvOut(LCD_Palette[3 * i], entry);
        EngDrvOut(LCD_Palette[3 * i + 0x300], entry);
    }
}

static void palette_canon()
{
    for (int i = 1; i < 255; i++)
    {
        EngDrvOut(LCD_Palette[3 * i], LCD_Palette[3 * i + 2]);
        EngDrvOut(LCD_Palette[3 * i + 0x300], LCD_Palette[3 * i + 2]);
    }
}

/* Take the display from Canon's GUI the same way the ML menu does:
 * switch Canon to the ML menu GUI mode (LiveView stops drawing its
 * overlays and stops handling keys), stop Canon from flipping its own
 * bitmap buffer, then turn the bitmap layer back on for us. Its buffer is
 * held from before the switch, and again while it lands: outside LiveView
 * that mode is Canon's own menu, which showed for a moment as the film OS
 * opened (#19), and opening it can let go of the hold. */
static void film_ui_open()
{
    NotifyBoxHide();
    if (lv) bmp_off();
    canon_gui_disable_front_buffer();
    SetGUIRequestMode(GUIMODE_ML_MENU);
    for (int i = 0; i < 10; i++)
    {
        msleep(20);
        canon_gui_disable_front_buffer();       /* nothing to do while it is still held */
    }
    CancelDateTimer();
    clrscr();
    bmp_on();
    ui_last_mode_poke = get_ms_clock();
}

static void film_ui_close()
{
    /* a page left at once (the shutter button, a Canon dialog) is still on
     * the screen: off it before Canon's palette comes back, or it shows in
     * Canon's colours while the GUI mode changes (#19). In LiveView bmp_off
     * hides it. */
    if (!lv) clrscr();
    palette_canon();
    if (lv) bmp_off();
    SetGUIRequestMode(0);
    msleep(100);
#if !FOS_LV_ONLY
    /* back to the HUD or the plate: Canon's screen stays held, so it doesn't
     * show between ours (#19, #64). hud_step gives it back if ours doesn't
     * come; Canon's menu (AFTER_CANON_MENU) gets it now. */
    canon_handover = 0;
    if (hud_enabled && roll_loaded && DISPLAY_IS_ON && !is_movie_mode() && !(ui_after & AFTER_CANON_MENU))
    {
        canon_gui_disable_front_buffer();
        clrscr();
        canon_handover = 1;
        if (lv) bmp_on();
        return;
    }
#endif
    vram_shown = 0;
    canon_gui_enable_front_buffer(0);
    clrscr();
    redraw();
    if (lv) bmp_on();
}

/* frame times are kept while the film OS runs and logged when it closes:
 * writing the card in the middle of an animation would stall it */
static struct fos_stat stat_all;

static void ui_task(int open)
{
    keyq_in = keyq_out = 0;
    ui_abort = 0;
    ui_after = 0;
    film_ui_open();
    DBG_STATE("film os: open");
    memset(&stat_all, 0, sizeof(stat_all));

    fos_ui_run(open);

    film_ui_close();
    ui_running = 0;
    hud_dirty = 1;
    DBG_STATE("film os: closed");
    if (stat_all.frames)
    {
        struct fos_stat *s = &stat_all;
        int n = s->frames, total = s->draw_ms + s->present_ms + s->rest_ms;
        dbg("film os: %d frames in %d ms = %d/s; per frame %d ms draw (%d canisters) + %d present + %d rest, worst %d, open %d ms, quality %d",
            n, total, total ? n * 1000 / total : 0, s->draw_ms / n, s->cans_ms / n, s->present_ms / n, s->rest_ms / n,
            s->worst_ms, s->open_ms, fos_can_quality);
    }

    if (ui_after & AFTER_APPLY)
    {
        apply_film(roll_film);
        expsim_due = 1;
        beep();
        film_capture_check();
    }
    if (dev_busy)
    {
        /* left in the middle of developing */
        dev_busy = 0;
        powersave_permit();
    }
    if (ui_after & AFTER_CANON_MENU)
        fake_simple_button(BGMT_MENU);
}

static void open_ui(int screen)
{
    if (ui_running)
        return;
    if (!fos_fonts_ready())
    {
        NotifyBox(5000, "Film OS: ML/DATA/" FOS_FONT_FILE " is missing");
        return;
    }
    ui_running = 1;
    /* the same priority as ML's own menu redraw: Canon's tasks still come
     * first, and the film OS rests between frames */
    task_create("film_os", 0x1a, 0x4000, ui_task, (void *) screen);
}

/* ---- what the film OS asks of the camera (see fos.h) */

int fos_port_ms()
{
    return get_ms_clock();
}

static void ui_overdrawn_check(int at_once);

void fos_port_sleep(int ms)
{
    msleep(ms);
    int now = get_ms_clock();

    /* the Canon dialog under us times out unless poked (CONFIG_MENU_TIMEOUT_FIX) */
    if (lv && now - ui_last_mode_poke > 2000)
    {
        SetGUIRequestMode(GUIMODE_ML_MENU);
        ui_last_mode_poke = now;
    }
    /* other code (ML's powersave, clear overlays) may put Canon's palette
     * back: check one entry every frame, rewrite all of them now and then */
    static int last_wake = 0;
    if (dev_busy && now - last_wake > 5000)
    {
        idle_wakeup_reset_counters(-10);
        last_wake = now;
    }
    uint32_t probe = shamem_read(LCD_Palette[3 * PALETTE_PROBE]);
    if ((probe && probe != palette_probe) || now - ui_last_palette > 1000)
    {
        palette_ours();
        ui_last_palette = now;
    }
    /* Canon can let go of the hold on its screen by itself (a dialog of its
     * own changing under us): take it again, and look for what it drew now */
    int let_go = !canon_gui_front_buffer_disabled();
    if (let_go)
    {
        static int times = 0;
        canon_gui_disable_front_buffer();
        if (times++ % 50 == 0) dbg("film os: Canon took its screen back, held again (seen %d time%s so far)", times, times == 1 ? "" : "s");
    }
    ui_overdrawn_check(let_go);
}

int fos_port_key()
{
    if (keyq_out == keyq_in)
        return FK_NONE;
    int k = keyq[keyq_out];
    keyq_out = (keyq_out + 1) % KEYQ;
    return k;
}

int fos_port_abort()
{
    if (ui_abort || ml_shutdown_requested || !DISPLAY_IS_ON)
        return 1;
    if (get_halfshutter_pressed())
        return 1;
    /* a Canon dialog took over (low battery, card door...): give the mode
     * request time to land, then let go if Canon keeps something else up */
    static int mismatch_since = 0;
    int now = get_ms_clock();
    if (CURRENT_GUI_MODE == GUIMODE_ML_MENU)
        mismatch_since = 0;
    else if (!mismatch_since)
        mismatch_since = now;
    else if (now - mismatch_since > 1500)
    {
        dbg("film os: Canon GUI mode %d, leaving", CURRENT_GUI_MODE);
        mismatch_since = 0;
        return 1;
    }
    return 0;
}

/* Canon drawing over our screen. On some DSLRs Canon keeps drawing its own
 * screen straight onto the one we show, with its buffer held or not: over
 * the top plate on the 50D, 500D, 550D, 600D and 6D (#43 #44 #64 #17), over
 * the film OS on the 60D (its exposure scale, #25 #49), the 550D (its
 * battery icon, #50) and the 600D (#64). Each row we put on the screen
 * leaves a sum of what it holds (the film OS's rows as they're presented,
 * the plate's as they're copied: the two never show at once, so they share
 * it). Every 50 ms half the rows are read back; once a row no longer
 * matches, all of them are, and the rows Canon drew on are put back at
 * once, from what we drew (the film OS's canvas, the plate's rows). If it
 * never matches for 2 s (Canon drawing all the time, or a camera that
 * doesn't read back what we wrote), the check rests for 5 s, then 10, up to
 * a minute, so it can't keep painting without end. The log counts how many
 * times it found Canon's drawing and says on which rows, every 50th time. */
static uint16_t shown_sum[FOS_H];        /* 16 bits a row: 1 KB less at boot, and a 1 in 65,536 miss */

static uint16_t row_sum(const uint8_t *row)
{
    const uint32_t *w = (const uint32_t *) row;
    uint32_t a = 0, b = 0;
    for (int x = 0; x < FOS_W / 4; x += 2) { a += w[x]; b += a; }   /* every other 4 pixels */
    uint32_t h = a ^ (b << 1);
    return (uint16_t) (h ^ (h >> 16));
}

struct overdraw_check { const char *what; int half, in_a_row, rest, resting_until, times; };

/* 1 if the buffer on the screen isn't the one we last drew in: Canon put
 * its other one in front (its screen, not ours, until we draw it there).
 * Logged, so a tester's log tells that apart from Canon drawing over ours;
 * not after Canon had its screen back (vram_shown 0), when flipping its
 * buffers is its own business. */
static int vram_moved(const uint8_t *vram)
{
    static int times = 0;
    if (vram == vram_shown) return 0;
    if (vram_shown && times++ % 50 == 0)
        dbg("screen: Canon put its other buffer on the screen, %x -> %x (seen %d time%s so far)",
            (uint32_t) (uintptr_t) vram_shown, (uint32_t) (uintptr_t) vram, times, times == 1 ? "" : "s");
    vram_shown = vram;
    return 1;
}

/* how many rows of vram Canon has drawn over since their sums were taken:
 * the first and the last in *y0, *y1, each marked in stale (a bit a row)
 * if it is given */
static int overdrawn(struct overdraw_check *o, const uint8_t *vram, uint8_t *stale, int *y0, int *y1)
{
    int now = get_ms_clock();
    if (o->resting_until && now - o->resting_until < 0) return 0;
    o->resting_until = 0;
    o->half ^= 1;
    int y = o->half;
    for (; y < FOS_H; y += 2)
        if (row_sum(vram + y * BMPPITCH) != shown_sum[y])
            break;
    int n = 0;
    if (y < FOS_H)
        for (y = 0; y < FOS_H; y++)
            if (row_sum(vram + y * BMPPITCH) != shown_sum[y])
            {
                if (!n++) *y0 = y;
                *y1 = y;
                if (stale) stale[y >> 3] |= 1 << (y & 7);
            }
    if (!n)
    {
        o->in_a_row = 0;
        o->rest = 0;
        return 0;
    }
    if (++o->in_a_row > 40)
    {
        o->in_a_row = 0;
        o->rest = o->rest ? MIN(o->rest * 2, 60000) : 5000;
        o->resting_until = (now + o->rest) ? now + o->rest : 1;
        dbg("%s: drawn over for 2 s on end (rows %d-%d), not checking for %d s", o->what, *y0, *y1, o->rest / 1000);
        return 0;
    }
    if (o->times++ % 50 == 0)
        dbg("%s: Canon drew over rows %d-%d (%d of them), put back; seen %d time%s so far, logged every 50th",
            o->what, *y0, *y1, n, o->times, o->times == 1 ? "" : "s");
    return n;
}

static void ui_overdrawn_check(int at_once)
{
    static struct overdraw_check check = { .what = "film os" };
    static int last = 0;
    int now = get_ms_clock();
    if (now - last < 50 && !at_once) return;
    last = now;
    uint8_t *vram = bmp_vram_real();
    if (!vram || !DISPLAY_IS_ON) return;
    int y0, y1;
    if (vram_moved(vram))
        fos_ui_overdrawn(0, FOS_H);
    else if (overdrawn(&check, vram, 0, &y0, &y1))
        fos_ui_overdrawn(y0, y1 + 1);
}

void fos_port_present(const uint8_t *rows, int pitch, int y0, int y1)
{
    uint8_t *vram = bmp_vram_real();
    if (!vram || !DISPLAY_IS_ON)
        return;
    y0 = COERCE(y0, 0, FOS_H);
    y1 = COERCE(y1, y0, FOS_H);
    for (int y = y0; y < y1; y++)
    {
        memcpy(vram + y * BMPPITCH, rows + (y - y0) * pitch, FOS_W);
        shown_sum[y] = row_sum(vram + y * BMPPITCH);
    }
}

void fos_port_palette()
{
    /* once: Canon's own entries, to check the colour conversion against */
    static int logged = 0;
    if (!logged && debug_log)
    {
        logged = 1;
        dbg("canon palette 1-7: %08x %08x %08x %08x %08x %08x %08x",
            LCD_Palette[3 * 1 + 2], LCD_Palette[3 * 2 + 2], LCD_Palette[3 * 3 + 2], LCD_Palette[3 * 4 + 2],
            LCD_Palette[3 * 5 + 2], LCD_Palette[3 * 6 + 2], LCD_Palette[3 * 7 + 2]);
        dbg("canon palette 8-15: %08x %08x %08x %08x %08x %08x %08x %08x",
            LCD_Palette[3 * 8 + 2], LCD_Palette[3 * 9 + 2], LCD_Palette[3 * 10 + 2], LCD_Palette[3 * 11 + 2],
            LCD_Palette[3 * 12 + 2], LCD_Palette[3 * 13 + 2], LCD_Palette[3 * 14 + 2], LCD_Palette[3 * 15 + 2]);
    }
    photo_mode = 0;
    palette_ours();
    ui_last_palette = get_ms_clock();
}

void fos_port_photo(const uint8_t (*pal)[3], int n, int k)
{
    photo_n = COERCE(n, 0, FOS_PRINT_COLOURS);
    memcpy(photo_pal, pal, photo_n * 3);
    photo_k = COERCE(k, 0, 256);
    photo_mode = 1;
    palette_ours();
    ui_last_palette = get_ms_clock();
}

void fos_port_log(const char *msg)
{
    dbg("%s", msg);
}

int fos_port_dev_cap()
{
    return 0;
}

/* ---- the card */

const char *fos_port_card()
{
    static char root[8];
    snprintf(root, sizeof(root), "%s:/", get_shooting_card()->drive_letter);
    return root;
}

void *fos_port_open(const char *path, int write)
{
    return write ? FIO_CreateFile(path) : FIO_OpenFile(path, O_RDONLY | O_SYNC);
}

/* Canon reads files by DMA: into memory the CPU caches, it goes through
 * an uncached buffer. Taken at start (card_buffer), while there is memory
 * for it: on a 5D Mark II, asked for only when the darkroom first read a
 * roll's log, it wasn't there, and every read came back empty (#10). */
#define BOUNCE (64 * 1024)
static uint8_t *bounce = 0;
static int bounce_size = 0;

static int card_buffer()
{
    for (int size = BOUNCE; !bounce && size >= 4096; size /= 4)
    {
        bounce = fio_malloc(size);
        if (bounce) bounce_size = size;
        else dbg("read: no %d-byte buffer for card reads", size);
    }
    return bounce != 0;
}

int fos_port_read(void *f, void *buf, int size)
{
    if (buf != CACHEABLE(buf))
        return FIO_ReadFile(f, buf, size);
    if (!card_buffer()) return -1;
    int done = 0;
    while (done < size)
    {
        int n = MIN(size - done, bounce_size);
        int got = FIO_ReadFile(f, UNCACHEABLE(bounce), n);
        if (got <= 0) break;
        memcpy((uint8_t *) buf + done, UNCACHEABLE(bounce), got);
        done += got;
        if (got < n) break;
    }
    return done;
}

int fos_port_write(void *f, const void *buf, int size)
{
    return FIO_WriteFile(f, buf, size);
}

int fos_port_seek(void *f, int offset)
{
    return FIO_SeekSkipFile(f, offset, SEEK_SET) == offset ? 0 : -1;
}

void fos_port_close(void *f)
{
    if (f) FIO_CloseFile(f);
}

int fos_port_list(const char *dir, int (*fn)(void *ctx, const char *name, int is_dir), void *ctx)
{
    char path[FIO_MAX_PATH_LENGTH];
    int len = snprintf(path, sizeof(path), "%s", dir);
    if (len > 0 && len < (int) sizeof(path) - 1 && path[len - 1] != '/') strcat(path, "/");
    struct fio_file file;
    struct fio_dirent *d = FIO_FindFirstEx(path, &file);
    if (IS_ERROR(d)) return -1;
    int n = 0;
    do
    {
        if (file.name[0] == '.') continue;
        n++;
        if (!fn(ctx, file.name, (file.mode & ATTR_DIRECTORY) != 0)) break;
    }
    while (FIO_FindNextEx(d, &file) == 0);
    FIO_FindClose(d);
    return n;
}

int fos_port_mkdir(const char *path)
{
    if (is_dir(path)) return 0;
    FIO_CreateDirectory(path);
    return is_dir(path) ? 0 : -1;
}

/* Canon's FIO_RemoveFile, for a file or an empty folder. Whether it takes a
 * folder on every camera isn't known, so what counts is that the path is
 * gone afterwards, as a folder and as a file. */
int fos_port_remove(const char *path)
{
    FIO_RemoveFile(path);
    if (is_dir(path)) return -1;
    FILE *f = FIO_OpenFile(path, O_RDONLY | O_SYNC);
    if (f) FIO_CloseFile(f);
    return f ? -1 : 0;
}

void *fos_port_alloc(int size)
{
    void *p = tmp_malloc(size);
    if (!p) dbg("alloc: %d bytes failed", size);
    return p;
}

void fos_port_free(void *p)
{
    if (p) free(p);
}

void fos_port_roll(struct fos_roll *r)
{
    if (roll_loaded && !recent_valid)
    {
        update_log_path();
        read_back_log();
    }
    memset(r, 0, sizeof(*r));
    r->loaded = roll_loaded;
    r->number = roll_number;
    r->film = COERCE(roll_film, 0, fos_nfilms - 1);
    r->exposures = roll_exposures;
    r->shot = frames_shot;
    r->need_wind = need_wind;
    r->no_rolls = FREE_ROLLS;
    r->total = run_shot;
    snprintf(r->loaded_at, sizeof(r->loaded_at), "%s", loaded_at);
    r->nrecent = nrecent;
    for (int i = 0; i < nrecent && i < FOS_RECENT; i++)
    {
        r->recent[i].no = recent[i].no;
        memcpy(r->recent[i].time, recent[i].time, sizeof(r->recent[i].time));
        memcpy(r->recent[i].fstop, recent[i].fstop, sizeof(r->recent[i].fstop));
    }
}

/* Screen orientation on the swivel screens (60D, 600D, 650D, 700D, 70D, #49).
 * Canon turns the whole screen, its own and ours, by the hinge's switch. A
 * broken switch leaves it the wrong way round, and Canon's menu has nothing
 * to set it by hand; ML's Display menu does it once, until the switch says
 * otherwise. With an orientation set here, Canon's NormalDisplay /
 * ReverseDisplay / MirrorDisplay put it back whenever the camera changes it
 * (Canon's 0 normal, 1 reverse, 2 mirror, as in ML's display_orientation_toggle).
 * Back on Auto, the screen goes back to the way Canon last turned it. */
#if defined(CONFIG_VARIANGLE_DISPLAY) && defined(DISPLAY_ORIENTATION)
#define CAN_FLIP 1
extern void NormalDisplay();
extern void ReverseDisplay();
extern void MirrorDisplay();

static void flip_to(int o)
{
    if (o == 0) NormalDisplay();
    else if (o == 1) ReverseDisplay();
    else MirrorDisplay();
}

static void flip_step(int now_please)
{
    static int canon_had = -1, we_set = -1, last = 0, times = 0;
    if (!DISPLAY_IS_ON) return;
    int o = DISPLAY_ORIENTATION;
    if (o < 0 || o > 2) return;
    if (!scr_flip)
    {
        /* back to Auto: the way Canon last turned it */
        if (canon_had >= 0 && o != canon_had) flip_to(canon_had);
        canon_had = we_set = -1;
        return;
    }
    int want = COERCE(scr_flip - 1, 0, 2);
    if (o == want) return;
    int now = get_ms_clock();
    if (!now_please && last && now - last < 300) return;   /* give Canon's change time to land */
    last = now ? now : 1;
    /* not the way we last set it: Canon turned it, and that is the hinge's
     * latest word (the first time, its word from before). Auto goes back to that */
    if (o != we_set) canon_had = o;
    if (times++ < 20) dbg("screen: orientation %d (Canon's %d), set %d", o, canon_had, want);
    we_set = want;
    flip_to(want);
}
#else
#define CAN_FLIP 0
static void flip_step(int now_please) { (void) now_please; }
#endif

static int *setting_var(int s)
{
    switch (s)
    {
        case FS_HUD:        return &hud_enabled;
        case FS_LEDS:       return &leds_on_half;
        case FS_FOCUS:      return &focus_screen;
        case FS_WIND:       return &wind_mode;
        case FS_APKEYS:     return &aperture_keys;
        case FS_LENS_WIDE:  return &lens_widest;
        case FS_FSTOP:      return &manual_fstop;
        case FS_STARTUP:    return &startup_picker;
        case FS_PLAYLOCK:   return &play_lock;
        case FS_BOOTANIM:   return &boot_anim;
        case FS_DEBUG:      return &debug_log;
        case FS_MODE:       return &film_mode;
        case FS_FILM:       return &film_sel;
        case FS_EXP:        return &exp_sel;
        case FS_SCR_STD:    return &scr_std;
        case FS_SCR_SAT:    return &scr_sat;
        case FS_SCR_WARM:   return &scr_warm;
        case FS_SCR_BRIGHT: return &scr_bright;
        case FS_SCR_FLIP:   return &scr_flip;
        case FS_EXPSIM:     return &expsim_set;
        case FS_ROLLLIM:    return &roll_limit;
    }
    return 0;
}

int fos_port_get(int s)
{
    int *v = setting_var(s);
    if (s == FS_FSTOP) return current_fstop();
    if (s == FS_SCR_FLIP && !CAN_FLIP) return -1;
    if (s == FS_EXPSIM) return expsim_get();
    return v ? *v : 0;
}

/* the roll limit changed: the roll in the camera ends (the log keeps what was shot on it), and with the
 * limit Off the camera is loaded again at once, with the film it had; a limit leaves it unloaded until a
 * roll is loaded on the shelf, as before */
static void roll_limit_changed()
{
    int film = roll_loaded ? roll_film : film_sel;
    end_roll_quietly();
    roll_loaded = 0;
    need_wind = 0;
    run_shot = 0;
    hud_dirty = 1;
    if (FREE_ROLLS)
        load_roll(film, FOS_SEGMENT, 0);
}

void fos_port_set(int s, int value)
{
    int *v = setting_var(s);
    int old = v ? *v : 0;
    if (v) *v = value;
    hud_dirty = 1;
    if (s == FS_ROLLLIM && value != old)
        roll_limit_changed();
    if (s == FS_SCR_FLIP) flip_step(1);
    if (s == FS_EXPSIM) expsim_due = 1;
    /* screen colour: the whole film OS changes at once */
    if (s == FS_SCR_STD || s == FS_SCR_SAT || s == FS_SCR_WARM || s == FS_SCR_BRIGHT)
    {
        palette_ours();
        ui_last_palette = get_ms_clock();
    }
}

void fos_port_action(int action, int a, int b)
{
    (void) b;                   /* the roll size is the roll limit's now, not the shelf's */
    switch (action)
    {
        case FA_LOAD:
            if (FREE_ROLLS)
            {
                /* no rolls: choosing a film changes the film. The one the camera has already changes
                 * nothing but the camera settings (the shelf opens on it at start-up) */
                if (!(roll_loaded && COERCE(a, 0, fos_nfilms - 1) == roll_film))
                {
                    end_roll_quietly();
                    run_shot = 0;
                    load_roll(a, FOS_SEGMENT, 0);
                }
            }
            else
            {
                /* a roll limit: the size is the setting's, not the shelf's */
                if (ROLL_IN_PROGRESS) rewind_roll();
                load_roll(a, fos_exposure_choices[COERCE(roll_limit - 1, 0, FOS_NEXP - 1)], 0);
            }
            ui_after |= AFTER_APPLY;
            break;
        case FA_REWIND:
        {
            /* (no rolls: the darkroom ends the roll to develop it; the camera is never without one) */
            int last = roll_number, film = roll_film;
            rewind_roll();
            if (FREE_ROLLS)
                load_roll(film, FOS_SEGMENT, last);
            break;
        }
        case FA_BUSY:
            if (a && !dev_busy) powersave_prohibit();
            if (!a && dev_busy) powersave_permit();
            dev_busy = a;
            dbg("darkroom: %s", a ? "developing" : "done");
            break;
        case FA_REAPPLY:
            if (roll_loaded) ui_after |= AFTER_APPLY;
            break;
        case FA_CANON_MENU:
            ui_after |= AFTER_CANON_MENU;
            break;
    }
}

void fos_port_clock(char *buf, int size)
{
    /* screens ask every frame; the RTC is read once a second */
    static char cached[8];
    static int last = -100000;
    int now_ms = get_ms_clock();
    if (now_ms - last > 1000)
    {
        struct tm now;
        LoadCalendarFromRTC(&now);
        snprintf(cached, sizeof(cached), "%02d:%02d", now.tm_hour, now.tm_min);
        last = now_ms;
    }
    snprintf(buf, size, "%s", cached);
}

int fos_port_lens_manual()
{
    return lens_is_manual();
}

const char *fos_port_lens_name()
{
    return lens_info.name[0] ? lens_info.name : "Lens";
}

/* M-Film's version (VERSION at the top of the repository; build_fork.sh writes fos_version.h) */
#if defined(__has_include)
#if __has_include("fos_version.h")
#include "fos_version.h"
#endif
#endif
#ifndef MFILM_VERSION
#define MFILM_VERSION "dev"
#endif

const char *fos_port_model()
{
    /* "Canon EOS 650D": as the camera reports itself */
    return camera_model[0] ? camera_model : "Canon camera";
}

const char *fos_port_version()
{
    return MFILM_VERSION;
}


void fos_port_stat(const struct fos_stat *s)
{
    stat_all.frames += s->frames;
    stat_all.draw_ms += s->draw_ms;
    stat_all.present_ms += s->present_ms;
    stat_all.rest_ms += s->rest_ms;
    stat_all.cans_ms += s->cans_ms;
    if (s->worst_ms > stat_all.worst_ms) stat_all.worst_ms = s->worst_ms;
    if (s->open_ms) stat_all.open_ms = s->open_ms;
}

/* ---------------------------------------------------------------- viewfinder HUD */

static volatile int hud_showing = 0;    /* HUD drawn on the LiveView screen */

static int hud_visible()
{
    return hud_enabled && roll_loaded && lv && display_idle() && !is_movie_mode() && !ui_running;
}

/* a DSLR shooting through the viewfinder: the back screen shows Canon's
 * settings display, and the top plate (fos_hud_plate) goes over it */
static int plate_visible()
{
    return !FOS_LV_ONLY && hud_enabled && roll_loaded && !lv && display_is_on() && display_idle()
        && !is_movie_mode() && !ui_running;
}

/* Magic Lantern's own LiveView overlays never draw: the film OS (or
 * Canon's clean screen) is all you see */
int filmroll_overlays_active()
{
    return 1;
}

/* the HUD draws with Canon's palette */
static struct fos_hud_palette hud_pal;

static void hud_palette_init()
{
    hud_pal.clear = COLOR_EMPTY;
    hud_pal.black = COLOR_BLACK;
    hud_pal.white = COLOR_WHITE;
    hud_pal.red = COLOR_RED;
    hud_pal.dark_red = COLOR_DARK_RED;
    for (int i = 0; i <= 100; i++)
        hud_pal.gray[i] = COLOR_GRAY(i);
}

/* the shutter speed as the LEDs show it: 1/125 is 125, slow ones keep the " */
static void shutter_led(char *buf, int size)
{
    if (!lens_info.raw_shutter)
    {
        snprintf(buf, size, "--");
        return;
    }
    const char *s = lens_format_shutter(lens_info.raw_shutter);
    int n = 0;
    for (; *s && n < size - 1; s++)
        if ((*s >= '0' && *s <= '9') || *s == '.' || *s == '"')
            buf[n++] = *s;
    buf[n] = 0;
}

static int hud_stale = 0;               /* some rows are stale, nothing else changed: copy just those */

#if !FOS_LV_ONLY
/* On a DSLR the HUD and the plate are put together off-screen, a band of
 * rows at a time, and only the rows that differ from what is on the screen
 * go to it. Drawn straight onto the screen, each redraw showed for a moment
 * what it starts with: the plate's black, then its digits, the HUD's strip
 * without its scale. The plate was redrawn so every 400 ms for its blink,
 * and on each of Canon's overdraws: the meter and the settings flickering
 * to black and back (#44, #64). Rows Canon drew over, or that were wiped,
 * are stale: they go to the screen whatever their sum. The EOS M draws its
 * HUD straight onto the screen as before: its boot memory is short
 * (MEMORY.md), and no flicker has been reported there. */
#define BAND 48                         /* rows at a time: 34 KB, from malloc the first time */
static uint8_t *band;
static uint32_t *put_sum;               /* each row as we last put it on the screen, all its pixels (in band's block) */
static uint8_t *stale;                  /* a bit a row (in band's block) */

static void hud_all_stale()
{
    if (stale) memset(stale, 0xFF, FOS_H / 8);
}

/* the pixels of a row in [x0, x1) pairs, to tell when it changed */
static uint32_t span_sum(const uint8_t *row, const int *x, int n)
{
    uint32_t a = 1, b = 0;
    for (int i = 0; i < n; i += 2)
    {
        int j = x[i], end = x[i + 1];
        for (; j < end && (j & 3); j++) { a += row[j]; b += a; }
        for (; j + 4 <= end; j += 4) { a += *(const uint32_t *) (row + j); b += a; }
        for (; j < end; j++) { a += row[j]; b += a; }
    }
    return a ^ (b << 16) ^ (b >> 16);
}

/* What Canon drew on LiveView while it had let go of its screen, wiped
 * around the HUD: its own parts stay as they are and go back from band
 * (all stale). clrscr() would show LiveView without the HUD until then,
 * and on the 50D that came ten times a second: in LiveView with Canon's
 * display not the clean one, ML's own kill-flicker gives Canon its screen
 * back every 100 ms (powersave.c), and hud_step takes it again. */
static void hud_wipe_around()
{
    uint8_t *vram = bmp_vram();
    if (!vram) return;
    if (!band) { clrscr(); return; }        /* drawn straight onto the screen anyway */
    for (int y = 0; y < FOS_H; y++)
    {
        int x[4], n = fos_hud_spans(y, x), at = 0;
        uint8_t *row = vram + y * BMPPITCH;
        for (int i = 0; i < n; i += 2)
        {
            if (x[i] > at) memset(row + at, hud_pal.clear, x[i] - at);
            at = x[i + 1];
        }
        if (at < FOS_W) memset(row + at, hud_pal.clear, FOS_W - at);
    }
}
#else
static void hud_all_stale() {}
#endif

/* The HUD (plate 0) or the plate onto the screen: the rows that changed and
 * the stale ones, or every row (all). only_stale: nothing on it changed, so
 * only the bands with stale rows need putting together. Returns how many
 * rows it copied, -1 if it drew straight onto the screen (no memory). */
static int hud_put(const struct fos_hud *h, int plate, int all, int only_stale)
{
    uint8_t *vram = bmp_vram();
    if (!vram) return 0;
#if FOS_LV_ONLY
    (void) plate; (void) all; (void) only_stale;
    fos_hud_draw(vram, BMPPITCH, h, &hud_pal);
    return -1;
#else
    static int no_band = 0;
    if (!band && !no_band)
    {
        band = malloc(BAND * FOS_W + FOS_H * sizeof(uint32_t) + FOS_H / 8);
        if (band)
        {
            put_sum = (uint32_t *) (band + BAND * FOS_W);
            stale = (uint8_t *) (put_sum + FOS_H);
            hud_all_stale();
        }
        else
        {
            no_band = 1;
            dbg("hud: no memory to put it together off-screen, drawing it straight onto the screen");
        }
    }
    if (!band)
    {
        if (plate) fos_hud_plate(vram, BMPPITCH, h, &hud_pal);
        else fos_hud_draw(vram, BMPPITCH, h, &hud_pal);
        return -1;
    }

    if (vram_moved(vram)) hud_all_stale();
    int copied = 0;
    for (int y0 = 0; y0 < FOS_H; y0 += BAND)
    {
        int n = MIN(BAND, FOS_H - y0);
        if (only_stale && !all)
        {
            int any = 0;
            for (int y = y0; y < y0 + n; y += 8) any |= stale[y >> 3];
            if (!any) continue;
        }
        memset(band, hud_pal.clear, n * FOS_W);
        if (plate) fos_hud_plate_rows(band, FOS_W, y0, n, h, &hud_pal);
        else fos_hud_draw_rows(band, FOS_W, y0, n, h, &hud_pal);
        for (int y = y0; y < y0 + n; y++)
        {
            int x[4] = { 0, FOS_W }, nx = plate ? 2 : fos_hud_spans(y, x);
            const uint8_t *src = band + (y - y0) * FOS_W;
            uint32_t sum = span_sum(src, x, nx);
            uint8_t bit = 1 << (y & 7);
            if (!all && !(stale[y >> 3] & bit) && sum == put_sum[y]) continue;
            uint8_t *dst = vram + y * BMPPITCH;
            for (int i = 0; i < nx; i += 2)
                memcpy(dst + x[i], src + x[i], x[i + 1] - x[i]);
            put_sum[y] = sum;
            stale[y >> 3] &= ~bit;
            if (plate) shown_sum[y] = row_sum(dst);     /* for the check, as it reads back */
            copied++;
        }
    }
    return copied;
#endif
}

#if !FOS_LV_ONLY
/* The top plate's watchdog (Canon drawing over our screen, above): checked
 * every 50 ms while the plate is up, from hud_step and the main loop. */
static volatile int plate_on_screen = 0;

static void plate_sums()
{
    uint8_t *vram = bmp_vram();
    if (!vram) return;
    for (int y = 0; y < FOS_H; y++)
        shown_sum[y] = row_sum(vram + y * BMPPITCH);
}

/* 1 if Canon drew over the plate: its rows are stale, for hud_step to put back */
static int plate_overdrawn()
{
    static struct overdraw_check check = { .what = "hud: the plate" };
    /* not up, or about to be drawn or put back anyway: the main loop's check
     * between hud_steps calls hud_step, whose own check would otherwise find
     * the same rows again, count them twice and read the screen twice */
    if (!plate_on_screen || hud_dirty || hud_stale) return 0;
    uint8_t *vram = bmp_vram();
    int y0, y1;
    if (!vram || !overdrawn(&check, vram, stale, &y0, &y1)) return 0;
    if (stale) hud_stale = 1;
    else hud_dirty = 1;                             /* drawn straight onto the screen: all of it again */
    return 1;
}
#endif

static void hud_draw(int lit, int blink, int plate, int all, int only_stale)
{
    struct fos_hud h;
    memset(&h, 0, sizeof(h));
    h.lit = lit;
    h.blink = blink;
    h.fstop = current_fstop();
    h.widest = lens_is_manual() ? lens_widest : 0;
    if (lit)
    {
        int ev8 = get_ae_value();               /* meter reading, 1/8 EV, + = too bright */
        h.meter = 1;
        h.want = h.fstop + (ev8 * 3 + (ev8 >= 0 ? 4 : -4)) / 8;
    }
    shutter_led(h.shutter, sizeof(h.shutter));
    h.frame = (FREE_ROLLS ? run_shot : frames_shot) + 1;       /* no rolls: a count that goes on over the hidden rolls */
    h.exposures = roll_exposures;
    h.finished = FREE_ROLLS ? 0 : ROLL_FINISHED;
    h.no_rolls = FREE_ROLLS;
    h.need_wind = need_wind;
    h.focus_screen = focus_screen;
    h.film = fos_films[COERCE(roll_film, 0, fos_nfilms - 1)].name;
    h.iso = fos_films[COERCE(roll_film, 0, fos_nfilms - 1)].box_iso;
    int put = hud_put(&h, plate, all, only_stale);
#if !FOS_LV_ONLY
    if (put < 0 && plate) plate_sums();
#else
    (void) put;
#endif
}

static void hud_clear()
{
    uint8_t *vram = bmp_vram();
    if (vram)
        fos_hud_clear(vram, BMPPITCH, &hud_pal);
    hud_all_stale();
}

/* one HUD refresh; called from the main loop */
static void hud_step()
{
    static int last_sig = -1;
    static int last_all = 0;
    static int drawn = 0;
    static int was_visible = -1;
    static int plate_drawn = 0;

    int plate = plate_visible();
    int visible = hud_visible() || plate;
    if (visible * 2 + plate != was_visible)
    {
        dbg("hud: visible %d plate %d (enabled %d roll %d lv %d movie %d ui %d)",
            visible, plate, hud_enabled, roll_loaded, lv, is_movie_mode(), ui_running);
        was_visible = visible * 2 + plate;
        hud_all_stale();                    /* someone else's screen was there */
    }
    hud_showing = visible;

#if !FOS_LV_ONLY
    /* On a DSLR Canon keeps drawing its own screen (the settings display, or
     * its LiveView overlays) and flipping it over ours: the two flicker in
     * turn, and a change of aperture or shutter brings Canon's back. While the
     * HUD or the plate is up, stop Canon flipping its buffer, as the ML menu
     * does, and wipe what it left. Canon gets the screen back as soon as ours
     * goes (a Canon menu, playback, the display off). The film OS's own pages
     * take the buffer themselves (film_ui_open), and leave it held for ours
     * when they close (film_ui_close). */
    /* Canon's screen comes back only once ours has been gone a moment: the
     * display isn't idle for a blink while a frame is taken, and giving the
     * screen back then (and taking it again) flashed Canon's screen, and its
     * picture of the frame, over ours (#17). The film OS's pages take the
     * screen at once, and so does anything that isn't a blink. */
    static int canon_held = 0, gone_since = 0;
    if (canon_handover && !ui_running)      /* once the film OS has gone, or the hold would be dropped below */
    {
        canon_handover = 0;
        canon_held = 1;
        dbg("hud: Canon's screen still held from the film OS");
    }
    if (visible)
    {
        gone_since = 0;
        if (!canon_held)
        {
            canon_gui_disable_front_buffer();
            if (!plate) clrscr();           /* the plate covers all of it */
            canon_held = 1;
            hud_all_stale();
            hud_dirty = 1;
            dbg("hud: Canon's screen held");
        }
        else if (!canon_gui_front_buffer_disabled())
        {
            /* Canon let go of the hold by itself, and would draw over ours
             * from now on (#64): take it again, and put all of ours back */
            static int times = 0;
            canon_gui_disable_front_buffer();
            if (!plate) hud_wipe_around();
            hud_all_stale();
            hud_stale = 1;
            if (times++ % 50 == 0) dbg("hud: Canon took its screen back, held again (seen %d time%s so far)", times, times == 1 ? "" : "s");
        }
    }
    else if (canon_held)
    {
        int now = get_ms_clock();
        if (!gone_since) gone_since = now ? now : 1;
        /* and while a frame is being taken and saved (job_state), or just was:
         * the screen comes back to ours by itself, so Canon's needn't show for
         * the second or so in between (#17) */
        int shooting = lens_info.job_state || get_halfshutter_pressed() || now - last_shot_ms < 2500;
        /* a button (Q, MENU, INFO, the dial...) pressed as ours went, or since: that's
         * Canon's screen being asked for, not a blink, so it gets it at once */
        int asked = last_key_ms && last_key_ms - gone_since > -200;
        if (ui_running || asked || (now - gone_since > 700 && !shooting) || now - gone_since > 4000)
        {
            canon_held = 0;
            gone_since = 0;
            if (!ui_running) { vram_shown = 0; canon_gui_enable_front_buffer(1); }
            dbg("hud: Canon's screen given back%s", ui_running ? " (to the film OS)" : asked ? " (a button)" : "");
        }
    }
#endif

    /* the plate off (LiveView on, or a Canon menu over it): hand the screen back */
    if (plate_drawn && !plate)
    {
#if !FOS_LV_ONLY
        uint8_t *vram = bmp_vram();
        if (vram && display_idle() && !ui_running) fos_hud_plate_clear(vram, BMPPITCH, &hud_pal);
#endif
        plate_drawn = 0;
#if !FOS_LV_ONLY
        plate_on_screen = 0;
#endif
        hud_all_stale();
        last_sig = -1;
    }

    if (!visible)
    {
        /* HUD switched off or roll rewound: wipe it once we're back on the LiveView screen */
        if (drawn && (!hud_enabled || !roll_loaded) && lv && display_idle() && !ui_running)
        {
            hud_clear();
            drawn = 0;
        }
        last_sig = -1;
        return;
    }
    drawn = 1;
#if !FOS_LV_ONLY
    plate_overdrawn();
#endif

    int now = get_ms_clock();
    int lit = !leds_on_half || get_halfshutter_pressed() || now < leds_until;
    int blink = (now / 400) % 2;
    int ev8 = lit ? get_ae_value() : 0;

    int sig = lit | blink << 1 | need_wind << 2 | ((frames_shot + run_shot) & 0xFF) << 3
            | lens_info.raw_shutter << 11 | (ev8 & 0xFF) << 19 | (current_fstop() & 0x1F) << 27;

    /* put together on change (the rows that changed go to the screen), the
     * stale rows put back, and every second all of it, to win over Canon's
     * overlays where the check doesn't see them */
    int changed = sig != last_sig || hud_dirty;
    int all = now - last_all > 1000;
    if (changed || all || hud_stale)
    {
        hud_dirty = 0;
        hud_stale = 0;
        last_sig = sig;
        if (all || FOS_LV_ONLY) last_all = now;     /* the EOS M draws all of it each time */
        hud_draw(lit, blink, plate, all, !changed);
        plate_drawn = plate;
#if !FOS_LV_ONLY
        plate_on_screen = plate;
#endif
    }
}

/* ---------------------------------------------------------------- keys */

/* while the film OS is up it gets the dial and the buttons; anything it
 * doesn't use goes on to Canon, like the ML menu does */
static int ui_key(struct event *event, int key)
{
    int dir = 0;
    switch (key)
    {
        case MODULE_KEY_PRESS_HALFSHUTTER:
        case MODULE_KEY_PRESS_FULLSHUTTER:
        case MODULE_KEY_REC:
            ui_abort = 1;               /* back to the camera, ready to shoot */
            return 1;
        case MODULE_KEY_UNPRESS_HALFSHUTTER:
        case MODULE_KEY_UNPRESS_FULLSHUTTER:
            return 1;                   /* Canon must see the shutter let go */

        case MODULE_KEY_WHEEL_LEFT:     dir = FK_WHEEL_L; break;
        case MODULE_KEY_WHEEL_RIGHT:    dir = FK_WHEEL_R; break;
        case MODULE_KEY_WHEEL_UP:       dir = FK_WHEEL_L; break;
        case MODULE_KEY_WHEEL_DOWN:     dir = FK_WHEEL_R; break;

        case MODULE_KEY_PRESS_LEFT:     keyq_push(FK_LEFT); return 0;
        case MODULE_KEY_PRESS_RIGHT:    keyq_push(FK_RIGHT); return 0;
        case MODULE_KEY_PRESS_UP:       keyq_push(FK_UP); return 0;
        case MODULE_KEY_PRESS_DOWN:     keyq_push(FK_DOWN); return 0;
        case MODULE_KEY_UNPRESS_UDLR:
            keyq_push(FK_RELEASE | FK_LEFT);
            keyq_push(FK_RELEASE | FK_RIGHT);
            keyq_push(FK_RELEASE | FK_UP);
            keyq_push(FK_RELEASE | FK_DOWN);
            return 0;

        case MODULE_KEY_PRESS_SET:
        case MODULE_KEY_Q:
        case MODULE_KEY_JOY_CENTER:     keyq_push(FK_SET); return 0;
        case MODULE_KEY_UNPRESS_SET:    return 0;
        case MODULE_KEY_MENU:           keyq_push(FK_MENU); return 0;
        case MODULE_KEY_INFO:           keyq_push(FK_INFO); return 0;
        case MODULE_KEY_PLAY:           keyq_push(FK_PLAY); return 0;
        case MODULE_KEY_TRASH:
        case MODULE_KEY_TOUCH_2_FINGER: keyq_push(FK_HOME); return 0;

        case 0:
            return 1;                   /* not a button: Canon's business */
        default:
            return 0;
    }

    /* newer firmware may group several wheel clicks into one event */
    int count = MAX(1, MIN(event->arg, 8));
    for (int i = 0; i < count; i++)
        keyq_push(dir);
    return 0;
}

static unsigned int lv_key(unsigned int key)
{
    /* what used to open the Magic Lantern menu opens home instead */
    if (key == MODULE_KEY_TRASH || key == MODULE_KEY_TOUCH_2_FINGER)
    {
        if (shooting_screen())
        {
            dbg("key: home");
            open_ui(FOS_OPEN_HOME);
        }
        return 0;
    }

    /* PLAY: the developed prints, never Canon's playback of what the
     * darkroom hasn't seen yet */
    if (key == MODULE_KEY_PLAY && play_lock && shooting_screen())
    {
        dbg("key: PLAY -> prints");
        open_ui(FOS_OPEN_PRINTS);
        return 0;
    }

    /* only on the plain LiveView screen: never steal keys from Canon's menus or dialogs */
    if (!lv || !roll_loaded || !display_idle())
        return 1;

    if (key == MODULE_KEY_PRESS_HALFSHUTTER || key == MODULE_KEY_UNPRESS_HALFSHUTTER)
        hud_dirty = 1;

    /* aperture ring for manual lenses: the wheel in Av mode (where it has
     * nothing to adjust without a lens), or up/down if the user prefers */
    if (lens_is_manual() && !ROLL_FINISHED)
    {
        int dir = 0;
        if (aperture_keys == 0 && shooting_mode == SHOOTMODE_AV)
            dir = (key == MODULE_KEY_WHEEL_RIGHT) ? 1 : (key == MODULE_KEY_WHEEL_LEFT) ? -1 : 0;
        else if (aperture_keys == 1)
            dir = (key == MODULE_KEY_PRESS_UP) ? 1 : (key == MODULE_KEY_PRESS_DOWN) ? -1 : 0;
        if (dir)
        {
            step_manual_fstop(dir);
            leds_until = get_ms_clock() + 2000;
            hud_dirty = 1;
            return 0;
        }
    }

    if (ROLL_FINISHED)
    {
        if (key == MODULE_KEY_PRESS_SET)
        {
            open_ui(FOS_OPEN_END);
            return 0;
        }
        return 1;
    }

    if (need_wind)
    {
        if (key == MODULE_KEY_PRESS_SET)
        {
            need_wind = 0;
            leds_until = get_ms_clock() + 1500;
            hud_dirty = 1;
            return 0;
        }
        if (wind_mode == 2 && (key == MODULE_KEY_PRESS_HALFSHUTTER || key == MODULE_KEY_PRESS_FULLSHUTTER))
            return 0;
    }

    return 1;
}

int handle_filmroll_keys(struct event *event)
{
    int key = module_translate_key(event->param, MODULE_KEY_PORTABLE);
    if (key && key != MODULE_KEY_PRESS_HALFSHUTTER && key != MODULE_KEY_UNPRESS_HALFSHUTTER &&
        key != MODULE_KEY_PRESS_FULLSHUTTER && key != MODULE_KEY_UNPRESS_FULLSHUTTER)
    {
        int now = get_ms_clock();
        last_key_ms = now ? now : 1;
    }
    if (ui_running)
    {
#ifdef BGMT_UNPRESS_LEFT
        /* the EOS M reports each direction's release on its own, and the
         * portable key codes have no name for those */
        switch (event->param)
        {
            case BGMT_UNPRESS_LEFT:  keyq_push(FK_RELEASE | FK_LEFT);  return 0;
            case BGMT_UNPRESS_RIGHT: keyq_push(FK_RELEASE | FK_RIGHT); return 0;
            case BGMT_UNPRESS_UP:    keyq_push(FK_RELEASE | FK_UP);    return 0;
            case BGMT_UNPRESS_DOWN:  keyq_push(FK_RELEASE | FK_DOWN);  return 0;
        }
#endif
        return ui_key(event, key);
    }

    if (!key)
        return 1;

    /* newer firmware may group several wheel clicks into one event */
    int count = 1;
    if (key == MODULE_KEY_WHEEL_LEFT || key == MODULE_KEY_WHEEL_RIGHT)
        count = MAX(1, event->arg);

    int pass = 1;
    for (int i = 0; i < count; i++)
        pass = lv_key(key);
    return pass;
}

/* ---------------------------------------------------------------- the ML menu, replaced */

#ifdef MODULE
int filmroll_menu_requested()
{
    if (shooting_screen())
        open_ui(FOS_OPEN_HOME);
    return 1;
}
#else
void filmroll_menu_requested()
{
    if (shooting_screen())
        open_ui(FOS_OPEN_HOME);
}
#endif

static void filmroll_init()
{
    if (roll_loaded)
        update_log_path();
    fos_ui_init();
    hud_palette_init();
    dbg("init: M-Film " MFILM_VERSION ", fork build %s %s, roll %d loaded %d shot %d/%d, startup %d hud %d",
        __DATE__, __TIME__, roll_number, roll_loaded, frames_shot, roll_exposures,
        startup_picker, hud_enabled);
}

/* The film OS's fonts are a file on the card (see fos_gfx.h): kept in
 * memory taken after boot, not in autoexec.bin */
static void load_fonts()
{
    int size = 0;
    uint8_t *raw = read_entire_file("ML/DATA/" FOS_FONT_FILE, &size);
    if (!raw)
    {
        dbg("fonts: ML/DATA/%s not found", FOS_FONT_FILE);
        return;
    }
    uint8_t *blob = malloc(size);       /* cached memory, for drawing */
    if (blob)
    {
        memcpy(blob, raw, size);
        if (!fos_fonts_load(blob, size))
        {
            free(blob);
            blob = 0;
        }
    }
    fio_free(raw);
    dbg("fonts: %d bytes, %s", size, blob ? "loaded" : "not usable");
}

/* the films beyond the built-in ones, from the card; the file stays in
 * memory (the films point into it) */
static void load_films()
{
    int size = 0;
    uint8_t *raw = read_entire_file("ML/DATA/" FOS_FILM_FILE, &size);
    if (!raw)
    {
        dbg("films: ML/DATA/%s not found, the built-in ones only", FOS_FILM_FILE);
        return;
    }
    uint8_t *blob = malloc(size);
    if (blob)
    {
        memcpy(blob, raw, size);
        if (!fos_films_load(blob, size))
        {
            free(blob);
            blob = 0;
        }
    }
    fio_free(raw);
    dbg("films: %d (%d bytes, %s)", fos_nfilms, size, blob ? "loaded" : "not usable");
}

/* the film OS is the camera's start screen */
static void startup_task()
{
    /* wait (up to ~15 s) for ML to finish booting and LiveView to settle */
    for (int i = 0; i < 150; i++)
    {
        if (ml_started && shooting_screen())
            break;
        if (i % 10 == 0 && ml_started)
            DBG_STATE("startup waiting");
        msleep(100);
    }
    msleep(300);

    DBG_STATE("startup check");
    if (!ml_started || !shooting_screen())
    {
        dbg("startup: not on the shooting screen, film OS skipped");
        return;
    }
    int shelf = startup_picker == 2 || (startup_picker == 1 && !ROLL_IN_PROGRESS);
    if (boot_anim && startup_picker)
        open_ui(shelf ? FOS_OPEN_BOOT : FOS_OPEN_BOOT_LV);
    else if (shelf)
        open_ui(FOS_OPEN_SHELF);
}

/* Everything runs from one task that waits for ML to finish booting, so
 * nothing touches Canon's GUI state before it exists: start screen first,
 * then frame logging and the HUD. */
static void filmroll_task()
{
    while (!ml_started)
        msleep(100);
    dbg("main: started");

    load_fonts();
    load_films();
    dbg("read: %s", card_buffer() ? "card buffer ready" : "no card buffer: the darkroom can't read the card");
    /* no rolls: the camera is always loaded, so a card that has never had a roll gets one now, with the
     * film it had last. Its camera settings go in when the film OS next lets go of the screen, as for a
     * loaded roll (the start-up screen is the film OS) */
    if (FREE_ROLLS && !roll_loaded)
    {
        load_roll(film_sel, FOS_SEGMENT, 0);
        ui_after |= AFTER_APPLY;
    }
    startup_task();
    film_capture_check();

    int last_alive = 0;
    TASK_LOOP
    {
        poll_frames();

        /* the roll has just been finished: once the camera is quiet, ask
         * whether to develop it now or load a new one first */
        if (end_due && get_ms_clock() > end_due && ROLL_FINISHED)
        {
            if (!ui_running && shooting_screen() && !get_halfshutter_pressed() && job_state_ready_to_take_pic())
            {
                end_due = 0;
                dbg("roll %03d finished: asking", roll_number);
                open_ui(FOS_OPEN_END);
            }
        }
        else if (end_due && !ROLL_FINISHED) end_due = 0;
        if (k % 2 == 0)
        {
            hud_step();                     /* every 100 ms */
            flip_step(0);
            expsim_step();
        }
#if !FOS_LV_ONLY
        else if (plate_overdrawn())
            hud_step();                     /* Canon drew over the plate: at once */
#endif

        int now = get_ms_clock();
        if (now - last_alive > 60000)
        {
            dbg("main: alive, roll %d loaded %d shot %d/%d file %d",
                roll_number, roll_loaded, frames_shot, roll_exposures, last_file_number);
            last_alive = now;
        }
        msleep(50);
    }
}

#ifndef MODULE
INIT_FUNC("filmroll", filmroll_init);
TASK_CREATE("film_main", filmroll_task, 0, 0x1e, 0x2000);
#else
#include "filmos_mod.h"
#endif
