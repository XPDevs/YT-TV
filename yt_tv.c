#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <errno.h>
#include <sys/wait.h>
#include <linux/input.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <linux/cec.h>

/* ===========================================================
   Platform & RAM detection
   =========================================================== */

#if defined(__aarch64__) || defined(__arm__)
static const bool is_arm = true;
#else
static const bool is_arm = false;
#endif

static long total_ram_mb(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    long kb = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemTotal: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb / 1024;
}

/* Set on 1 GiB-class boards (Raspberry Pi 3B).  Turns off every
   GPU-assisted path in WebKit and keeps the page cache small so the
   TV UI fits in the ~800 MiB left after the OS has booted. */
static bool low_mem = false;

/* ===========================================================
   Screen detection & selection
   =========================================================== */

typedef struct {
    int n;
    XineramaScreenInfo *info;
    bool fallback;          /* info points at fallback_screen */
} ScreenConfig;

static XineramaScreenInfo fallback_screen;

static bool detect_screens(ScreenConfig *cfg) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return false;
    cfg->info = XineramaQueryScreens(dpy, &cfg->n);
    cfg->fallback = false;
    if (!cfg->info || cfg->n <= 0) {
        /* X without the Xinerama extension (common on small/embedded
           servers) still has one usable screen - do not bail out. */
        int scr = DefaultScreen(dpy);
        memset(&fallback_screen, 0, sizeof fallback_screen);
        fallback_screen.width  = DisplayWidth(dpy, scr);
        fallback_screen.height = DisplayHeight(dpy, scr);
        cfg->info = &fallback_screen;
        cfg->n = 1;
        cfg->fallback = true;
    }
    XCloseDisplay(dpy);
    return cfg->n > 0;
}

static void free_screens(ScreenConfig *cfg) {
    if (cfg->info && !cfg->fallback) XFree(cfg->info);
    cfg->info = NULL;
    cfg->n = 0;
}

/* Save/load chosen screen index */
static const char *CONFIG_FILE = ".yt_tv_display";

static int load_saved_screen(void) {
    char *home = getenv("HOME");
    if (!home) return -1;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", home, CONFIG_FILE);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int idx = -1;
    fscanf(f, "%d", &idx);
    fclose(f);
    return idx;
}

static void save_screen_choice(int idx) {
    char *home = getenv("HOME");
    if (!home) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", home, CONFIG_FILE);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%d", idx); fclose(f); }
}

/* Display selection dialog – returns chosen index or -1 if cancelled */
static int choose_screen_dialog(ScreenConfig *cfg) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Select Display",
        NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 20);

    GtkWidget *label = gtk_label_new("Choose the screen for YouTube TV:");
    gtk_grid_attach(GTK_GRID(grid), label, 0, 0, 2, 1);

    GSList *group = NULL;
    int *selected = malloc(sizeof(int));
    *selected = -1;

    for (int i = 0; i < cfg->n; i++) {
        char desc[256];
        snprintf(desc, sizeof(desc), "Screen %d: %dx%d at %d,%d%s",
                 i, cfg->info[i].width, cfg->info[i].height,
                 cfg->info[i].x_org, cfg->info[i].y_org,
                 i == cfg->n - 1 ? " (primary)" : "");
        GtkWidget *rb = gtk_radio_button_new_with_label(group, desc);
        group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(rb));
        gtk_grid_attach(GTK_GRID(grid), rb, 0, i+1, 2, 1);
        g_object_set_data(G_OBJECT(rb), "idx", GINT_TO_POINTER(i));
        g_signal_connect(rb, "toggled", G_CALLBACK(+[](GtkToggleButton *b, gpointer p) {
            if (gtk_toggle_button_get_active(b))
                *(int*)p = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "idx"));
        }), selected);
        if (i == 0) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rb), TRUE);
    }

    gtk_container_add(GTK_CONTAINER(content), grid);
    gtk_widget_show_all(dialog);

    int result = gtk_dialog_run(GTK_DIALOG(dialog));
    int chosen = *selected;
    free(selected);
    gtk_widget_destroy(dialog);

    if (result == GTK_RESPONSE_CANCEL || chosen < 0 || chosen >= cfg->n)
        return -1;
    return chosen;
}

/* ===========================================================
   App state
   =========================================================== */

typedef struct {
    GtkWidget *win;
    WebKitWebView *webview;
    GtkWidget *loading_label;

    int     scr_w, scr_h, scr_x, scr_y;

    int     cec_fd;          /* /dev/cecN, -1 when CEC is unavailable */
    guint   cec_watch_id;
    bool    cec_ok;
    __u8    cec_log_addr;    /* logical address claimed on the CEC bus */

    Display *x_dpy;
    Window   win_xid;

    GtkWidget *overlay;              /* overlay container for video and OSD */
    GtkWidget *volume_osd;           /* volume overlay window (if any) */
    guint     volume_osd_timeout;    /* timeout to hide volume OSD */

    /* Black overlays for other screens */
    GList *black_windows;
} App;

static bool xtest_ok = false;
static bool ctrl_debug = false;
static App app;

/* Forward declarations (defined further down) */
static void load_youtube_tv(void);

/* ===========================================================
   User-agent: Smart TV
   =========================================================== */

#define TV_UA "Mozilla/5.0 (SMART-TV; Linux; Tizen 5.0) " \
              "AppleWebKit/537.36 (KHTML, like Gecko) " \
              "SamsungBrowser/2.0 Chrome/63.0.3239.84 " \
              "TV Safari/537.36"

/* ===========================================================
   X11 key sender
   =========================================================== */

static void win_focus(void) {
    if (!app.x_dpy || !app.win_xid) return;
    XSetInputFocus(app.x_dpy, app.win_xid, RevertToParent, CurrentTime);
    XFlush(app.x_dpy);
    usleep(5000);
}

static void x11_send_key(KeySym ks) {
    if (!app.x_dpy || !app.win_xid) return;
    KeyCode kc = XKeysymToKeycode(app.x_dpy, ks);
    if (!kc) return;
    win_focus();
    XTestFakeKeyEvent(app.x_dpy, kc, True, CurrentTime);
    XTestFakeKeyEvent(app.x_dpy, kc, False, CurrentTime);
    XFlush(app.x_dpy);
}

static void x11_send_key_str(const char *key) {
    struct { const char *name; KeySym ks; } map[] = {
        {"ArrowUp",    XK_Up},      {"ArrowDown",  XK_Down},
        {"ArrowLeft",  XK_Left},    {"ArrowRight", XK_Right},
        {"Enter",      XK_Return},  {"Escape",     XK_Escape},
        {"space",      XK_space},
        {"KeyL",       XK_l},       {"KeyJ",       XK_j},
        {"KeyR",       XK_r},       {"KeyG",       XK_g},
        {"KeyY",       XK_y},       {"KeyB",       XK_b},
        {"0", XK_0},{"1", XK_1},{"2", XK_2},{"3", XK_3},{"4", XK_4},
        {"5", XK_5},{"6", XK_6},{"7", XK_7},{"8", XK_8},{"9", XK_9},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strcmp(key, map[i].name) == 0) { x11_send_key(map[i].ks); return; }
    KeySym ks = XStringToKeysym(key);
    if (ks) x11_send_key(ks);
}

/* ===========================================================
   WebView JS helpers
   =========================================================== */

static void js_exec(const char *js) {
    if (!app.webview) return;
    webkit_web_view_evaluate_javascript(app.webview, js, -1,
                                        NULL, NULL, NULL, NULL, NULL);
}

static void webview_play_pause(void) {
    x11_send_key(XK_space);
    js_exec("(()=>{const v=document.querySelector('video');"
            "if(v)v.paused?v.play():v.pause();})()");
}
static void webview_volume_up(void) {
    js_exec("(()=>{const v=document.querySelector('video');"
            "if(v)v.volume=Math.min(1,v.volume+0.1);})()");
    /* Show volume OSD */
    if (app.volume_osd) gtk_widget_show(app.volume_osd);
    if (app.volume_osd_timeout) g_source_remove(app.volume_osd_timeout);
    app.volume_osd_timeout = g_timeout_add(1500, (GSourceFunc)+[](gpointer d) {
        (void)d;
        if (app.volume_osd) gtk_widget_hide(app.volume_osd);
        app.volume_osd_timeout = 0;
        return FALSE;
    }, NULL);
}
static void webview_volume_down(void) {
    js_exec("(()=>{const v=document.querySelector('video');"
            "if(v)v.volume=Math.max(0,v.volume-0.1);})()");
    if (app.volume_osd) gtk_widget_show(app.volume_osd);
    if (app.volume_osd_timeout) g_source_remove(app.volume_osd_timeout);
    app.volume_osd_timeout = g_timeout_add(1500, (GSourceFunc)+[](gpointer d) {
        (void)d;
        if (app.volume_osd) gtk_widget_hide(app.volume_osd);
        app.volume_osd_timeout = 0;
        return FALSE;
    }, NULL);
}
static void webview_mute(void) {
    js_exec("(()=>{const v=document.querySelector('video');"
            "if(v)v.muted=!v.muted;})()");
}
static void webview_play(void) {
    js_exec("(()=>{const v=document.querySelector('video');"
            "if(v)v.play().catch(function(){});})()");
}
static void webview_pause(void) {
    js_exec("(()=>{const v=document.querySelector('video');"
            "if(v)v.pause();})()");
}

/* ===========================================================
   WebView callbacks
   =========================================================== */

static void focus_webview(void) {
    if (app.win) gtk_window_present(GTK_WINDOW(app.win));
    if (app.webview) gtk_widget_grab_focus(GTK_WIDGET(app.webview));
}

static void on_webview_load_changed(WebKitWebView *wv,
                                    WebKitLoadEvent ev, gpointer d) {
    (void)wv; (void)d;
    if (ev == WEBKIT_LOAD_STARTED && app.loading_label)
        gtk_label_set_text(GTK_LABEL(app.loading_label), "Loading…");
    else if (ev == WEBKIT_LOAD_FINISHED) {
        if (app.loading_label) gtk_label_set_text(GTK_LABEL(app.loading_label), "");
        printf("[yt] YouTube TV loaded\n");
        focus_webview();
        js_exec("(()=>{const v=document.querySelector('video');"
                "if(v){v.muted=false;v.volume=1;"
                "v.play().catch(function(){});"
                "setInterval(function(){"
                "if(v.paused){v.muted=false;v.play().catch(function(){})}"
                "},2000);"
                "}})()");
    }
}

static void on_webview_load_failed(WebKitWebView *wv, WebKitLoadEvent ev,
                                   const gchar *uri, GError *err, gpointer d) {
    (void)wv; (void)ev; (void)uri; (void)d;
    if (app.loading_label) gtk_label_set_text(GTK_LABEL(app.loading_label), "");
    printf("[yt] Load failed: %s\n", err->message);
}

static GtkWidget *make_webview(void) {
    WebKitSettings *s = webkit_settings_new_with_settings(
        "user-agent", TV_UA,
        "enable-javascript", TRUE,
        "enable-plugins", TRUE,
        "enable-media-stream", TRUE,
        NULL);
    webkit_settings_set_enable_encrypted_media(s, TRUE);
    webkit_settings_set_enable_mediasource(s, TRUE);
    webkit_settings_set_enable_webaudio(s, TRUE);
    webkit_settings_set_enable_fullscreen(s, TRUE);
    webkit_settings_set_enable_developer_extras(s, TRUE);

    /* Raspberry Pi / low-RAM tuning.  The Pi 3B has no GL path worth
       using for WebKit's compositor, and every GPU-assisted feature
       costs RAM and CPU we cannot afford on 1 GiB. */
    if (is_arm || low_mem) {
        webkit_settings_set_hardware_acceleration_policy(
            s, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
        webkit_settings_set_enable_webgl(s, FALSE);
        webkit_settings_set_enable_accelerated_2d_canvas(s, FALSE);
        webkit_settings_set_enable_smooth_scrolling(s, FALSE);
        webkit_settings_set_enable_page_cache(s, FALSE);
        webkit_settings_set_enable_developer_extras(s, !low_mem);
        webkit_web_context_set_cache_model(webkit_web_context_get_default(),
                                           WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    }

    GtkWidget *wv = webkit_web_view_new_with_settings(s);
    app.webview = WEBKIT_WEB_VIEW(wv);

    /* On ARM, advertise no VP9/AV1 support so YouTube serves H.264,
       which the Pi's V4L2 hardware decoder can actually play.  Set
       YT_TV_ALLOW_VP9=1 to turn this off. */
    if (is_arm && !g_getenv("YT_TV_ALLOW_VP9")) {
        WebKitUserContentManager *ucm =
            webkit_web_view_get_user_content_manager(app.webview);
        if (ucm) {
            const char *js =
                "(function(){"
                "var rx=/vp0?9|av01/i;"
                "function ok(t){try{return !rx.test(String(t||''))}catch(e){return true}}"
                "try{var f=MediaSource.isTypeSupported;"
                "MediaSource.isTypeSupported=function(t){"
                "return ok(t)&&f.apply(this,arguments)};}catch(e){}"
                "try{var g=HTMLMediaElement.prototype.canPlayType;"
                "HTMLMediaElement.prototype.canPlayType=function(t){"
                "return ok(t)?g.apply(this,arguments):''};}catch(e){}"
                "})();";
            WebKitUserScript *us = webkit_user_script_new(
                js, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
            webkit_user_content_manager_add_script(ucm, us);
            webkit_user_script_unref(us);
            printf("[yt] ARM codec hint: VP9/AV1 hidden, H.264 preferred\n");
        }
    }

    g_signal_connect(wv, "load-changed", G_CALLBACK(on_webview_load_changed), NULL);
    g_signal_connect(wv, "load-failed",  G_CALLBACK(on_webview_load_failed), NULL);

    return wv;
}

/* ===========================================================
   HDMI-CEC - built in, no cec-client / libcec

   The vc4 KMS driver exposes the Pi's HDMI CEC pin as /dev/cec0.  We
   register as a "Playback Device" so the TV lists us properly, then
   translate the remote's user-control messages into the same X key
   events a USB keyboard or gamepad would produce.
   =========================================================== */

static int cec_transmit(__u8 initiator, __u8 dest, const __u8 *payload, __u32 len) {
    if (app.cec_fd < 0) return -1;
    struct cec_msg m;
    memset(&m, 0, sizeof m);
    m.len = len;
    m.msg[0] = (__u8)((initiator << 4) | dest);
    if (len > 1) memcpy(&m.msg[1], payload, len - 1);
    return ioctl(app.cec_fd, CEC_TRANSMIT, &m);
}

/* Playback logical addresses in the order a TV hands them out. */
static __u8 cec_playback_addr_from_mask(__u16 mask) {
    static const __u8 cand[] = { 4, 8, 11 };   /* PLAYBACK_1/2/3 */
    for (size_t i = 0; i < sizeof cand; i++)
        if (mask & (1u << cand[i])) return cand[i];
    return CEC_LOG_ADDR_INVALID;
}

/* Tell the TV who we are: physical address + OSD name. */
static void cec_announce(void) {
    if (app.cec_fd < 0 || app.cec_log_addr == CEC_LOG_ADDR_INVALID) return;

    __u16 pa = 0xffff;
    if (ioctl(app.cec_fd, CEC_ADAP_G_PHYS_ADDR, &pa) == 0 && pa != 0xffff) {
        const __u8 pa_msg[4] = { 0x84, (__u8)(pa >> 8), (__u8)(pa & 0xff),
                                 CEC_OP_PRIM_DEVTYPE_PLAYBACK };
        cec_transmit(app.cec_log_addr, CEC_LOG_ADDR_BROADCAST, pa_msg, 4);
    }
    const __u8 name_msg[6] = { 0x47, 'y', 't', '_', 't', 'v' };
    cec_transmit(app.cec_log_addr, CEC_LOG_ADDR_TV, name_msg, sizeof name_msg);
}

static void cec_ui_command(unsigned cmd) {
    switch (cmd) {
    case CEC_OP_UI_CMD_SELECT:
    case CEC_OP_UI_CMD_ENTER:         x11_send_key_str("Enter");      break;
    case CEC_OP_UI_CMD_UP:            x11_send_key_str("ArrowUp");    break;
    case CEC_OP_UI_CMD_DOWN:          x11_send_key_str("ArrowDown");  break;
    case CEC_OP_UI_CMD_LEFT:          x11_send_key_str("ArrowLeft");  break;
    case CEC_OP_UI_CMD_RIGHT:         x11_send_key_str("ArrowRight"); break;
    case CEC_OP_UI_CMD_BACK:          x11_send_key_str("Escape");     break;
    case CEC_OP_UI_CMD_VOLUME_UP:     webview_volume_up();            break;
    case CEC_OP_UI_CMD_VOLUME_DOWN:   webview_volume_down();          break;
    case CEC_OP_UI_CMD_MUTE:          webview_mute();                 break;
    case CEC_OP_UI_CMD_PLAY:          webview_play();                 break;
    case CEC_OP_UI_CMD_PAUSE:
    case CEC_OP_UI_CMD_STOP:          webview_pause();                break;
    case CEC_OP_UI_CMD_FAST_FORWARD:  x11_send_key_str("KeyL");       break;
    case CEC_OP_UI_CMD_REWIND:        x11_send_key_str("KeyJ");       break;
    case CEC_OP_UI_CMD_CHANNEL_UP:    x11_send_key_str("KeyG");       break;
    case CEC_OP_UI_CMD_CHANNEL_DOWN:  x11_send_key_str("KeyY");       break;
    case CEC_OP_UI_CMD_MEDIA_TOP_MENU: x11_send_key_str("KeyY");      break;
    default:
        if (cmd >= CEC_OP_UI_CMD_NUMBER_0_OR_NUMBER_10 &&
            cmd <= CEC_OP_UI_CMD_NUMBER_9) {
            char n[2] = { (char)('0' + (cmd - CEC_OP_UI_CMD_NUMBER_0_OR_NUMBER_10)), 0 };
            x11_send_key_str(n);
        }
        break;
    }
}

static gboolean on_cec_io(GIOChannel *ch, GIOCondition cond, gpointer d) {
    (void)ch; (void)d;

    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        printf("[cec] adapter went away\n");
        app.cec_watch_id = 0;
        if (app.cec_fd >= 0) { close(app.cec_fd); app.cec_fd = -1; }
        app.cec_ok = false;
        return FALSE;
    }

    struct cec_event ev;
    while (read(app.cec_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.event != CEC_EVENT_STATE_CHANGE) continue;
        printf("[cec] state change: phys=0x%04x mask=0x%04x\n",
               ev.state_change.phys_addr, ev.state_change.log_addr_mask);
        if (app.cec_log_addr == CEC_LOG_ADDR_INVALID)
            app.cec_log_addr = cec_playback_addr_from_mask(ev.state_change.log_addr_mask);
        if (app.cec_log_addr != CEC_LOG_ADDR_INVALID)
            cec_announce();
    }

    struct cec_msg m;
    for (;;) {
        memset(&m, 0, sizeof m);
        if (ioctl(app.cec_fd, CEC_RECEIVE, &m) < 0) break;   /* EAGAIN = drained */
        if (m.len >= 3 && m.msg[1] == CEC_MSG_USER_CONTROL_PRESSED)
            cec_ui_command(m.msg[2]);
    }
    return TRUE;
}

static void start_cec(void) {
    if (app.cec_ok || g_getenv("YT_TV_NO_CEC")) return;

    for (int i = 0; i < 4; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/cec%d", i);
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        struct cec_log_addrs la;
        memset(&la, 0, sizeof la);
        for (int j = 0; j < CEC_MAX_LOG_ADDRS; j++)
            la.log_addr[j] = CEC_LOG_ADDR_INVALID;
        la.log_addr_type[0]       = CEC_LOG_ADDR_TYPE_PLAYBACK;
        la.primary_device_type[0] = CEC_OP_PRIM_DEVTYPE_PLAYBACK;
        la.num_log_addrs          = 1;
        la.cec_version            = CEC_OP_CEC_VERSION_2_0;
        la.vendor_id              = 0x000c03;   /* HDMI Licensing, LLC */
        snprintf(la.osd_name, sizeof la.osd_name, "youtube-tv");

        if (ioctl(fd, CEC_ADAP_S_LOG_ADDRS, &la) < 0) {
            la.cec_version = CEC_OP_CEC_VERSION_1_4;   /* older TVs / adapters */
            if (ioctl(fd, CEC_ADAP_S_LOG_ADDRS, &la) < 0) {
                printf("[cec] %s: cannot claim a logical address: %s\n",
                       path, strerror(errno));
                close(fd);
                continue;
            }
        }

        app.cec_fd       = fd;
        app.cec_log_addr = la.log_addr[0];
        app.cec_ok       = true;

        GIOChannel *ch = g_io_channel_unix_new(fd);
        app.cec_watch_id = g_io_add_watch(
            ch, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, on_cec_io, NULL);
        g_io_channel_unref(ch);

        printf("[cec] %s: playback device, logical address 0x%02x\n",
               path, app.cec_log_addr);
        cec_announce();
        return;
    }
    printf("[cec] no adapter on /dev/cec0-3 "
           "(needs dtoverlay=vc4-kms-v3d and a CEC-capable TV)\n");
}

static void stop_cec(void) {
    if (app.cec_watch_id) { g_source_remove(app.cec_watch_id); app.cec_watch_id = 0; }
    if (app.cec_fd >= 0)  { close(app.cec_fd); app.cec_fd = -1; }
    app.cec_ok = false;
}

/* ===========================================================
   USB Controller (evdev)
   =========================================================== */

#define MAX_CONTROLLERS 4

typedef struct {
    int   fd;
    guint watch_id;
    char  name[128];
    int   hat0x;
    int   hat0y;
    int   prev_axis[ABS_CNT];
} ControllerDev;

static ControllerDev controllers[MAX_CONTROLLERS];
static int n_controllers = 0;

static void controller_handle_key(unsigned short code, int val) {
    if (val != 1) return;
    switch (code) {
    case BTN_A:       x11_send_key_str("Enter");        break;
    case BTN_B:       x11_send_key_str("Escape");       break;
    case BTN_X:       x11_send_key_str("KeyJ");         break;
    case BTN_Y:       x11_send_key_str("KeyL");         break;
    case BTN_START:   webview_play_pause();             break;
    case BTN_SELECT:  x11_send_key_str("Escape");       break;
    case BTN_TL:      webview_volume_down();            break;
    case BTN_TR:      webview_volume_up();              break;
    case BTN_TL2:     webview_mute();                   break;
    case BTN_TR2:     x11_send_key_str("ArrowRight");   break;
    case BTN_THUMBL:  x11_send_key_str("KeyR");         break;
    case BTN_THUMBR:  x11_send_key_str("KeyG");         break;
    case BTN_MODE:    x11_send_key_str("KeyY");         break;
    default: break;
    }
}

/* Map an axis value to a direction: -1=centered, 0=up, 1=down, 2=left, 3=right */
static int axis_to_dir(unsigned short code, int val) {
    /* Detect range: controllers like PS use 0-255 (center 128),
       others use -32768-32767 (center 0) */
    int center;
    int dead;
    if (val >= 0 && val <= 65535) {
        center = 128;
        dead   = 64;
    } else {
        center = 0;
        dead   = 20000;
    }

    if (code == ABS_X || code == ABS_RX) {
        if      (val < center - dead) return 2;
        else if (val > center + dead) return 3;
    } else {
        if      (val < center - dead) return 0;
        else if (val > center + dead) return 1;
    }
    return -1;
}

static void controller_handle_abs(ControllerDev *ctrl, unsigned short code, int val) {
    switch (code) {
    case ABS_HAT0X:
        if (val == ctrl->hat0x) return;
        ctrl->hat0x = val;
        if (val == -1)      x11_send_key_str("ArrowLeft");
        else if (val == 1)  x11_send_key_str("ArrowRight");
        break;
    case ABS_HAT0Y:
        if (val == ctrl->hat0y) return;
        ctrl->hat0y = val;
        if (val == -1)      x11_send_key_str("ArrowUp");
        else if (val == 1)  x11_send_key_str("ArrowDown");
        break;
    case ABS_X:      case ABS_Y:
    case ABS_RX:     case ABS_RY: {
        int dir = axis_to_dir(code, val);
        if (dir == ctrl->prev_axis[code]) return;
        ctrl->prev_axis[code] = dir;
        if      (dir == 0) x11_send_key_str("ArrowUp");
        else if (dir == 1) x11_send_key_str("ArrowDown");
        else if (dir == 2) x11_send_key_str("ArrowLeft");
        else if (dir == 3) x11_send_key_str("ArrowRight");
        break;
    }
    case ABS_Z: {
        int dir = val > 16384 ? 3 : (val < -16384 ? 2 : -1);
        if (dir == ctrl->prev_axis[code]) return;
        ctrl->prev_axis[code] = dir;
        if (dir == 2) x11_send_key_str("ArrowLeft");
        else if (dir == 3) x11_send_key_str("ArrowRight");
        break;
    }
    case ABS_RZ: {
        int dir = val > 16384 ? 1 : (val < -16384 ? 0 : -1);
        if (dir == ctrl->prev_axis[code]) return;
        ctrl->prev_axis[code] = dir;
        if (dir == 0) x11_send_key_str("ArrowUp");
        else if (dir == 1) x11_send_key_str("ArrowDown");
        break;
    }
    default: break;
    }
}

static void dump_event(const struct input_event *ev) {
    const char *type_str = "?";
    switch (ev->type) {
    case EV_KEY: type_str = "EV_KEY"; break;
    case EV_ABS: type_str = "EV_ABS"; break;
    case EV_SYN: type_str = "EV_SYN"; break;
    case EV_MSC: type_str = "EV_MSC"; break;
    }
    printf("[ctrl:debug] type=%-6s  code=0x%04x (%3d)  value=%d\n",
           type_str, ev->code, ev->code, ev->value);
}

static gboolean on_controller_data(GIOChannel *ch, GIOCondition cond, gpointer d) {
    (void)d;
    int fd = g_io_channel_unix_get_fd(ch);
    int ci = -1;
    for (int i = 0; i < n_controllers; i++)
        if (controllers[i].fd == fd) { ci = i; break; }
    if (ci < 0) return FALSE;

    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        printf("[ctrl] disconnected: %s\n", controllers[ci].name);
        close(controllers[ci].fd);
        for (int j = ci; j < n_controllers - 1; j++)
            controllers[j] = controllers[j+1];
        n_controllers--;
        return FALSE;
    }

    /* Drain all pending events */
    struct input_event evbuf[64];
    int nr = read(fd, evbuf, sizeof(evbuf));
    if (nr <= 0) return TRUE;

    int nevents = nr / (int)sizeof(struct input_event);
    for (int i = 0; i < nevents; i++) {
        const struct input_event *ev = &evbuf[i];
        if (ctrl_debug) dump_event(ev);
        if (ev->type == EV_KEY)
            controller_handle_key(ev->code, ev->value);
        else if (ev->type == EV_ABS)
            controller_handle_abs(&controllers[ci], ev->code, ev->value);
        /* EV_SYN, EV_MSC: skip */
    }
    return TRUE;
}

#define test_bit(n, addr) \
    (((unsigned long *)(addr))[(n) / (8 * sizeof(long))] & (1UL << ((n) % (8 * sizeof(long)))))

static bool is_gamepad(int fd) {
    unsigned long bits[KEY_MAX/8/sizeof(long) + 1];
    memset(bits, 0, sizeof(bits));

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
        return false;

    /* Must have at least one gamepad-like button */
    int btn_ok = test_bit(BTN_A, bits) || test_bit(BTN_B, bits) ||
                 test_bit(BTN_X, bits) || test_bit(BTN_Y, bits) ||
                 test_bit(BTN_START, bits) || test_bit(BTN_SELECT, bits) ||
                 test_bit(BTN_TL, bits) || test_bit(BTN_TR, bits);
    if (!btn_ok) return false;

    unsigned long abs_bits[ABS_CNT/8/sizeof(long) + 1];
    memset(abs_bits, 0, sizeof(abs_bits));
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0)
        return false;

    /* Must have some joystick axis or D-pad */
    if (!test_bit(ABS_X, abs_bits) && !test_bit(ABS_HAT0X, abs_bits) &&
        !test_bit(ABS_RX, abs_bits))
        return false;

    return true;
}

/* inotify hotplug */
static int  inotify_fd = -1;
static guint inotify_watch = 0;

static void try_add_controller(const char *path) {
    if (n_controllers >= MAX_CONTROLLERS) return;

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return;

    if (!is_gamepad(fd)) { close(fd); return; }

    char devname[128] = {0};
    ioctl(fd, EVIOCGNAME(sizeof(devname)-1), devname);

    GIOChannel *ch = g_io_channel_unix_new(fd);
    guint wid = g_io_add_watch(ch,
        G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
        on_controller_data, NULL);
    g_io_channel_unref(ch);

    controllers[n_controllers].fd       = fd;
    controllers[n_controllers].watch_id = wid;
    controllers[n_controllers].hat0x    = 0;
    controllers[n_controllers].hat0y    = 0;
    strncpy(controllers[n_controllers].name, devname,
            sizeof(controllers[n_controllers].name)-1);

    printf("[ctrl] USB controller %d: %s (%s)\n",
           n_controllers + 1, devname, path);
    n_controllers++;
}

static gboolean on_inotify_data(GIOChannel *ch, GIOCondition cond, gpointer d) {
    (void)d;
    if (cond & (G_IO_HUP | G_IO_ERR)) return FALSE;

    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    int n = read(g_io_channel_unix_get_fd(ch), buf, sizeof(buf));
    if (n <= 0) return TRUE;

    for (int off = 0; off < n; ) {
        struct inotify_event *ev = (struct inotify_event *)(buf + off);
        off += sizeof(struct inotify_event) + ev->len;

        if ((ev->mask & IN_CREATE) && ev->len > 0 &&
            strncmp(ev->name, "event", 5) == 0) {
            char path[320];
            snprintf(path, sizeof path, "/dev/input/%s", ev->name);
            try_add_controller(path);
        }
    }
    return TRUE;
}

static void start_controller(void) {
    DIR *d = opendir("/dev/input");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && n_controllers < MAX_CONTROLLERS) {
            if (strncmp(de->d_name, "event", 5) != 0) continue;
            char path[320];
            snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
            try_add_controller(path);
        }
        closedir(d);
    }

    if (n_controllers > 0)
        printf("[ctrl] %d controller(s) connected\n", n_controllers);
    if (ctrl_debug)
        printf("[ctrl] debug mode ON — all raw events will be printed\n");

    /* Set up inotify hotplug watch */
    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) return;

    if (inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE) < 0) {
        close(inotify_fd);
        inotify_fd = -1;
        return;
    }

    GIOChannel *ich = g_io_channel_unix_new(inotify_fd);
    g_io_channel_set_encoding(ich, NULL, NULL);
    inotify_watch = g_io_add_watch(ich, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                   on_inotify_data, NULL);
    g_io_channel_unref(ich);
    printf("[ctrl] hotplug enabled\n");
}

static void stop_controller(void) {
    if (inotify_watch) { g_source_remove(inotify_watch); inotify_watch = 0; }
    if (inotify_fd >= 0) { close(inotify_fd); inotify_fd = -1; }
    for (int i = 0; i < n_controllers; i++) {
        if (controllers[i].watch_id)
            g_source_remove(controllers[i].watch_id);
        if (controllers[i].fd >= 0)
            close(controllers[i].fd);
    }
    n_controllers = 0;
}

/* ===========================================================
   Keyboard handler
   =========================================================== */

static guint esc_timeout_id = 0;

static gboolean esc_hold_exit(gpointer d) {
    (void)d;
    printf("[yt] Escape held — exiting\n");
    gtk_main_quit();
    return FALSE;
}

static void esc_release(void) {
    if (esc_timeout_id) { g_source_remove(esc_timeout_id); esc_timeout_id = 0; }
}

/* Forward declaration for display switch */
static void switch_display(void);

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer d) {
    (void)w; (void)d;

    /* Ctrl+Shift+D to switch display */
    if (ev->type == GDK_KEY_PRESS &&
        (ev->state & GDK_CONTROL_MASK) && (ev->state & GDK_SHIFT_MASK) &&
        (ev->keyval == GDK_KEY_d || ev->keyval == GDK_KEY_D)) {
        switch_display();
        return TRUE;
    }

    /* hold Escape for 1s to quit */
    if (ev->keyval == GDK_KEY_Escape && ev->type == GDK_KEY_PRESS) {
        if (!esc_timeout_id)
            esc_timeout_id = g_timeout_add(1000, esc_hold_exit, NULL);
        return FALSE;
    }
    if (ev->type == GDK_KEY_RELEASE && ev->keyval == GDK_KEY_Escape) {
        esc_release();
        return FALSE;
    }

    if (ev->type == GDK_KEY_RELEASE) return FALSE;

    switch (ev->keyval) {
    /* Volume */
    case GDK_KEY_F9:       case GDK_KEY_KP_Add:
        webview_volume_up(); return TRUE;
    case GDK_KEY_F10:      case GDK_KEY_KP_Subtract:
        webview_volume_down(); return TRUE;
    case GDK_KEY_m:        case GDK_KEY_M:
        webview_mute(); return TRUE;

    /* Playback */
    case GDK_KEY_space:    case GDK_KEY_K:
        webview_play_pause(); return TRUE;
    case GDK_KEY_j:        case GDK_KEY_J:
        x11_send_key_str("KeyJ"); return TRUE;
    case GDK_KEY_l:        case GDK_KEY_L:
        x11_send_key_str("KeyL"); return TRUE;

    /* Navigation — let pass through to webview */
    case GDK_KEY_Up:       case GDK_KEY_Down:
    case GDK_KEY_Left:     case GDK_KEY_Right:
    case GDK_KEY_Return:   case GDK_KEY_KP_Enter:
        return FALSE;

    /* Like, Guide, YouTube, Back */
    case GDK_KEY_r:        case GDK_KEY_R:
        x11_send_key_str("KeyR"); return TRUE;
    case GDK_KEY_g:        case GDK_KEY_G:
        x11_send_key_str("KeyG"); return TRUE;
    case GDK_KEY_y:        case GDK_KEY_Y:
        x11_send_key_str("KeyY"); return TRUE;
    case GDK_KEY_b:        case GDK_KEY_B:
        x11_send_key_str("KeyB"); return TRUE;

    /* Numbers */
    case GDK_KEY_0: case GDK_KEY_KP_0: x11_send_key_str("0"); return TRUE;
    case GDK_KEY_1: case GDK_KEY_KP_1: x11_send_key_str("1"); return TRUE;
    case GDK_KEY_2: case GDK_KEY_KP_2: x11_send_key_str("2"); return TRUE;
    case GDK_KEY_3: case GDK_KEY_KP_3: x11_send_key_str("3"); return TRUE;
    case GDK_KEY_4: case GDK_KEY_KP_4: x11_send_key_str("4"); return TRUE;
    case GDK_KEY_5: case GDK_KEY_KP_5: x11_send_key_str("5"); return TRUE;
    case GDK_KEY_6: case GDK_KEY_KP_6: x11_send_key_str("6"); return TRUE;
    case GDK_KEY_7: case GDK_KEY_KP_7: x11_send_key_str("7"); return TRUE;
    case GDK_KEY_8: case GDK_KEY_KP_8: x11_send_key_str("8"); return TRUE;
    case GDK_KEY_9: case GDK_KEY_KP_9: x11_send_key_str("9"); return TRUE;

    /* Reload */
    case GDK_KEY_F5:
        if (app.webview) webkit_web_view_reload(app.webview);
        return TRUE;

    /* Developer tools */
    case GDK_KEY_F12:
        if (app.webview) {
            WebKitWebInspector *insp = webkit_web_view_get_inspector(app.webview);
            if (insp) webkit_web_inspector_show(insp);
        }
        return TRUE;

    /* Quit */
    case GDK_KEY_q:        case GDK_KEY_Q:
        gtk_main_quit(); return TRUE;
    default: break;
    }
    return FALSE;
}

/* ===========================================================
   Window XID tracker
   =========================================================== */

static void update_xid(void) {
    if (!app.win) return;
    GdkWindow *gw = gtk_widget_get_window(app.win);
    if (gw) app.win_xid = GDK_WINDOW_XID(gw);
}

/* ===========================================================
   YouTube TV
   =========================================================== */

/* Straight to the TV UI - no boot animation. */
static void load_youtube_tv(void) {
    if (app.webview) gtk_widget_show(GTK_WIDGET(app.webview));
    if (app.loading_label) gtk_widget_show(app.loading_label);
    webkit_web_view_load_uri(app.webview, "https://www.youtube.com/tv");
    printf("[yt] Loading YouTube TV\n");
}

/* ===========================================================
   Multi‑display: black overlays & display switching
   =========================================================== */

/* Create a black fullscreen window on a given screen (by index) */
static GtkWidget *create_black_overlay(ScreenConfig *cfg, int screen_idx) {
    GtkWidget *win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_set_keep_below(GTK_WINDOW(win), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DESKTOP);

    GdkScreen *gdk_screen = gdk_screen_get_default();
    GdkDisplay *display = gdk_display_get_default();
    if (!gdk_screen) {
        int num_screens = gdk_display_get_n_screens(display);
        if (screen_idx < num_screens)
            gdk_screen = gdk_display_get_screen(display, screen_idx);
        else
            gdk_screen = gdk_display_get_default_screen(display);
    }
    if (gdk_screen)
        gtk_window_set_screen(GTK_WINDOW(win), gdk_screen);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win), box);
    GdkRGBA black = {0,0,0,1};
    gtk_widget_override_background_color(box, GTK_STATE_FLAG_NORMAL, &black);

    gtk_window_fullscreen(GTK_WINDOW(win));
    gtk_widget_show_all(win);
    return win;
}

/* Rebuild black overlays for all screens except the chosen one */
static void update_black_overlays(ScreenConfig *cfg, int chosen_idx) {
    /* Destroy existing black windows */
    g_list_free_full(app.black_windows, (GDestroyNotify)gtk_widget_destroy);
    app.black_windows = NULL;

    if (!cfg || cfg->n < 2) return;

    for (int i = 0; i < cfg->n; i++) {
        if (i == chosen_idx) continue;
        GtkWidget *w = create_black_overlay(cfg, i);
        app.black_windows = g_list_append(app.black_windows, w);
    }
}

/* Switch display: re‑run selection and reconfigure */
static void switch_display(void) {
    ScreenConfig scfg;
    if (!detect_screens(&scfg)) {
        printf("[display] Cannot detect screens\n");
        return;
    }
    if (scfg.n < 2) {
        printf("[display] Only one screen, cannot switch\n");
        free_screens(&scfg);
        return;
    }

    int chosen = choose_screen_dialog(&scfg);
    if (chosen < 0) {
        free_screens(&scfg);
        return;
    }

    /* Save choice */
    save_screen_choice(chosen);

    /* Update app screen geometry */
    app.scr_x = scfg.info[chosen].x_org;
    app.scr_y = scfg.info[chosen].y_org;
    app.scr_w = scfg.info[chosen].width;
    app.scr_h = scfg.info[chosen].height;

    /* Move main window to the chosen screen */
    gtk_window_move(GTK_WINDOW(app.win), app.scr_x, app.scr_y);
    gtk_window_resize(GTK_WINDOW(app.win), app.scr_w, app.scr_h);
    gtk_window_fullscreen(GTK_WINDOW(app.win));

    /* Rebuild black overlays */
    update_black_overlays(&scfg, chosen);

    free_screens(&scfg);
    printf("[display] Switched to screen %d: %dx%d at %d,%d\n",
           chosen, app.scr_w, app.scr_h, app.scr_x, app.scr_y);
}

/* ===========================================================
   Volume OSD creation
   =========================================================== */

static GtkWidget *create_volume_osd(void) {
    GtkWidget *win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DESKTOP);
    gtk_window_set_default_size(GTK_WINDOW(win), 200, 40);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_add(GTK_CONTAINER(win), box);
    GtkWidget *label = gtk_label_new("🔊 Volume");
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    GtkWidget *progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress), TRUE);
    gtk_box_pack_start(GTK_BOX(box), progress, TRUE, TRUE, 0);
    gtk_widget_set_halign(progress, GTK_ALIGN_FILL);
    gtk_widget_set_valign(progress, GTK_ALIGN_CENTER);

    GdkRGBA bg = {0.1,0.1,0.1,0.7};
    gtk_widget_override_background_color(box, GTK_STATE_FLAG_NORMAL, &bg);
    gtk_widget_set_opacity(box, 0.8);

    gtk_widget_show_all(win);
    gtk_widget_hide(win);
    return win;
}

/* ===========================================================
   main
   =========================================================== */

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-d") == 0)
            ctrl_debug = true;
    }

    long ram_mb = total_ram_mb();
    low_mem = ram_mb > 0 && ram_mb < 1536;

    gtk_init(&argc, &argv);
    gst_init(&argc, &argv);

    memset(&app, 0, sizeof app);
    app.cec_fd = -1;

    printf("[yt] Platform: %s, %ld MiB RAM%s\n",
           is_arm ? "arm" : "x86", ram_mb,
           low_mem ? " (low-memory mode: no GPU compositing)" : "");

    app.x_dpy = XOpenDisplay(NULL);
    if (app.x_dpy) {
        int ev, er, maj, min;
        if (XTestQueryExtension(app.x_dpy, &ev, &er, &maj, &min)) {
            xtest_ok = true;
            printf("[x11] XTest available (v%d.%d)\n", maj, min);
        } else {
            printf("[x11] XTest not available\n");
        }
    }

    ScreenConfig scfg;
    if (!detect_screens(&scfg)) {
        fprintf(stderr, "No screens detected\n");
        return 1;
    }

    int chosen_idx = -1;
    if (scfg.n > 1) {
        /* Try to load saved choice */
        int saved = load_saved_screen();
        if (saved >= 0 && saved < scfg.n) {
            chosen_idx = saved;
            printf("[display] Using saved screen %d\n", chosen_idx);
        } else {
            chosen_idx = choose_screen_dialog(&scfg);
            if (chosen_idx < 0) {
                free_screens(&scfg);
                return 0;  /* user cancelled */
            }
            save_screen_choice(chosen_idx);
        }
    } else {
        chosen_idx = 0;
    }

    app.scr_x = scfg.info[chosen_idx].x_org;
    app.scr_y = scfg.info[chosen_idx].y_org;
    app.scr_w = scfg.info[chosen_idx].width;
    app.scr_h = scfg.info[chosen_idx].height;

    printf("Screen %d: %dx%d @ %d,%d%s\n", chosen_idx,
           app.scr_w, app.scr_h, app.scr_x, app.scr_y,
           scfg.n > 1 ? " (chosen)" : "");

    /* Create black overlays for other screens */
    if (scfg.n > 1) {
        update_black_overlays(&scfg, chosen_idx);
    }

    /* Main window */
    app.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.win), "YouTube TV");
    gtk_window_set_default_size(GTK_WINDOW(app.win), app.scr_w, app.scr_h);
    gtk_window_move(GTK_WINDOW(app.win), app.scr_x, app.scr_y);
    g_signal_connect(app.win, "delete-event", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *ov = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(app.win), ov);
    app.overlay = ov;

    /* Webview as main child (hidden until video ends or preload kicks in) */
    GtkWidget *wv = make_webview();
    gtk_container_add(GTK_CONTAINER(ov), wv);
    gtk_widget_hide(wv);

    /* Loading label overlay (hidden until preload) */
    app.loading_label = gtk_label_new("");
    gtk_label_set_markup(GTK_LABEL(app.loading_label),
        "<span size='30000' color='#666'>Loading YouTube TV…</span>");
    gtk_widget_set_halign(app.loading_label, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(app.loading_label, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(ov), app.loading_label);
    gtk_widget_hide(app.loading_label);

    /* Volume OSD (hidden initially) */
    app.volume_osd = create_volume_osd();

    gtk_widget_show_all(app.win);
    gtk_window_fullscreen(GTK_WINDOW(app.win));

    g_signal_connect(app.win, "key-press-event",   G_CALLBACK(on_key), NULL);
    g_signal_connect(app.win, "key-release-event", G_CALLBACK(on_key), NULL);

    update_xid();
    g_signal_connect(app.win, "configure-event", G_CALLBACK(update_xid), NULL);

    /* Give the X server explicit input focus right away so the USB
       keyboard (Q / hold-Esc) works even with no window manager,
       and even if the startup video never finishes. */
    win_focus();

    load_youtube_tv();
    focus_webview();

    start_cec();
    start_controller();

    printf("[yt] Ready — Keyboard: Space/K=Play/Pause  J/L=Seek  Arrows=Navigate  "
           "Enter=Select  F9/F10=Volume  M=Mute  R=Like  G=Guide  "
           "Y=YouTube  B=Back 0-9=Nums  Q=Quit  F12=Inspector\n");
    printf("[yt] Press Ctrl+Shift+D to switch display\n");

    gtk_main();

    /* Cleanup */
    stop_cec();
    stop_controller();
    if (app.x_dpy)        XCloseDisplay(app.x_dpy);

    g_list_free_full(app.black_windows, (GDestroyNotify)gtk_widget_destroy);
    if (app.volume_osd) gtk_widget_destroy(app.volume_osd);

    return 0;
}
