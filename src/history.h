#pragma once

#include "feed.h"

/* Adds/moves a video to the top of the watch history. */
void history_add(int owner_id, int id, const char *title, const char *image,
                 int duration, double pos);

/* Updates the saved position without changing the order. */
void history_update_pos(int owner_id, int id, double pos);

/* Fills a feed with history entries (newest first). Returns item count. */
int history_fill(feed_t *f);
