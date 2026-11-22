#include <ctype.h>
#include "feed.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "history.h"
#include "net.h"
#include "vendor/cJSON.h"
#include "vk.h"

feed_category_t g_feed_categories[] = {
    { "Рекомендации", NULL,        FEED_KIND_CATALOG },
    { "История",      NULL,        FEED_KIND_HISTORY },
    { "Музыка",       "музыка",    FEED_KIND_SEARCH },
    { "Игры",         "игры",      FEED_KIND_SEARCH },
    { "Фильмы",       "фильмы",    FEED_KIND_SEARCH },
    { "Юмор",         "юмор",      FEED_KIND_SEARCH },
    { "Новости",      "новости",   FEED_KIND_SEARCH },
    { "Спорт",        "спорт",     FEED_KIND_SEARCH },
    { "Наука",        "наука",     FEED_KIND_SEARCH },
    { "Авто",         "авто",      FEED_KIND_SEARCH },
    { "Кулинария",    "кулинария", FEED_KIND_SEARCH },
};

const int g_feed_category_count =
    (int)(sizeof(g_feed_categories) / sizeof(g_feed_categories[0]));

#define PAGE_SIZE 24
#define RESP_CAP  (512 * 1024)

static char g_resp[RESP_CAP];

static void url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '+';
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
}

static int num(const cJSON *o, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(v))
        return v->valueint;
    if (cJSON_IsString(v) && v->valuestring)
        return atoi(v->valuestring);
    return def;
}

static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void pick_image(const cJSON *v, char *out, size_t cap)
{
    out[0] = '\0';

    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(v, "image");
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0)
        arr = cJSON_GetObjectItemCaseSensitive(v, "first_frame");
    if (!cJSON_IsArray(arr))
        return;

    const cJSON *best = NULL;
    int best_score = -1;

    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        const char *url = str(it, "url");
        if (!url || !*url)
            continue;
        int w = num(it, "width", 0);
        if (w <= 0)
            continue;

        int pad = num(it, "with_padding", 0);
        int score;
        if (w >= 320 && w <= 640)
            score = 1000 - (w - 320) / 8;
        else if (w > 640)
            score = 500 - (w - 640) / 64;
        else
            score = 100 - (320 - w) / 8;
        if (pad)
            score -= 300;

        if (score > best_score) {
            best_score = score;
            best = it;
        }
    }

    if (!best)
        best = cJSON_GetArrayItem(arr, 0);
    const char *url = best ? str(best, "url") : NULL;
    if (!url)
        return;

    if (url[0] == '/' && url[1] == '/')
        snprintf(out, cap, "https:%s", url);
    else
        snprintf(out, cap, "%s", url);
}

static int looks_like_video(const cJSON *o)
{
    if (!cJSON_IsObject(o))
        return 0;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(o, "id");
    const cJSON *own = cJSON_GetObjectItemCaseSensitive(o, "owner_id");
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(o, "title");
    if (!id || !own || !cJSON_IsString(title))
        return 0;
    if (!cJSON_IsNumber(id) && !cJSON_IsString(id))
        return 0;
    if (!cJSON_IsNumber(own) && !cJSON_IsString(own))
        return 0;
    return 1;
}

static int already_have(const feed_t *f, int owner_id, int id)
{
    for (int i = 0; i < f->count; i++)
        if (f->items[i].owner_id == owner_id && f->items[i].id == id)
            return 1;
    return 0;
}

static int add_video(feed_t *f, const cJSON *v)
{
    int owner_id = num(v, "owner_id", 0);
    int id = num(v, "id", 0);
    const char *title = str(v, "title");

    if (!owner_id || id <= 0 || !title || !*title)
        return 0;
    if (already_have(f, owner_id, id))
        return 0;
    if (f->count >= FEED_MAX_ITEMS)
        return 0;

    feed_item_t *it = &f->items[f->count];
    memset(it, 0, sizeof(*it));
    it->owner_id = owner_id;
    it->id = id;
    it->duration = num(v, "duration", 0);
    snprintf(it->title, sizeof(it->title), "%s", title);
    pick_image(v, it->image, sizeof(it->image));

    f->count++;
    return 1;
}

static int collect(const cJSON *node, feed_t *f)
{
    int added = 0;

    if (cJSON_IsArray(node)) {
        cJSON *it;
        cJSON_ArrayForEach(it, node)
            added += collect(it, f);
        return added;
    }
    if (!cJSON_IsObject(node))
        return 0;

    if (looks_like_video(node))
        return add_video(f, node);

    for (const cJSON *ch = node->child; ch; ch = ch->next)
        added += collect(ch, f);

    return added;
}

void feed_init(feed_t *f)
{
    memset(f, 0, sizeof(*f));
}

void feed_reset(feed_t *f, int category, const char *query)
{
    f->count = 0;
    f->offset = 0;
    f->end = 0;
    f->category = category;
    f->section_id[0] = '\0';
    f->err[0] = '\0';
    if (query)
        snprintf(f->query, sizeof(f->query), "%s", query);
    else
        f->query[0] = '\0';
}

static int request(const char *path, feed_t *f)
{
    char err[256] = {0};

    for (int attempt = 0; attempt < 3; attempt++) {
        if (net_https_get("api.vk.com", path, g_resp, sizeof(g_resp), NULL,
                          err, sizeof(err)) == 0)
            return 0;
        applog("feed: request attempt %d failed: %s", attempt + 1, err);
#ifdef __SWITCH__
        svcSleepThread(250000000ULL);
#endif
    }

    snprintf(f->err, sizeof(f->err), "Network: %.200s", err);
    return -1;
}

static void redact_copy(char *dst, size_t cap, const char *src, size_t maxlen)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && i < maxlen && o + 16 < cap; ) {
        if (strncmp(src + i, "vk1.", 4) == 0) {
            o += (size_t)snprintf(dst + o, cap - o, "vk1.<TOKEN>");
            i += 4;
            while (src[i] && (isalnum((unsigned char)src[i]) || src[i] == '.' ||
                              src[i] == '_' || src[i] == '-'))
                i++;
            continue;
        }
        if (strncmp(src + i, "access_token", 12) == 0) {
            o += (size_t)snprintf(dst + o, cap - o, "access_token=<TOKEN>");
            i += 12;
            while (src[i] && src[i] != '&' && src[i] != '"' && src[i] != ' ')
                i++;
            continue;
        }
        dst[o++] = src[i++];
    }
    dst[o] = '\0';
}

static void log_keys(const char *tag, const cJSON *o)
{
    if (!o) {
        applog("feed: %s = NULL", tag);
        return;
    }
    if (!cJSON_IsObject(o)) {
        applog("feed: %s type=%d", tag, o->type);
        return;
    }
    char buf[240];
    size_t n = 0;
    for (const cJSON *c = o->child; c && n + 2 < sizeof(buf); c = c->next)
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s,", c->string ? c->string : "?");
    applog("feed: %s keys: %s", tag, buf);
}

static void log_head(const char *tag)
{
    char tmp[161];
    size_t n = 0;
    for (const char *p = g_resp; *p && n + 16 < sizeof(tmp); p++) {
        if (strncmp(p, "vk1.", 4) == 0) {
            n += (size_t)snprintf(tmp + n, sizeof(tmp) - n, "vk1.<redacted>");
            p += 3;
        } else {
            tmp[n++] = *p;
        }
    }
    tmp[n] = '\0';
    applog("feed: %s head: %s", tag, tmp);
}

static void dump_response(void)
{
#ifdef __SWITCH__
    FILE *fp = fopen("sdmc:/switch/vkvideo/feed_last.json", "wb");
    if (fp) {
        fwrite(g_resp, 1, strlen(g_resp), fp);
        fclose(fp);
    }
#endif
}

static int parse_into(feed_t *f)
{
    dump_response();
    {
        size_t L = strlen(g_resp);
        char red[200], tail[120];
        redact_copy(red, sizeof(red), g_resp, 140);
        applog("feed: parse len=%d", (int)L);
        applog("feed: head: %s", red);
        if (L > 80) {
            redact_copy(tail, sizeof(tail), g_resp + L - 80, 80);
            applog("feed: tail: %s", tail);
        }
        applog("feed: bytes: %02x %02x %02x %02x %02x %02x",
               L > 0 ? (unsigned char)g_resp[0] : 0,
               L > 1 ? (unsigned char)g_resp[1] : 0,
               L > 2 ? (unsigned char)g_resp[2] : 0,
               L > 3 ? (unsigned char)g_resp[3] : 0,
               L > 4 ? (unsigned char)g_resp[4] : 0,
               L > 5 ? (unsigned char)g_resp[5] : 0);
    }

    cJSON *root = cJSON_Parse(g_resp);
    if (!root) {
        snprintf(f->err, sizeof(f->err), "Bad JSON from VK");
        applog("feed: parse error at %s", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "?");
        log_head("badjson");
        return -1;
    }
    log_keys("root", root);

    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (error) {
        int code = num(error, "error_code", 0);
        const char *msg = str(error, "error_msg");
        snprintf(f->err, sizeof(f->err), "VK API %d: %s", code,
                 msg ? msg : "error");
        applog("feed: api error %d: %s", code, msg ? msg : "error");
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *resp = cJSON_GetObjectItemCaseSensitive(root, "response");
    log_keys("response", resp);
    const cJSON *items = resp ? cJSON_GetObjectItemCaseSensitive(resp, "items") : NULL;
    const cJSON *count = resp ? cJSON_GetObjectItemCaseSensitive(resp, "count") : NULL;
    applog("feed: resp count=%d items=%d",
           cJSON_IsNumber(count) ? count->valueint : -1,
           cJSON_IsArray(items) ? cJSON_GetArraySize(items) : -1);

    int added = collect(root, f);
    cJSON_Delete(root);
    return added;
}

static int s_section_rot;
static int s_query_rot;

static int catalog_pick_section(feed_t *f, const app_config_t *cfg)
{
    char path[2048];
    snprintf(path, sizeof(path),
             "/method/video.getCatalog?v=5.199&access_token=%s"
             "&count=24&extended=1",
             cfg->token);
    if (request(path, f) != 0)
        return -1;

    cJSON *root = cJSON_Parse(g_resp);
    if (!root) {
        snprintf(f->err, sizeof(f->err), "Bad catalog JSON");
        return -1;
    }

    const cJSON *resp = cJSON_GetObjectItemCaseSensitive(root, "response");
    const cJSON *sections = resp ? cJSON_GetObjectItemCaseSensitive(resp, "sections") : NULL;

    char ids[24][80];
    int n = 0;
    if (cJSON_IsArray(sections)) {
        cJSON *it;
        cJSON_ArrayForEach(it, sections) {
            const char *id = str(it, "id");
            if (id && *id && n < 24)
                snprintf(ids[n++], sizeof(ids[0]), "%s", id);
        }
    }
    cJSON_Delete(root);

    if (n == 0)
        return 0;

    int idx = (s_section_rot++) % n;
    snprintf(f->section_id, sizeof(f->section_id), "%s", ids[idx]);
    applog("feed: catalog section %d/%d picked", idx + 1, n);
    return 1;
}

static int catalog_load_section(feed_t *f, const app_config_t *cfg)
{
    if (!f->section_id[0])
        return -1;

    char path[2048];
    snprintf(path, sizeof(path),
             "/method/video.getCatalogSection?v=5.199&access_token=%s"
             "&section_id=%s&count=%d&offset=%d&extended=1",
             cfg->token, f->section_id, PAGE_SIZE, f->offset);

    if (request(path, f) != 0)
        return -1;

    int added = parse_into(f);
    if (added > 0)
        f->offset += PAGE_SIZE;
    return added;
}

static const char *fallback_query(void)
{
    static const char *q[] = {
        "тренды 2026", "популярное сегодня", "новинки", "лучшее видео",
        "виральное", "подборки", "музыка 2026", "игры 2026"
    };
    int n = (int)(sizeof(q) / sizeof(q[0]));
    return q[(s_query_rot++) % n];
}

int feed_load(feed_t *f, const app_config_t *cfg, int category,
              const char *query, int append)
{
    char enc[512];
    char path[2048];
    int added;

    if (!append)
        feed_reset(f, category, query);

    if (!cfg->token[0]) {
        snprintf(f->err, sizeof(f->err), "No token");
        return -1;
    }

    f->err[0] = '\0';

    int kind = (category >= 0 && category < g_feed_category_count)
                   ? g_feed_categories[category].kind
                   : FEED_KIND_SEARCH;

    if (kind == FEED_KIND_HISTORY) {
        if (!append)
            history_fill(f);
        else
            f->end = 1;
        return f->count;
    }

    const char *q = query && *query ? query
                  : (kind == FEED_KIND_SEARCH && category >= 0 &&
                     category < g_feed_category_count
                        ? g_feed_categories[category].query : NULL);

    if (kind == FEED_KIND_CATALOG && !q) {
        applog("feed: catalog offset=%d section=%.20s", f->offset, f->section_id);

        if (!append || !f->section_id[0]) {
            if (catalog_pick_section(f, cfg) < 0)
                applog("feed: catalog failed: %.120s", f->err);
        }

        for (int attempts = 0; attempts < 3 && f->section_id[0]; attempts++) {
            added = catalog_load_section(f, cfg);
            applog("feed: section added=%d count=%d err=%.120s",
                   added, f->count, f->err);
            if (added > 0)
                return added;
            if (added < 0)
                break;

            if (append) {
                f->end = 1;
                return 0;
            }

            if (catalog_pick_section(f, cfg) <= 0) {
                f->section_id[0] = '\0';
                break;
            }
        }

        q = fallback_query();
        applog("feed: fallback query '%s'", q);
    }

    if (!q)
        q = "видео";

    url_encode(q, enc, sizeof(enc));
    snprintf(path, sizeof(path),
             "/method/video.search?v=5.199&access_token=%s&q=%s"
             "&count=%d&offset=%d&extended=1&sort=2",
             cfg->token, enc, PAGE_SIZE, f->offset);

    applog("feed: search q=%.40s offset=%d", q, f->offset);
    if (request(path, f) != 0) {
        applog("feed: search request failed: %.120s", f->err);
        return -1;
    }

    added = parse_into(f);
    applog("feed: search added=%d count=%d err=%.120s", added, f->count, f->err);
    if (added < 0)
        return -1;

    if (added == 0)
        f->end = 1;
    f->offset += PAGE_SIZE;
    return added;
}
