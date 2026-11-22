#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __SWITCH__

#include "log.h"

#define HIST_PATH "sdmc:/switch/vkvideo/history.txt"
#define HIST_TMP  "sdmc:/switch/vkvideo/history.tmp"
#define HIST_MAX  60

typedef struct {
    int owner_id;
    int id;
    int duration;
    double pos;
    unsigned long long when;
    char title[256];
    char image[1024];
} hist_t;

static void sanitize(char *s)
{
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r')
            *s = ' ';
}

static int hist_read(hist_t *h, int max)
{
    FILE *f = fopen(HIST_PATH, "r");
    if (!f)
        return 0;

    int n = 0;
    char line[2048];
    while (n < max && fgets(line, sizeof(line), f)) {
        char *p = line;
        char *fields[7];
        int nf = 0;

        while (nf < 7 && p) {
            fields[nf++] = p;
            char *tab = strchr(p, '\t');
            if (tab) {
                *tab = '\0';
                p = tab + 1;
            } else {
                p = NULL;
            }
        }
        if (nf < 7)
            continue;

        hist_t *e = &h[n];
        memset(e, 0, sizeof(*e));
        e->owner_id = atoi(fields[0]);
        e->id = atoi(fields[1]);
        e->duration = atoi(fields[2]);
        e->pos = atof(fields[3]);
        e->when = strtoull(fields[4], NULL, 10);
        snprintf(e->title, sizeof(e->title), "%s", fields[5]);
        snprintf(e->image, sizeof(e->image), "%s", fields[6]);

        char *nl = strchr(e->image, '\n');
        if (nl) *nl = '\0';
        char *cr = strchr(e->image, '\r');
        if (cr) *cr = '\0';

        if (e->owner_id && e->id > 0)
            n++;
    }

    fclose(f);
    return n;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return -1;
    }
    char buf[4096];
    size_t n;
    int ok = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = -1; break; }
    fclose(in);
    fclose(out);
    return ok;
}

static void commit_file(const char *tmp, const char *dst)
{
    if (rename(tmp, dst) == 0)
        return;
    if (copy_file(tmp, dst) == 0) {
        remove(tmp);
        applog("history: rename fallback copy ok");
    } else {
        applog("history: write failed");
    }
}

static void hist_write(const hist_t *h, int n)
{
    FILE *f = fopen(HIST_TMP, "w");
    if (!f) {
        applog("history: cannot create temp");
        return;
    }

    for (int i = 0; i < n; i++) {
        fprintf(f, "%d\t%d\t%d\t%.3f\t%llu\t%s\t%s\n",
                h[i].owner_id, h[i].id, h[i].duration, h[i].pos,
                h[i].when, h[i].title, h[i].image);
    }

    fclose(f);
    remove(HIST_PATH);
    commit_file(HIST_TMP, HIST_PATH);
}

void history_add(int owner_id, int id, const char *title, const char *image,
                 int duration, double pos)
{
    hist_t h[HIST_MAX];
    int n = hist_read(h, HIST_MAX);

    int found = -1;
    for (int i = 0; i < n; i++) {
        if (h[i].owner_id == owner_id && h[i].id == id) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        for (int i = found; i > 0; i--)
            h[i] = h[i - 1];
    } else {
        if (n < HIST_MAX)
            n++;
        for (int i = n - 1; i > 0; i--)
            h[i] = h[i - 1];
    }

    hist_t *e = &h[0];
    memset(e, 0, sizeof(*e));
    e->owner_id = owner_id;
    e->id = id;
    e->duration = duration;
    e->pos = pos >= 0 ? pos : 0;
    e->when = (unsigned long long)time(NULL);
    snprintf(e->title, sizeof(e->title), "%s", title ? title : "");
    snprintf(e->image, sizeof(e->image), "%s", image ? image : "");
    sanitize(e->title);
    sanitize(e->image);

    hist_write(h, n);
    applog("history: add %d_%d at %.1f", owner_id, id, e->pos);
}

void history_update_pos(int owner_id, int id, double pos)
{
    hist_t h[HIST_MAX];
    int n = hist_read(h, HIST_MAX);

    for (int i = 0; i < n; i++) {
        if (h[i].owner_id == owner_id && h[i].id == id) {
            h[i].pos = pos >= 0 ? pos : 0;
            hist_write(h, n);
            return;
        }
    }
}

int history_fill(feed_t *f)
{
    hist_t h[HIST_MAX];
    int n = hist_read(h, FEED_MAX_ITEMS);
    if (n > FEED_MAX_ITEMS)
        n = FEED_MAX_ITEMS;

    f->count = 0;
    f->offset = n;
    f->end = 1;
    f->section_id[0] = '\0';

    for (int i = 0; i < n; i++) {
        feed_item_t *it = &f->items[i];
        memset(it, 0, sizeof(*it));
        it->owner_id = h[i].owner_id;
        it->id = h[i].id;
        it->duration = h[i].duration;
        it->resume = h[i].pos > 0 ? (int)h[i].pos : 0;
        snprintf(it->title, sizeof(it->title), "%s", h[i].title);
        snprintf(it->image, sizeof(it->image), "%s", h[i].image);
        f->count++;
    }

    applog("history: loaded %d entries", f->count);
    return f->count;
}

#else

void history_add(int owner_id, int id, const char *title, const char *image,
                 int duration, double pos)
{
    (void)owner_id; (void)id; (void)title; (void)image; (void)duration; (void)pos;
}

void history_update_pos(int owner_id, int id, double pos)
{
    (void)owner_id; (void)id; (void)pos;
}

int history_fill(feed_t *f)
{
    (void)f;
    return 0;
}

#endif
