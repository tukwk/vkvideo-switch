#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "feed.h"
#include "net.h"

static const char *g_catalog;
static const char *g_section;
static const char *g_search;
static char g_last_path[2048];

int net_https_get(const char *host, const char *path,
                  char *out, size_t cap, size_t *out_len,
                  char *err, size_t errcap)
{
    (void)host;
    (void)err;
    (void)errcap;
    snprintf(g_last_path, sizeof(g_last_path), "%s", path);

    const char *src = g_search;
    if (strstr(path, "getCatalogSection"))
        src = g_section;
    else if (strstr(path, "getCatalog"))
        src = g_catalog;

    snprintf(out, cap, "%s", src);
    if (out_len)
        *out_len = strlen(out);
    return 0;
}

static const char *CATALOG_REPLY =
    "{\"response\":{\"sections\":["
    "{\"id\":\"SEC_ALL\",\"name\":\"Все\",\"url\":\"https://vkvideo.ru/video\"},"
    "{\"id\":\"SEC_MUSIC\",\"name\":\"Музыка\",\"url\":\"https://vkvideo.ru/video/music\"}"
    "]}}";

static const char *SECTION_REPLY =
    "{\"response\":{\"section\":{\"id\":\"SEC_ALL\",\"name\":\"Все\","
    "\"blocks\":[{\"videos\":["
    "{\"id\":11,\"owner_id\":-7,\"title\":\"Из каталога\",\"duration\":42,"
    "\"image\":[{\"url\":\"https://img/cat.jpg\",\"width\":320,\"height\":180}]},"
    "{\"id\":12,\"owner_id\":-7,\"title\":\"Ещё видео\",\"duration\":7,"
    "\"image\":[{\"url\":\"https://img/cat2.jpg\",\"width\":480,\"height\":270}]}"
    "]}]}}}";

static const char *SEARCH_REPLY =
    "{\"response\":{\"count\":1,\"items\":["
    "{\"type\":\"video\",\"id\":456239017,\"owner_id\":-123,"
    "\"title\":\"Поиск\",\"duration\":754,"
    "\"image\":[{\"url\":\"https://img/320.jpg\",\"width\":320,\"height\":180}]}"
    "]}}";

static void test_catalog(void)
{
    app_config_t cfg;
    feed_t f;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.token, sizeof(cfg.token), "tok");

    feed_init(&f);
    g_catalog = CATALOG_REPLY;
    g_section = SECTION_REPLY;
    g_search = SEARCH_REPLY;
    g_last_path[0] = '\0';

    int n = feed_load(&f, &cfg, 0, NULL, 0);
    assert(n == 2);
    assert(f.count == 2);
    assert(strcmp(f.items[0].title, "Из каталога") == 0);
    assert(strcmp(f.items[0].image, "https://img/cat.jpg") == 0);
    assert(strstr(g_last_path, "video.getCatalogSection") != NULL);
    assert(strstr(g_last_path, "section_id=SEC_ALL") != NULL);
}

static void test_search(void)
{
    app_config_t cfg;
    feed_t f;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.token, sizeof(cfg.token), "tok");

    feed_init(&f);
    g_catalog = CATALOG_REPLY;
    g_section = SECTION_REPLY;
    g_search = SEARCH_REPLY;
    g_last_path[0] = '\0';

    int n = feed_load(&f, &cfg, 2, NULL, 0);
    assert(n == 1);
    assert(f.count == 1);
    assert(strcmp(f.items[0].title, "Поиск") == 0);
    assert(strstr(g_last_path, "video.search") != NULL);
    assert(strstr(g_last_path, "%D0%BC%D1%83%D0%B7%D1%8B%D0%BA%D0%B0") != NULL);
}

static void test_api_error(void)
{
    app_config_t cfg;
    feed_t f;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.token, sizeof(cfg.token), "tok");

    feed_init(&f);
    g_catalog = "{\"error\":{\"error_code\":5,\"error_msg\":\"auth failed\"}}";
    g_section = g_catalog;
    g_search = g_catalog;

    int n = feed_load(&f, &cfg, 3, NULL, 0);
    assert(n < 0);
    assert(strstr(f.err, "auth failed") != NULL);
}

int main(void)
{
    test_catalog();
    test_search();
    test_api_error();
    puts("feed tests: OK");
    return 0;
}
