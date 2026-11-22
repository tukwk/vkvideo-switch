#pragma once

#include <stddef.h>
#include <stdint.h>

#include "feed.h"

int  ui_init(void);
void ui_shutdown(void);

/* Temporary release of the framebuffer (e.g. before the video player). */
int  ui_suspend(void);
int  ui_resume(void);

void ui_clear_thumbs(void);
void ui_set_quality(int q);
int  ui_thumb_ready(const feed_item_t *it);
int  ui_thumb_load(const feed_item_t *it, char *err, size_t errcap);
int  ui_visible_missing(const feed_t *f, int page);

void ui_draw_welcome(const char *status);
void ui_draw_feed(const feed_t *f, int page, int selected, const char *status,
                  int loading_thumbs);
void ui_draw_message(const char *title, const char *subtitle);
void ui_draw_progress(const char *title, const char *line, double frac);

/* Draw directly into an RGBA8888 buffer (used by the player OSD). */
int  ui_text_width(int px, const char *text);
void ui_text_to(uint8_t *dst, uint32_t stride, int x, int y, int px,
                const char *text, uint32_t color);
void ui_rect_to(uint8_t *dst, uint32_t stride, int x, int y, int w, int h,
                uint32_t color, int alpha);
