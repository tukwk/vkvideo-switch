#pragma once

#include "config.h"

#define FEED_MAX_ITEMS 60
#define FEED_IMAGE_URL 1024
#define FEED_TITLE_LEN 256

typedef struct {
    int owner_id;
    int id;
    int duration;
    int resume;
    char title[FEED_TITLE_LEN];
    char image[FEED_IMAGE_URL];
} feed_item_t;

typedef struct {
    feed_item_t items[FEED_MAX_ITEMS];
    int count;
    int offset;
    int category;
    char query[128];
    int end;
    char section_id[80];
    char err[256];
} feed_t;

enum {
    FEED_KIND_CATALOG = 1,
    FEED_KIND_SEARCH = 2,
    FEED_KIND_HISTORY = 3,
};

typedef struct {
    const char *title;
    const char *query;
    int kind;
} feed_category_t;

extern feed_category_t g_feed_categories[];
extern const int g_feed_category_count;

void feed_init(feed_t *f);
void feed_reset(feed_t *f, int category, const char *query);

/* Loads one page. append=0 resets the list first. Returns number of new items
 * (>=0) or -1 on error (f->err). */
int feed_load(feed_t *f, const app_config_t *cfg, int category,
              const char *query, int append);
