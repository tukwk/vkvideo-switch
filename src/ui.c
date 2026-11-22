#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype.h"

#include "vendor/stb_image.h"

#include "font_bin.h"
#include "http.h"

#define W 1280
#define H 720

#define CARD_W 384
#define CARD_H 264
#define THUMB_H 216
#define GRID_X 40
#define GRID_Y 132
#define GAP_X 24
#define GAP_Y 16

#define THUMB_MAX 24

#define COL(r, g, b) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | 0xFF000000u)
#define C_BG    COL(14, 14, 18)
#define C_CARD  COL(28, 28, 34)
#define C_CARD2 COL(40, 40, 48)
#define C_TEXT  COL(240, 240, 245)
#define C_MUT   COL(150, 150, 166)
#define C_ACC   COL(77, 141, 255)
#define C_DARK  COL(0, 0, 0)
#define C_OK    COL(80, 200, 120)

typedef struct {
    int owner_id;
    int id;
    u8 *pix;
    int lru;
} thumb_t;

static Framebuffer g_fb;
static u32 *g_px;
static u32 g_stride;
static int g_fb_ok;

static stbtt_fontinfo g_font;
static int g_font_ok;

static thumb_t g_thumbs[THUMB_MAX];
static int g_lru;
static int g_quality = 720;

/* ---- primitives --------------------------------------------------------- */

static inline void put_px(int x, int y, u32 c)
{
    if (x >= 0 && x < W && y >= 0 && y < H)
        g_px[(size_t)y * g_stride + (size_t)x] = c;
}

static void fill(int x, int y, int w, int h, u32 c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0)
        return;
    for (int yy = y; yy < y + h; yy++) {
        u32 *row = g_px + (size_t)yy * g_stride + x;
        for (int xx = 0; xx < w; xx++)
            row[xx] = c;
    }
}

static void border(int x, int y, int w, int h, int t, u32 c)
{
    fill(x, y, w, t, c);
    fill(x, y + h - t, w, t, c);
    fill(x, y, t, h, c);
    fill(x + w - t, y, t, h, c);
}

/* ---- text --------------------------------------------------------------- */

static int utf8_next(const char **sp)
{
    const unsigned char *s = (const unsigned char *)*sp;
    int c = *s++;
    int extra;

    if (c < 0x80) {
        *sp = (const char *)s;
        return c;
    }
    if ((c & 0xE0) == 0xC0) { c &= 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; extra = 3; }
    else { *sp = (const char *)s; return '?'; }

    for (int i = 0; i < extra && (*s & 0xC0) == 0x80; i++)
        c = (c << 6) | (*s++ & 0x3F);

    *sp = (const char *)s;
    return c;
}

static float font_scale(int px)
{
    return stbtt_ScaleForPixelHeight(&g_font, (float)px);
}

static int glyph_adv(int cp, float scale)
{
    int adv = 0;
    stbtt_GetCodepointHMetrics(&g_font, cp, &adv, NULL);
    return (int)lroundf((float)adv * scale);
}

static int text_w(int px, const char *s)
{
    if (!g_font_ok)
        return (int)strlen(s) * px / 2;
    float scale = font_scale(px);
    int w = 0, prev = 0;
    while (*s) {
        int cp = utf8_next(&s);
        if (prev)
            w += (int)lroundf((float)stbtt_GetCodepointKernAdvance(&g_font, prev, cp) * scale);
        w += glyph_adv(cp, scale);
        prev = cp;
    }
    return w;
}

static void blend_glyph(int x, int y, const u8 *bmp, int bw, int bh, u32 color)
{
    u8 cr = (u8)(color & 0xFF);
    u8 cg = (u8)((color >> 8) & 0xFF);
    u8 cb = (u8)((color >> 16) & 0xFF);

    for (int yy = 0; yy < bh; yy++) {
        int py = y + yy;
        if (py < 0 || py >= H)
            continue;
        for (int xx = 0; xx < bw; xx++) {
            int px = x + xx;
            if (px < 0 || px >= W)
                continue;
            unsigned a = bmp[(size_t)yy * bw + xx];
            if (!a)
                continue;
            u8 *d = (u8 *)&g_px[(size_t)py * g_stride + px];
            d[0] = (u8)((d[0] * (255 - a) + cr * a) / 255);
            d[1] = (u8)((d[1] * (255 - a) + cg * a) / 255);
            d[2] = (u8)((d[2] * (255 - a) + cb * a) / 255);
            d[3] = 0xFF;
        }
    }
}

static void text(int x, int y, int px, const char *s, u32 color)
{
    if (!g_font_ok || !s)
        return;

    float scale = font_scale(px);
    int ascent, descent, linegap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &linegap);
    int baseline = y + (int)lroundf((float)ascent * scale);

    int pen = x;
    int prev = 0;

    while (*s) {
        int cp = utf8_next(&s);
        if (prev)
            pen += (int)lroundf((float)stbtt_GetCodepointKernAdvance(&g_font, prev, cp) * scale);

        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&g_font, cp, scale, scale, &x0, &y0, &x1, &y1);
        int bw = x1 - x0;
        int bh = y1 - y0;

        if (bw > 0 && bh > 0 && bw < 128 && bh < 128) {
            u8 bmp[128 * 128];
            stbtt_MakeCodepointBitmap(&g_font, bmp, bw, bh, bw, scale, scale, cp);
            blend_glyph(pen + x0, baseline + y0, bmp, bw, bh, color);
        }

        pen += glyph_adv(cp, scale);
        prev = cp;
    }
}

static void text_center(int cx, int y, int px, const char *s, u32 color)
{
    text(cx - text_w(px, s) / 2, y, px, s, color);
}

/* Greedy word wrap with an ellipsis on the last line. */
static void text_wrapped(int x, int y, int maxw, int px, int maxlines,
                         const char *s, u32 color)
{
    if (!g_font_ok)
        return;

    int line = 0;
    int cy = y;

    while (*s && line < maxlines) {
        while (*s == ' ')
            s++;

        const char *fit_end = s;
        const char *last_space = NULL;
        const char *q = s;
        int w = 0;

        while (*q) {
            const char *before = q;
            int cp = utf8_next(&q);
            if (cp == ' ')
                last_space = q;
            int adv = glyph_adv(cp, font_scale(px));
            if (w + adv > maxw) {
                fit_end = before;
                break;
            }
            w += adv;
            fit_end = q;
        }

        if (*q && last_space && last_space > s && last_space <= fit_end)
            fit_end = last_space;

        if (fit_end == s) {
            utf8_next(&s); /* glyph wider than the line: skip it */
            continue;
        }

        int is_last = (line == maxlines - 1);
        if (is_last && *fit_end) {
            /* Draw what fits, then an ellipsis. */
            char buf[512];
            size_t n = (size_t)(fit_end - s);
            if (n >= sizeof(buf))
                n = sizeof(buf) - 1;
            memcpy(buf, s, n);
            buf[n] = '\0';

            int tw = text_w(px, buf);
            while (n > 0 && tw + text_w(px, "…") > maxw) {
                buf[--n] = '\0';
                tw = text_w(px, buf);
            }
            text(x, cy, px, buf, color);
            text(x + tw, cy, px, "…", color);
            return;
        }

        char buf[512];
        size_t n = (size_t)(fit_end - s);
        if (n >= sizeof(buf))
            n = sizeof(buf) - 1;
        memcpy(buf, s, n);
        buf[n] = '\0';
        text(x, cy, px, buf, color);

        s = (*fit_end == ' ') ? fit_end + 1 : fit_end;
        cy += px + 4;
        line++;
    }
}

/* ---- thumbnails --------------------------------------------------------- */

static void thumbs_free(void)
{
    for (int i = 0; i < THUMB_MAX; i++) {
        free(g_thumbs[i].pix);
        memset(&g_thumbs[i], 0, sizeof(g_thumbs[i]));
    }
}

static thumb_t *thumb_find(const feed_item_t *it)
{
    for (int i = 0; i < THUMB_MAX; i++)
        if (g_thumbs[i].owner_id == it->owner_id &&
            g_thumbs[i].id == it->id)
            return &g_thumbs[i];
    return NULL;
}

static thumb_t *thumb_slot(void)
{
    for (int i = 0; i < THUMB_MAX; i++)
        if (!g_thumbs[i].pix && !g_thumbs[i].owner_id)
            return &g_thumbs[i];

    int victim = 0;
    for (int i = 1; i < THUMB_MAX; i++)
        if (g_thumbs[i].lru < g_thumbs[victim].lru)
            victim = i;
    free(g_thumbs[victim].pix);
    memset(&g_thumbs[victim], 0, sizeof(g_thumbs[victim]));
    return &g_thumbs[victim];
}

int ui_thumb_ready(const feed_item_t *it)
{
    thumb_t *t = thumb_find(it);
    if (t) {
        t->lru = ++g_lru;
        return 1;
    }
    return 0;
}

static void scale_fill(const u8 *src, int sw, int sh, u8 *dst, int dw, int dh)
{
    double sx = (double)sw / dw;
    double sy = (double)sh / dh;
    double scale = sx > sy ? sx : sy;

    int cw = (int)(dw * scale);
    int ch = (int)(dh * scale);
    int ox = (sw - cw) / 2;
    int oy = (sh - ch) / 2;

    for (int y = 0; y < dh; y++) {
        int syy = oy + (int)(y * scale);
        if (syy < 0) syy = 0;
        if (syy >= sh) syy = sh - 1;
        for (int x = 0; x < dw; x++) {
            int sxx = ox + (int)(x * scale);
            if (sxx < 0) sxx = 0;
            if (sxx >= sw) sxx = sw - 1;
            memcpy(dst + ((size_t)y * dw + x) * 4,
                   src + ((size_t)syy * sw + sxx) * 4, 4);
        }
    }
}

int ui_thumb_load(const feed_item_t *it, char *err, size_t errcap)
{
    if (thumb_find(it))
        return 0;

    /* Reserve the slot first: failed thumbnails must not be retried forever. */
    thumb_t *slot = thumb_slot();
    if (!slot)
        return 0;
    slot->owner_id = it->owner_id;
    slot->id = it->id;
    slot->pix = NULL;
    slot->lru = ++g_lru;

    if (!it->image[0])
        return 0;

    unsigned char *buf = NULL;
    size_t len = 0;
    if (http_get_mem(it->image, &buf, &len, 3 * 1024 * 1024, err, errcap) != 0)
        return 0;

    int w = 0, h = 0, comp = 0;
    unsigned char *img = stbi_load_from_memory(buf, (int)len, &w, &h, &comp, 4);
    free(buf);
    if (!img) {
        snprintf(err, errcap, "image decode failed");
        return 0;
    }

    u8 *pix = malloc((size_t)CARD_W * THUMB_H * 4);
    if (!pix) {
        free(img);
        snprintf(err, errcap, "out of memory (thumb)");
        return 0;
    }

    scale_fill(img, w, h, pix, CARD_W, THUMB_H);
    free(img);
    slot->pix = pix;
    slot->lru = ++g_lru;
    return 1;
}

int ui_visible_missing(const feed_t *f, int page)
{
    int base = page * 6;
    for (int i = 0; i < 6 && base + i < f->count; i++)
        if (!ui_thumb_ready(&f->items[base + i]) && f->items[base + i].image[0])
            return 1;
    return 0;
}

/* ---- framebuffer -------------------------------------------------------- */

static void begin(void)
{
    u32 stride = 0;
    void *fb = g_fb_ok ? framebufferBegin(&g_fb, &stride) : NULL;
    g_px = fb;
    g_stride = stride / 4;
    if (g_px)
        fill(0, 0, W, H, C_BG);
}

static void end(void)
{
    if (g_px)
        framebufferEnd(&g_fb);
    g_px = NULL;
}

int ui_init(void)
{
    memset(&g_fb, 0, sizeof(g_fb));
    if (!g_font_ok) {
        int off = stbtt_GetFontOffsetForIndex(font_bin, 0);
        if (off >= 0 && stbtt_InitFont(&g_font, font_bin, off))
            g_font_ok = 1;
    }

    nwindowSetDimensions(nwindowGetDefault(), W, H);
    if (R_FAILED(framebufferCreate(&g_fb, nwindowGetDefault(), W, H,
                                   PIXEL_FORMAT_RGBA_8888, 2)))
        return -1;
    framebufferMakeLinear(&g_fb);
    g_fb_ok = 1;
    return 0;
}

int ui_suspend(void)
{
    if (g_fb_ok) {
        framebufferClose(&g_fb);
        g_fb_ok = 0;
    }
    return 0;
}

int ui_resume(void)
{
    return ui_init();
}

void ui_shutdown(void)
{
    ui_suspend();
    thumbs_free();
}

void ui_clear_thumbs(void)
{
    thumbs_free();
}

void ui_set_quality(int q)
{
    g_quality = q;
}

/* ---- screens ------------------------------------------------------------ */

static void draw_header(void)
{
    text(40, 18, 26, "VK VIDEO", C_ACC);
    text(40 + text_w(26, "VK VIDEO") + 14, 26, 15, "for Nintendo Switch", C_MUT);

    char q[32];
    snprintf(q, sizeof(q), "[-] %dp", g_quality);
    int qw = text_w(16, q) + 28;
    fill(W - 40 - qw, 16, qw, 36, C_CARD);
    text(W - 40 - qw + 14, 25, 16, q, C_TEXT);
}

static void draw_chips(const feed_t *f)
{
    int x = 40;
    int y = 62;

    for (int i = 0; i < g_feed_category_count; i++) {
        int w = text_w(15, g_feed_categories[i].title) + 28;
        if (x + w > W - 40)
            break;

        int active = (i == f->category);
        fill(x, y, w, 34, active ? C_ACC : C_CARD);
        text(x + 14, y + 8, 15, g_feed_categories[i].title,
             active ? C_DARK : C_MUT);
        x += w + 10;
    }
}

static void draw_card(const feed_item_t *it, int x, int y, int selected)
{
    fill(x, y, CARD_W, CARD_H, C_CARD);

    thumb_t *t = thumb_find(it);
    if (t && t->pix) {
        for (int yy = 0; yy < THUMB_H; yy++) {
            memcpy(g_px + (size_t)(y + yy) * g_stride + x,
                   t->pix + (size_t)yy * CARD_W * 4, (size_t)CARD_W * 4);
        }
    } else {
        fill(x, y, CARD_W, THUMB_H, C_CARD2);
        text_center(x + CARD_W / 2, y + THUMB_H / 2 - 10, 15, "no preview", C_MUT);
    }

    if (it->resume > 0 && it->duration > 0) {
        int w = (int)((long long)CARD_W * it->resume / it->duration);
        if (w > CARD_W) w = CARD_W;
        if (w < 0) w = 0;
        fill(x, y + THUMB_H - 4, w, 4, C_ACC);
    }

    if (it->duration > 0) {
        char dur[32];
        snprintf(dur, sizeof(dur), "%d:%02d", it->duration / 60, it->duration % 60);
        int w = text_w(14, dur) + 16;
        fill(x + CARD_W - w - 8, y + THUMB_H - 32, w, 24, C_DARK);
        text(x + CARD_W - w, y + THUMB_H - 30, 14, dur, C_TEXT);
    }

    text_wrapped(x + 12, y + THUMB_H + 10, CARD_W - 24, 18, 2, it->title,
                 selected ? C_ACC : C_TEXT);

    if (selected)
        border(x - 3, y - 3, CARD_W + 6, CARD_H + 6, 3, C_ACC);
}

void ui_draw_feed(const feed_t *f, int page, int selected, const char *status,
                  int loading_thumbs)
{
    if (!g_fb_ok)
        return;
    begin();
    if (!g_px) {
        end();
        return;
    }

    draw_header();
    draw_chips(f);

    int base = page * 6;
    for (int i = 0; i < 6; i++) {
        int idx = base + i;
        int col = i % 3;
        int row = i / 3;
        int x = GRID_X + col * (CARD_W + GAP_X);
        int y = GRID_Y + row * (CARD_H + GAP_Y);

        if (idx < f->count)
            draw_card(&f->items[idx], x, y, idx == base + selected);
        else
            fill(x, y, CARD_W, CARD_H, COL(20, 20, 26));
    }

    if (f->count == 0) {
        text_center(W / 2, 340, 24, "Ничего не найдено", C_TEXT);
        text_center(W / 2, 380, 17, "ZR — другая категория, X — поиск", C_MUT);
    }

    if (loading_thumbs)
        text(40, 690, 15, "Загружаю обложки...", C_MUT);
    else if (status && *status)
        text(40, 690, 15, status, C_MUT);
    else {
        char hint[256];
        snprintf(hint, sizeof(hint),
                 "A — смотреть   X — поиск   Y — токен   - — качество (%dp)   "
                 "ZL/ZR — категория   L/R — страница   + — выход",
                 g_quality);
        text(40, 690, 15, hint, C_MUT);
    }

    end();
}

void ui_draw_welcome(const char *status)
{
    if (!g_fb_ok)
        return;
    begin();
    if (!g_px) {
        end();
        return;
    }

    text_center(W / 2, 140, 54, "VK VIDEO", C_ACC);
    text_center(W / 2, 214, 22, "Видео VK на Nintendo Switch", C_TEXT);

    fill(W / 2 - 260, 290, 520, 54, C_ACC);
    text_center(W / 2, 304, 21, "Y — вставить токен VK", C_DARK);

    text_center(W / 2, 380, 17,
                "Токен нужен один раз и хранится в /switch/vkvideo/config.ini",
                C_MUT);
    text_center(W / 2, 410, 17,
                "Получить: vkhost.github.io (доступ к видео)", C_MUT);

    if (status && *status)
        text_center(W / 2, 470, 16, status, C_MUT);

    text_center(W / 2, 680, 15, "+ — выход", C_MUT);

    end();
}

void ui_draw_message(const char *title, const char *subtitle)
{
    if (!g_fb_ok)
        return;
    begin();
    if (!g_px) {
        end();
        return;
    }
    text_center(W / 2, 300, 30, title, C_TEXT);
    if (subtitle)
        text_center(W / 2, 348, 18, subtitle, C_MUT);
    end();
}

int ui_text_width(int px, const char *text)
{
    return text_w(px, text);
}

void ui_text_to(u8 *dst, u32 stride, int x, int y, int px,
                const char *str, u32 color)
{
    u32 *old_px = g_px;
    u32 old_stride = g_stride;
    g_px = (u32 *)dst;
    g_stride = stride / 4;
    text(x, y, px, str, color);
    g_px = old_px;
    g_stride = old_stride;
}

void ui_rect_to(u8 *dst, u32 stride, int x, int y, int w, int h,
                u32 color, int alpha)
{
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    u8 cr = (u8)(color & 0xFF);
    u8 cg = (u8)((color >> 8) & 0xFF);
    u8 cb = (u8)((color >> 16) & 0xFF);

    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= H)
            continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= W)
                continue;
            u8 *d = dst + (size_t)yy * stride + (size_t)xx * 4;
            d[0] = (u8)((d[0] * (255 - alpha) + cr * alpha) / 255);
            d[1] = (u8)((d[1] * (255 - alpha) + cg * alpha) / 255);
            d[2] = (u8)((d[2] * (255 - alpha) + cb * alpha) / 255);
            d[3] = 0xFF;
        }
    }
}

void ui_draw_progress(const char *title, const char *line, double frac)
{
    if (!g_fb_ok)
        return;
    begin();
    if (!g_px) {
        end();
        return;
    }

    text_center(W / 2, 280, 26, title, C_TEXT);
    if (line)
        text_center(W / 2, 322, 17, line, C_MUT);

    if (frac >= 0.0) {
        if (frac > 1.0)
            frac = 1.0;
        int bx = W / 2 - 300, by = 370, bw = 600, bh = 14;
        fill(bx, by, bw, bh, C_CARD2);
        fill(bx, by, (int)(bw * frac), bh, C_ACC);
    }

    end();
}
