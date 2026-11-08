/* The film OS as a module: the end of filmroll.c when it is built as
 * ML/modules/filmos.mo (build_fork.sh --module), in place of its INIT_FUNC
 * and TASK_CREATE. Included there so it sees filmroll.c's statics. */


/* The settings, saved in ML/SETTINGS/filmos.cfg by the module loader (same
 * names as before: filmroll.roll, filmroll.shot, ...). */
MODULE_CONFIGS_START()
    MODULE_CONFIG(film_sel)
    MODULE_CONFIG(exp_sel)
    MODULE_CONFIG(roll_limit)
    MODULE_CONFIG(hud_enabled)
    MODULE_CONFIG(focus_screen)
    MODULE_CONFIG(leds_on_half)
    MODULE_CONFIG(wind_mode)
    MODULE_CONFIG(play_lock)
    MODULE_CONFIG(startup_picker)
    MODULE_CONFIG(boot_anim)
    MODULE_CONFIG(debug_log)
    MODULE_CONFIG(film_mode)
    MODULE_CONFIG(lens_widest)
    MODULE_CONFIG(manual_fstop)
    MODULE_CONFIG(aperture_keys)
    MODULE_CONFIG(scr_std)
    MODULE_CONFIG(scr_sat)
    MODULE_CONFIG(scr_warm)
    MODULE_CONFIG(scr_bright)
    MODULE_CONFIG(scr_flip)
    MODULE_CONFIG(expsim_set)
    MODULE_CONFIG(roll_loaded)
    MODULE_CONFIG(roll_number)
    MODULE_CONFIG(roll_film)
    MODULE_CONFIG(roll_exposures)
    MODULE_CONFIG(frames_shot)
    MODULE_CONFIG(last_file_number)
    MODULE_CONFIG(roll_folder)
    MODULE_CONFIG(need_wind)
    MODULE_CONFIG(run_shot)
MODULE_CONFIGS_END()

/* A camera that ran the built-in film OS kept these settings in Magic
 * Lantern's own magic.cfg (config.c). The first time the module starts (no
 * FILMOS.CFG yet) it takes them from there, so a roll in the camera carries
 * on where it was. */
static void config_from_core()
{
    char path[64];
    snprintf(path, sizeof(path), "%sfilmos.cfg", get_config_dir());
    if ((int) FIO_GetFileSize_direct(path) >= 0)
        return;
    snprintf(path, sizeof(path), "%smagic.cfg", get_config_dir());
    int size = 0;
    char *buf = (char *) read_entire_file(path, &size);
    if (!buf)
        return;
    int taken = 0;
    for (char *line = buf; line < buf + size; )
    {
        char *end = line;
        while (end < buf + size && *end != '\n' && *end != '\r') end++;
        char save = (end < buf + size) ? *end : 0;
        if (end < buf + size) *end = 0;
        if (strncmp(line, "filmroll.", 9) == 0)
        {
            for (module_config_t *c = __module_config_filmos; c->name; c++)
            {
                int n = strlen(c->ref->name);
                if (strncmp(line, c->ref->name, n) == 0 && (line[n] == ' ' || line[n] == '='))
                {
                    char *v = line + n;
                    while (*v == ' ' || *v == '=') v++;
                    *(c->ref->value) = atoi(v);
                    taken++;
                    break;
                }
            }
        }
        if (end < buf + size) *end = save;
        line = end + 1;
    }
    fio_free(buf);
    dbg("init: %d settings taken from magic.cfg (first start as a module)", taken);
}

/* errno for the maths library linked into the module (fork/module/Makefile) */
int *fos_errno(void)
{
    static int e;
    return &e;
}

static unsigned int filmos_init()
{
    config_from_core();
    filmroll_init();
    task_create("film_main", 0x1e, 0x2000, filmroll_task, 0);
    return 0;
}

static unsigned int filmos_deinit()
{
    return 0;
}

/* What the core's shim (filmhook.c) calls. Not static: the module loader
 * hands these to the core by name once the module is loaded. */
int filmos_key_used(struct event *event)
{
    return handle_filmroll_keys(event) == 0;
}

int filmos_menu_open()
{
    return filmroll_menu_requested();
}

int filmos_overlays()
{
    return filmroll_overlays_active();
}

MODULE_INFO_START()
    MODULE_INIT(filmos_init)
    MODULE_DEINIT(filmos_deinit)
MODULE_INFO_END()
