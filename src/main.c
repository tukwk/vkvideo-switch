#include <switch.h>

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "feed.h"
#include "history.h"
#include "http.h"
#include "log.h"
#include "net.h"
#include "player.h"
#include "resume.h"
#include "ui.h"
#include "vk.h"

#define SD_ROOT     "sdmc:/switch/vkvideo"
#define CONFIG_PATH SD_ROOT "/config.ini"
#define API_HOST    "api.vk.com"

static app_config_t g_cfg;
static char g_resp[192 * 1024];
static char g_status[256];

#define LOCK_PATH SD_ROOT "/session.lock"

static void ensure_dirs(void)
{
    mkdir("sdmc:/switch", 0777);
    mkdir(SD_ROOT, 0777);
}

static void cache_cleanup(void)
{
    DIR *d = opendir(SD_ROOT);
    if (!d)
        return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        size_t l = strlen(n);
        int is_mp4 = (l > 4 && strcmp(n + l - 4, ".mp4") == 0);
        int is_ok  = (l > 3 && strcmp(n + l - 3, ".ok") == 0);
        if (!is_mp4 && !is_ok)
            continue;

        char p[512];
        snprintf(p, sizeof(p), "%s/%s", SD_ROOT, n);
        remove(p);
    }
    closedir(d);
    applog("cache cleaned");
}

static char *trim_inplace(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static int ask_text(const char *header, const char *initial, bool password,
                    char *out, size_t cap)
{
    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc))
        return -1;

    if (password) {
        swkbdConfigMakePresetPassword(&kbd);
    } else {
        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetType(&kbd, SwkbdType_All); /* includes RU layout */
    }

    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetGuideText(&kbd, header);
    if (initial && *initial)
        swkbdConfigSetInitialText(&kbd, initial);
    swkbdConfigSetStringLenMax(&kbd, (u32)(cap - 1));

    rc = swkbdShow(&kbd, out, cap);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

static int vk_fetch(const char *ref, vk_video_t *out, char *err, size_t errcap)
{
    int owner_id = 0, video_id = 0;
    char path[2048];

    if (vk_parse_ref(ref, &owner_id, &video_id) != 0) {
        snprintf(err, errcap, "bad video reference");
        return -1;
    }
    if (vk_build_get_path(path, sizeof(path), owner_id, video_id,
                          g_cfg.token) != 0) {
        snprintf(err, errcap, "request too long");
        return -1;
    }
    if (net_https_get(API_HOST, path, g_resp, sizeof(g_resp), NULL,
                      err, errcap) != 0)
        return -1;
    return vk_parse_response(g_resp, out, err, errcap);
}

static int play_item(const feed_item_t *it)
{
    char ref[64];
    char err[256] = {0};
    vk_video_t v;

    snprintf(ref, sizeof(ref), "%d_%d", it->owner_id, it->id);

    ui_draw_message("Получаю ссылку...", it->title);

    applog("play %s: video.get", ref);
    if (vk_fetch(ref, &v, err, sizeof(err)) != 0) {
        applog("video.get failed: %s", err);
        snprintf(g_status, sizeof(g_status), "VK: %.180s", err);
        return -1;
    }
    applog("video.get ok: %dx%d files=%d%d%d%d%d", v.width, v.height,
           !!v.mp4_240[0], !!v.mp4_360[0], !!v.mp4_480[0],
           !!v.mp4_720[0], !!v.mp4_1080[0]);

    if (!v.mp4_1080[0] && !v.mp4_720[0] && !v.mp4_480[0] &&
        !v.mp4_360[0] && !v.mp4_240[0]) {
        snprintf(g_status, sizeof(g_status),
                 "Только HLS/внешний поток — пока не поддерживается");
        return -1;
    }

    double start = resume_load(ref);
    if (start < 0 && it->resume > 0)
        start = it->resume;
    if (start < 0)
        start = 0;
    if (v.duration > 0 && start > v.duration - 15.0)
        start = 0;
    applog("resume position %.1f", start);

    history_add(it->owner_id, it->id, it->title, it->image,
                v.duration, start);

    int pref = g_cfg.preferred_quality;
    if (appletGetOperationMode() == AppletOperationMode_Handheld && pref > 720)
        pref = 720;

    static const int all_q[] = { 1080, 720, 480, 360, 240 };
    int nq = (int)(sizeof(all_q) / sizeof(all_q[0]));
    int first = 0;
    while (first < nq && all_q[first] > pref)
        first++;

    const char *cand[8];
    int cand_q[8];
    int n = 0;
    for (int i = first; i < nq && n < 8; i++) {
        const char *u = vk_pick_url(&v, all_q[i]);
        if (!u[0])
            continue;
        int dup = 0;
        for (int k = 0; k < n; k++)
            if (strcmp(cand[k], u) == 0)
                dup = 1;
        if (!dup) {
            cand[n] = u;
            cand_q[n] = all_q[i];
            n++;
        }
    }

    if (n == 0) {
        snprintf(g_status, sizeof(g_status), "Нет mp4-потока");
        return -1;
    }

    ui_suspend();

    int r = -1;
    double pos = start;
    double endpos = 0.0;

    for (int i = 0; i < n; i++) {
        for (int attempt = 0; attempt < 2; attempt++) {
            err[0] = '\0';
            applog("player try %d/%d q=%d attempt=%d pos=%.1f",
                   i + 1, n, cand_q[i], attempt + 1, pos);
            r = player_play(cand[i], v.title, pos, ref, &endpos, err, sizeof(err));
            if (r == PLAYER_RETRY_LOWER) {
                pos = endpos;
                break;
            }
            if (r == 0)
                break;
            if (strstr(err, "connect") == NULL)
                break;
        }

        if (r == 0)
            break;

        if (r == PLAYER_RETRY_LOWER) {
            applog("quality fallback at %.1f (was %dp)", pos, cand_q[i]);
            if (i + 1 >= n) {
                snprintf(g_status, sizeof(g_status),
                         "Не хватает производительности даже на %dp", cand_q[i]);
                r = -1;
                break;
            }
            continue;
        }

        applog("player failed on candidate %d q=%d: %s", i + 1, cand_q[i], err);
        if (strstr(err, "connect") == NULL)
            break;
    }

    ui_resume();
    applog("player end: r=%d err=%s", r, err);
    if (endpos > 3.0)
        history_update_pos(it->owner_id, it->id, endpos);

    if (r != 0)
        snprintf(g_status, sizeof(g_status), "Плеер: %.180s", err);
    else
        snprintf(g_status, sizeof(g_status), "Остановлено");

    return r;
}

static void enter_token(void)
{
    char buf[256] = {0};
    ui_suspend();
    int r = ask_text("Токен VK (права: video)", g_cfg.token, true, buf, sizeof(buf));
    ui_resume();

    if (r != 0) {
        snprintf(g_status, sizeof(g_status),
                 "Клавиатура недоступна в applet-режиме — запусти hbmenu с R");
        return;
    }

    char *val = trim_inplace(buf);
    if (val[0]) {
        snprintf(g_cfg.token, sizeof(g_cfg.token), "%s", val);
        config_save(&g_cfg, CONFIG_PATH);
        snprintf(g_status, sizeof(g_status), "Токен сохранён");
    }
}

static int enter_search(char *out, size_t cap)
{
    char buf[128] = {0};
    ui_suspend();
    int r = ask_text("Поиск видео", "", false, buf, sizeof(buf));
    ui_resume();

    if (r != 0 || !buf[0])
        return -1;

    snprintf(out, cap, "%s", trim_inplace(buf));
    return out[0] ? 0 : -1;
}

static void cycle_quality(void)
{
    static const int q[] = { 360, 480, 720, 1080 };
    int n = (int)(sizeof(q) / sizeof(q[0]));
    int idx = 0;
    for (int i = 0; i < n; i++)
        if (q[i] == g_cfg.preferred_quality)
            idx = i;
    do {
        idx = (idx + 1) % n;
    } while (appletGetOperationMode() == AppletOperationMode_Handheld &&
             q[idx] == 1080);
    g_cfg.preferred_quality = q[idx];
    config_save(&g_cfg, CONFIG_PATH);
    ui_set_quality(g_cfg.preferred_quality);
    applog("quality set to %d", g_cfg.preferred_quality);
    snprintf(g_status, sizeof(g_status), "Качество: %dp", g_cfg.preferred_quality);
}

static int load_one_thumb(feed_t *f, int page)
{
    int base = page * 6;
    for (int i = 0; i < 6 && base + i < f->count; i++) {
        feed_item_t *it = &f->items[base + i];
        if (ui_thumb_ready(it) || !it->image[0])
            continue;
        char err[128] = {0};
        applog("thumb start %d_%d", it->owner_id, it->id);
        int r = ui_thumb_load(it, err, sizeof(err));
        applog("thumb done %d_%d r=%d err=%s", it->owner_id, it->id, r, err);
        return r;
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    socketInitializeDefault();
    fsdevMountSdmc();
    ensure_dirs();

    if (access(LOCK_PATH, F_OK) == 0) {
        applog("previous exit was unclean, cleaning cache");
        cache_cleanup();
    }
    FILE *lock = fopen(LOCK_PATH, "wb");
    if (lock) {
        fputs("running\n", lock);
        fclose(lock);
    }

    applog("boot: libnx up, sdmc mounted");
    net_init();

    config_load(&g_cfg, CONFIG_PATH);
    applog("config: token=%d", g_cfg.token[0] ? 1 : 0);
    if (g_cfg.preferred_quality == 0)
        g_cfg.preferred_quality = 720;
    if (appletGetOperationMode() == AppletOperationMode_Handheld &&
        g_cfg.preferred_quality > 720)
        g_cfg.preferred_quality = 720;

    if (ui_init() != 0) {
        applog("ui_init FAILED");
        consoleInit(NULL);
        printf("Framebuffer init failed\n");
        consoleUpdate(NULL);
        svcSleepThread(5000000000ULL);
        return 1;
    }

    ui_set_quality(g_cfg.preferred_quality);

    feed_t feed;
    feed_init(&feed);
    feed.category = 0;

    int page = 0;
    int sel = 0;
    int dirty = 1;
    int loading = 0;
    char query[128] = {0};

    applog("ui ready");
    if (g_cfg.token[0]) {
        g_status[0] = '\0';
        ui_draw_message("Загружаю ленту", g_feed_categories[0].title);
        int n = feed_load(&feed, &g_cfg, 0, NULL, 0);
        applog("feed: n=%d err=%s", n, feed.err);
        if (n <= 0)
            snprintf(g_status, sizeof(g_status), "Лента: %.180s", feed.err);
        page = 0;
        sel = 0;
        dirty = 1;
    }

    padConfigureInput(1, HidNpadStyleTag_NpadFullKey |
                          HidNpadStyleTag_NpadHandheld |
                          HidNpadStyleTag_NpadJoyDual);
    PadState pad;
    padInitializeDefault(&pad);
    applog("pad configured");
    unsigned frame = 0;

    for (;;) {
        if (!appletMainLoop()) {
            if (appletGetFocusState() == AppletFocusState_Background) {
                svcSleepThread(100000000ULL);
                dirty = 1;
                continue;
            }
            break;
        }

        if (appletGetFocusState() != AppletFocusState_InFocus) {
            svcSleepThread(100000000ULL);
            dirty = 1;
            continue;
        }

        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (down & HidNpadButton_Plus)
            break;

        if (!g_cfg.token[0]) {
            if (down & HidNpadButton_Y)
                enter_token();
            if (g_cfg.token[0]) {
                ui_draw_message("Загружаю ленту", g_feed_categories[0].title);
                int n = feed_load(&feed, &g_cfg, 0, NULL, 0);
                if (n <= 0)
                    snprintf(g_status, sizeof(g_status), "Лента: %.180s", feed.err);
                page = sel = 0;
            }
            ui_draw_welcome(g_status);
            svcSleepThread(16000000ULL);
            continue;
        }

        if (down & HidNpadButton_Y) {
            char before[256];
            snprintf(before, sizeof(before), "%s", g_cfg.token);
            enter_token();
            dirty = 1;
            if (strcmp(before, g_cfg.token) != 0 && g_cfg.token[0]) {
                ui_draw_message("Загружаю ленту", g_feed_categories[feed.category].title);
                int n = feed_load(&feed, &g_cfg, feed.category,
                                  query[0] ? query : NULL, 0);
                if (n <= 0)
                    snprintf(g_status, sizeof(g_status), "Лента: %.180s", feed.err);
                page = sel = 0;
            }
        }

        if (down & HidNpadButton_X) {
            char q[128];
            if (enter_search(q, sizeof(q)) == 0) {
                snprintf(query, sizeof(query), "%s", q);
                ui_draw_message("Ищу видео", query);
                int n = feed_load(&feed, &g_cfg, feed.category, query, 0);
                if (n <= 0)
                    snprintf(g_status, sizeof(g_status), "Поиск: %.150s", feed.err);
                page = sel = 0;
                dirty = 1;
            }
        }

        if (down & HidNpadButton_ZL || down & HidNpadButton_ZR) {
            int dir = (down & HidNpadButton_ZR) ? 1 : -1;
            int cat = feed.category + dir;
            if (cat < 0)
                cat = g_feed_category_count - 1;
            if (cat >= g_feed_category_count)
                cat = 0;
            query[0] = '\0';
            ui_draw_message("Загружаю ленту", g_feed_categories[cat].title);
            int n = feed_load(&feed, &g_cfg, cat, NULL, 0);
            if (n <= 0)
                snprintf(g_status, sizeof(g_status), "Лента: %.180s", feed.err);
            page = sel = 0;
            dirty = 1;
        }

        if (down & HidNpadButton_Minus)
            cycle_quality();

        if (down & HidNpadButton_R) {
            if ((page + 1) * 6 < feed.count) {
                page++;
                sel = 0;
                dirty = 1;
            } else if (!feed.end) {
                ui_draw_message("Загружаю ещё", g_feed_categories[feed.category].title);
                int n = feed_load(&feed, &g_cfg, feed.category,
                                  query[0] ? query : NULL, 1);
                if (n > 0) {
                    page++;
                    sel = 0;
                } else if (n == 0) {
                    snprintf(g_status, sizeof(g_status), "Больше нет");
                } else {
                    snprintf(g_status, sizeof(g_status), "Лента: %.180s", feed.err);
                }
                dirty = 1;
            }
        }

        if (down & HidNpadButton_L) {
            if (page > 0) {
                page--;
                sel = 0;
                dirty = 1;
            }
        }

        int on_page = feed.count - page * 6;
        if (on_page > 6)
            on_page = 6;
        if (on_page < 0)
            on_page = 0;

        if (down & (HidNpadButton_Left | HidNpadButton_Right |
                    HidNpadButton_Up | HidNpadButton_Down))
            dirty = 1;

        if (on_page > 0) {
            if (down & HidNpadButton_Right)
                sel = (sel + 1) % on_page;
            if (down & HidNpadButton_Left)
                sel = (sel + on_page - 1) % on_page;
            if (down & HidNpadButton_Down)
                sel = (sel + 3 < on_page) ? sel + 3 : on_page - 1;
            if (down & HidNpadButton_Up)
                sel = (sel >= 3) ? sel - 3 : 0;
        }

        if ((down & HidNpadButton_A) && on_page > 0) {
            feed_item_t *it = &feed.items[page * 6 + sel];
            play_item(it);
            dirty = 1;
        }

        if (down)
            applog("input mask=%llx sel=%d page=%d",
                   (unsigned long long)down, sel, page);

        if (ui_visible_missing(&feed, page)) {
            loading = 1;
            if (load_one_thumb(&feed, page))
                dirty = 1;
        } else {
            loading = 0;
        }

        frame++;
        if (frame % 120 == 0)
            dirty = 1; /* recover after overlays / home menu */

        if (dirty) {
            ui_draw_feed(&feed, page, sel, g_status, loading);
            dirty = 0;
        }

        svcSleepThread(16000000ULL);
    }

    applog("exit");
    cache_cleanup();
    remove(LOCK_PATH);
    ui_shutdown();
    net_exit();
    return 0;
}
