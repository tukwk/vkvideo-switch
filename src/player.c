#include "player.h"

#include <stdarg.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "log.h"
#include "resume.h"
#include "stream.h"
#include "ui.h"

#define FB_WIDTH  1280
#define FB_HEIGHT 720

#define ABUF_COUNT 6
#define ABUF_BYTES 0x20000

#define VQ_MAX 32
#define VQ_LOW 6

#define COL(r, g, b) ((u32)(r) | ((u32)(g) << 8) | ((u32)(b) << 16) | 0xFF000000u)
#define C_WHITE COL(245, 245, 250)
#define C_DIM   COL(160, 160, 170)
#define C_ACC   COL(77, 141, 255)
#define C_BLACK COL(0, 0, 0)
#define C_BAR   COL(40, 40, 48)

typedef struct {
    AVFormatContext *fmt;
    AVCodecContext  *vctx;
    AVCodecContext  *actx;
    struct SwsContext *sws;
    SwrContext      *swr;
    int vindex;
    int aindex;
    AVRational vtb;
    AVRational atb;

    int dw, dh, x0, y0;
    u32 stride;
    u8 *canvas;
    Framebuffer fb;

    stream_t *stream;
    AVIOContext *avio;

    double duration;
    double pos;
    double vbase;
    u64 last_us;
    u64 osd_until;
    u64 last_present;
    int paused;
    int buffering;
    int hw_decoding;
    int starving;
    u64 last_video_us;
    double audio_base;
    double trim_until;
    double last_ac;
    u64 last_ac_us;
    float volume;
    char msg[48];
} player_t;

/* ---- audio -------------------------------------------------------------- */

static AudioOutBuffer s_buf[ABUF_COUNT];
static u8 *s_data[ABUF_COUNT];
static int s_free[ABUF_COUNT];
static u32 s_frames[ABUF_COUNT];
static int s_order[ABUF_COUNT];
static int s_order_n;
static int s_fill = -1;
static u32 s_used;
static u64 s_written;
static u32 s_pending;
static u32 s_rate = 48000;
static u32 s_ch = 2;
static int s_aud_ready;

static int audio_free_slot(void)
{
    for (int i = 0; i < ABUF_COUNT; i++)
        if (s_free[i])
            return i;
    return -1;
}

static int audio_open(void)
{
    if (R_FAILED(audoutInitialize()))
        return 0;
    s_rate = audoutGetSampleRate();
    s_ch = audoutGetChannelCount();
    s_aud_ready = 1;
    audoutStartAudioOut();
    applog("audio: opened rate=%u ch=%u", s_rate, s_ch);
    return 1;
}

static void audio_close(void)
{
    if (s_aud_ready) {
        audoutExit();
        s_aud_ready = 0;
    }
}

static void audio_flush_current(void)
{
    if (s_fill < 0 || s_used == 0) {
        s_fill = -1;
        s_used = 0;
        return;
    }

    u32 frames = s_used / (s_ch * 2);
    AudioOutBuffer *b = &s_buf[s_fill];
    b->next = NULL;
    b->buffer = s_data[s_fill];
    b->buffer_size = ABUF_BYTES;
    b->data_size = s_used;
    b->data_offset = 0;

    if (R_SUCCEEDED(audoutAppendAudioOutBuffer(b))) {
        s_frames[s_fill] = frames;
        s_free[s_fill] = 0;
        s_written += frames;
        s_pending += frames;
        if (s_order_n < ABUF_COUNT)
            s_order[s_order_n++] = s_fill;
    } else {
        s_free[s_fill] = 1;
    }

    s_fill = -1;
    s_used = 0;
}

static void audio_release(void)
{
    if (!s_aud_ready)
        return;

    AudioOutBuffer *rel = NULL;
    u32 count = 0;
    if (R_FAILED(audoutWaitPlayFinish(&rel, &count, 0)) || count == 0)
        return;

    if (count > (u32)s_order_n)
        count = (u32)s_order_n;

    for (u32 k = 0; k < count; k++) {
        int idx = s_order[0];
        for (int i = 1; i < s_order_n; i++)
            s_order[i - 1] = s_order[i];
        s_order_n--;

        if (!s_free[idx]) {
            if (s_pending >= s_frames[idx])
                s_pending -= s_frames[idx];
            s_frames[idx] = 0;
            s_free[idx] = 1;
        }
    }
}

static void audio_feed(const u8 *data, u32 bytes)
{
    if (!s_aud_ready)
        return;
    while (bytes > 0) {
        if (s_fill < 0) {
            int slot = audio_free_slot();
            if (slot < 0)
                return;
            s_fill = slot;
            s_used = 0;
        }

        u32 space = ABUF_BYTES - s_used;
        u32 n = bytes < space ? bytes : space;
        memcpy(s_data[s_fill] + s_used, data, n);
        s_used += n;
        data += n;
        bytes -= n;

        if (s_used == ABUF_BYTES)
            audio_flush_current();
    }
}

/* Decode an audio frame into our buffer, trimming samples that belong
 * before trim_until (needed after a seek to a keyframe). */
static void audio_emit(player_t *p, AVFrame *frame, u8 **abuf, int *cap)
{
    int out_samples = swr_get_out_samples(p->swr, frame->nb_samples);
    if (out_samples <= 0)
        return;
    int need = out_samples * 4;
    if (need > *cap) {
        u8 *nb = realloc(*abuf, (size_t)need);
        if (!nb)
            return;
        *abuf = nb;
        *cap = need;
    }
    int got = swr_convert(p->swr, abuf, out_samples,
                          (const uint8_t **)frame->extended_data,
                          frame->nb_samples);
    if (got <= 0)
        return;

    int skip = 0;
    if (p->trim_until > 0) {
        int64_t ts = frame->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE)
            ts = frame->pts;
        if (ts != AV_NOPTS_VALUE && p->atb.den) {
            double ft = ts * av_q2d(p->atb);
            if (ft < p->trim_until)
                skip = (int)((p->trim_until - ft) * (double)s_rate);
        }
    }
    if (skip > 0) {
        if (skip >= got)
            return;
        got -= skip;
        memmove(*abuf, *abuf + (size_t)skip * 4, (size_t)got * 4);
    }
    audio_feed(*abuf, (u32)got * 4);
}

static void audio_flush_tail(void)
{
    if (s_fill < 0 || s_used == 0)
        return;

    u32 rem = s_used % 0x1000;
    if (rem) {
        u32 pad = 0x1000 - rem;
        if (s_used + pad <= ABUF_BYTES) {
            memset(s_data[s_fill] + s_used, 0, pad);
            s_used += pad;
        }
    }
    audio_flush_current();
}

static void audio_reset(player_t *p)
{
    if (s_aud_ready) {
        audoutStopAudioOut();
        bool flushed = false;
        audoutFlushAudioOutBuffers(&flushed);

        AudioOutBuffer *rel = NULL;
        u32 cnt = 0;
        for (int i = 0; i < ABUF_COUNT; i++) {
            if (R_FAILED(audoutWaitPlayFinish(&rel, &cnt, 0)) || cnt == 0)
                break;
        }

        audoutStartAudioOut();
        applog("audio: soft reset flushed=%d", flushed);
    }

    if (p)
        p->audio_base = p->pos;
    if (p && p->swr)
        swr_init(p->swr);

    s_fill = -1;
    s_used = 0;
    s_written = 0;
    s_pending = 0;
    s_order_n = 0;
    for (int i = 0; i < ABUF_COUNT; i++) {
        s_free[i] = 1;
        s_frames[i] = 0;
    }
}


/* ---- rendering ---------------------------------------------------------- */

static void fmt_time(double t, char *buf, size_t cap)
{
    if (t < 0) t = 0;
    int s = (int)(t + 0.5);
    snprintf(buf, cap, "%d:%02d", s / 60, s % 60);
}

static void draw_osd(player_t *p, u8 *fb, u32 stride)
{
    u64 now = av_gettime();
    if (!p->paused && now > p->osd_until && !p->msg[0])
        return;

    ui_rect_to(fb, stride, 0, 654, FB_WIDTH, 66, C_BLACK, 170);

    char cur[16], dur[16], line[64];
    fmt_time(p->pos, cur, sizeof(cur));
    fmt_time(p->duration, dur, sizeof(dur));
    snprintf(line, sizeof(line), "%s / %s", cur, dur);
    ui_text_to(fb, stride, 40, 664, 20, line, C_WHITE);

    int barw = 1160;
    ui_rect_to(fb, stride, 40, 698, barw, 6, C_BAR, 255);
    if (p->duration > 0.1) {
        int fill = (int)(barw * (p->pos / p->duration));
        if (fill < 0) fill = 0;
        if (fill > barw) fill = barw;
        ui_rect_to(fb, stride, 40, 698, fill, 6, C_ACC, 255);
    }

    if (p->paused)
        ui_text_to(fb, stride, 570, 300, 40, "PAUSE", C_WHITE);

    if (p->msg[0])
        ui_text_to(fb, stride, 40, 610, 18, p->msg, C_DIM);
}

static void present(player_t *p)
{
    u32 stride = 0;
    void *fb = framebufferBegin(&p->fb, &stride);
    if (!fb)
        return;

    if (stride == p->stride) {
        memcpy(fb, p->canvas, (size_t)FB_HEIGHT * p->stride);
    } else {
        for (u32 y = 0; y < FB_HEIGHT; y++)
            memcpy((u8 *)fb + (size_t)y * stride,
                   p->canvas + (size_t)y * p->stride, p->stride);
    }

    draw_osd(p, fb, stride);
    framebufferEnd(&p->fb);
    p->last_present = av_gettime();
}

static void present_status(player_t *p, const char *text)
{
    u32 stride = 0;
    void *fb = framebufferBegin(&p->fb, &stride);
    if (!fb)
        return;
    memset(fb, 0, (size_t)stride * FB_HEIGHT);
    ui_text_to(fb, stride, 480, 340, 24, text, C_WHITE);
    framebufferEnd(&p->fb);
    p->last_present = av_gettime();
}

static void present_loading(player_t *p, unsigned long long ms, const char *text)
{
    u32 stride = 0;
    void *fb = framebufferBegin(&p->fb, &stride);
    if (!fb)
        return;

    memset(fb, 0, (size_t)stride * FB_HEIGHT);

    double t = (double)ms / 1000.0;
    int bw = 170, bh = 110;
    int cx = FB_WIDTH / 2;
    int cy = FB_HEIGHT / 2 - 60;
    int bx = (int)(cx - bw / 2 + (FB_WIDTH / 2 - bw) * 0.65 * sin(t * 1.1));
    int by = (int)(cy - bh / 2 + (FB_HEIGHT / 2 - bh) * 0.45 * sin(t * 0.8 + 1.2));
    int r = (int)(128 + 127 * sin(t * 1.0));
    int g = (int)(128 + 127 * sin(t * 1.0 + 2.1));
    int b = (int)(128 + 127 * sin(t * 1.0 + 4.2));

    ui_rect_to(fb, stride, bx, by, bw, bh, COL(r, g, b), 255);
    ui_rect_to(fb, stride, bx, by, bw, 3, COL(255, 255, 255), 200);

    int px = 48;
    int tw = ui_text_width(px, "VK");
    ui_text_to(fb, stride, bx + (bw - tw) / 2, by + 18, px, "VK",
               COL(250, 250, 255));

    int ey = by + 84;
    int blink = (ms % 3200) < 180;
    if (!blink) {
        ui_rect_to(fb, stride, bx + 55, ey, 10, 12, C_BLACK, 255);
        ui_rect_to(fb, stride, bx + 105, ey, 10, 12, C_BLACK, 255);
    } else {
        ui_rect_to(fb, stride, bx + 55, ey + 5, 10, 3, C_BLACK, 255);
        ui_rect_to(fb, stride, bx + 105, ey + 5, 10, 3, C_BLACK, 255);
    }
    ui_rect_to(fb, stride, bx + 65, ey + 30, 40, 5, C_BLACK, 255);
    if ((ms % 5000) < 400)
        ui_rect_to(fb, stride, bx + 78, ey + 35, 14, 12, COL(220, 60, 80), 255);

    double ang = t * 3.0;
    for (int i = 0; i < 8; i++) {
        double a = ang + i * 3.14159265 / 4.0;
        int dx = (int)(cos(a) * 42);
        int dy = (int)(sin(a) * 42);
        int alpha = 90 + i * 20;
        if (alpha > 255) alpha = 255;
        ui_rect_to(fb, stride, FB_WIDTH / 2 + dx - 5,
                   cy + 190 + dy - 5, 10, 10, C_ACC, alpha);
    }

    int tw2 = ui_text_width(24, text);
    ui_text_to(fb, stride, (FB_WIDTH - tw2) / 2, cy + 250, 24, text, C_WHITE);

    const char *hint = "B — отмена";
    int tw3 = ui_text_width(18, hint);
    ui_text_to(fb, stride, (FB_WIDTH - tw3) / 2, FB_HEIGHT - 70, 18, hint, C_DIM);

    framebufferEnd(&p->fb);
    p->last_present = av_gettime();
}


static void render_frame(player_t *p, AVFrame *f)
{
    p->sws = sws_getCachedContext(p->sws, f->width, f->height, f->format,
                                  p->dw, p->dh, AV_PIX_FMT_RGBA,
                                  SWS_FAST_BILINEAR, NULL, NULL, NULL);
    if (!p->sws) {
        applog("render: sws NULL fmt=%d %dx%d", f->format, f->width, f->height);
        return;
    }

    uint8_t *dst[4] = { p->canvas + (size_t)p->y0 * p->stride + (size_t)p->x0 * 4,
                        NULL, NULL, NULL };
    int dst_stride[4] = { (int)p->stride, 0, 0, 0 };

    sws_scale(p->sws, (const uint8_t *const *)f->data, f->linesize, 0,
              f->height, dst, dst_stride);

    p->buffering = 0;
    p->last_video_us = av_gettime();
    present(p);
}



static AVFrame *take_decoded_frame(player_t *p, AVFrame *src)
{
    if (p->hw_decoding && src->format == AV_PIX_FMT_NVTEGRA) {
        AVFrame *sw = av_frame_alloc();
        if (!sw)
            return NULL;
        int r = av_hwframe_transfer_data(sw, src, 0);
        if (r < 0) {
            applog("player: hwframe transfer failed %d", r);
            av_frame_free(&sw);
            return NULL;
        }
        sw->pts = src->pts;
        sw->best_effort_timestamp = src->best_effort_timestamp;
        sw->duration = src->duration;
        return sw;
    }

    AVFrame *copy = av_frame_alloc();
    if (!copy)
        return NULL;
    av_frame_move_ref(copy, src);
    return copy;
}

static double video_pts(player_t *p, AVFrame *f);

static void enqueue_decoded(player_t *p, AVFrame *src, AVFrame **vq,
                            double *vq_pts, int *vq_len, double fallback);

static void decode_video(player_t *p, AVPacket *pkt, AVFrame **vq,
                         double *vq_pts, int *vq_len)
{
    AVFrame *tmp = av_frame_alloc();
    if (!tmp)
        return;
    int vsend = avcodec_send_packet(p->vctx, pkt);
    while (vsend == AVERROR(EAGAIN)) {
        if (avcodec_receive_frame(p->vctx, tmp) < 0)
            break;
        enqueue_decoded(p, tmp, vq, vq_pts, vq_len, p->pos);
        vsend = avcodec_send_packet(p->vctx, pkt);
    }
    if (vsend >= 0) {
        while (avcodec_receive_frame(p->vctx, tmp) >= 0)
            enqueue_decoded(p, tmp, vq, vq_pts, vq_len, p->pos);
    }
    av_frame_free(&tmp);
}

static void enqueue_decoded(player_t *p, AVFrame *src, AVFrame **vq,
                            double *vq_pts, int *vq_len, double fallback)
{
    AVFrame *copy = take_decoded_frame(p, src);
    if (!copy)
        return;

    double pts = video_pts(p, copy);

    /* Skip frames that belong before the seek target (keyframe offset). */
    if (p->trim_until > 0 && pts >= 0 && pts < p->trim_until - 0.03) {
        av_frame_free(&copy);
        return;
    }

    if (*vq_len >= VQ_MAX) {
        if (pts >= 0 && pts > p->pos + 0.5) {
            av_frame_free(&copy);
            return;
        }
        av_frame_free(&vq[0]);
        for (int k = 1; k < *vq_len; k++) {
            vq[k - 1] = vq[k];
            vq_pts[k - 1] = vq_pts[k];
        }
        (*vq_len)--;
    }

    vq[*vq_len] = copy;
    vq_pts[*vq_len] = pts >= 0 ? pts : fallback;
    (*vq_len)++;
}

static double video_pts(player_t *p, AVFrame *f)
{
    int64_t ts = f->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE)
        ts = f->pts;
    if (ts == AV_NOPTS_VALUE)
        return -1.0;
    return ts * av_q2d(p->vtb);
}

static void seek_to(player_t *p, double target)
{
    if (target < 0)
        target = 0;
    if (p->duration > 0.5 && target > p->duration - 0.5)
        target = p->duration - 0.5;

    if (p->stream)
        stream_set_nonblock(p->stream, 0);
    int64_t ts = (int64_t)(target / av_q2d(p->vtb));
    int r = av_seek_frame(p->fmt, p->vindex, ts, AVSEEK_FLAG_BACKWARD);
    if (p->stream)
        stream_set_nonblock(p->stream, 1);
    applog("player: seek %.2f -> ts=%lld r=%d",
           target, (long long)ts, r);

    if (p->vctx)
        avcodec_flush_buffers(p->vctx);
    if (p->actx)
        avcodec_flush_buffers(p->actx);

    p->pos = target;
    p->vbase = target;
    p->trim_until = target;
    p->last_ac = target;
    p->last_ac_us = 0;
    audio_reset(p);
    p->starving = 0;

    p->last_us = av_gettime();
    p->last_video_us = p->last_us;
    p->osd_until = av_gettime() + 3000000ULL;
    snprintf(p->msg, sizeof(p->msg), "Перемотка: %d:%02d",
             (int)target / 60, (int)target % 60);
}

/* ---- main --------------------------------------------------------------- */

int player_play(const char *url, const char *title, double start_pos,
                const char *resume_key, double *end_pos,
                char *err, size_t errcap)
{
    (void)title;

    player_t p;
    AVPacket *pkt = NULL;
    AVPacket *held = NULL;
    int held_used = 0;
    AVFrame *frame = NULL;
    AVFrame *vq[VQ_MAX] = {0};
    double vq_pts[VQ_MAX] = {0};
    int vq_len = 0;
    int ret = -1;
    int eof = 0;
    int video_flushed = 0;
    int audio_flushed = 0;
    int has_audio = 0;
    u8 *abuf = NULL;
    int abuf_cap = 0;

    memset(&p, 0, sizeof(p));
    p.vindex = -1;
    p.aindex = -1;
    p.volume = 1.0f;
    p.buffering = 1;
    p.pos = start_pos;
    p.vbase = start_pos;
    p.audio_base = start_pos;

    if (err && errcap)
        err[0] = '\0';

    av_log_set_level(AV_LOG_WARNING);

    /* Framebuffer first, so we can show "buffering" while the stream opens. */
    nwindowSetDimensions(nwindowGetDefault(), FB_WIDTH, FB_HEIGHT);
    Result fbrc = framebufferCreate(&p.fb, nwindowGetDefault(), FB_WIDTH, FB_HEIGHT,
                                    PIXEL_FORMAT_RGBA_8888, 2);
    if (R_FAILED(fbrc)) {
        snprintf(err, errcap, "cannot create framebuffer");
        goto out;
    }
    framebufferMakeLinear(&p.fb);

    p.stride = FB_WIDTH * 4;
    p.canvas = memalign(0x1000, (size_t)p.stride * FB_HEIGHT);
    if (!p.canvas) {
        snprintf(err, errcap, "out of memory");
        goto out;
    }
    memset(p.canvas, 0, (size_t)p.stride * FB_HEIGHT);

    padConfigureInput(1, HidNpadStyleTag_NpadFullKey |
                          HidNpadStyleTag_NpadHandheld |
                          HidNpadStyleTag_NpadJoyDual);
    PadState pad;
    padInitializeDefault(&pad);

    applog("player: stream start %.120s", url);
    p.stream = stream_start(url, err, errcap);
    if (!p.stream)
        goto out;

    u64 t0 = av_gettime();
    int aborted = 0;
    while (!stream_ready(p.stream)) {
        if (!appletMainLoop()) { aborted = 1; break; }
        padUpdate(&pad);
        u64 d = padGetButtonsDown(&pad);
        if (d & (HidNpadButton_B | HidNpadButton_Plus)) { aborted = 1; break; }
        present_loading(&p, av_gettime() / 1000, "Подключаюсь к потоку...");
        svcSleepThread(33000000ULL);
        if (av_gettime() - t0 > 20000000) {
            snprintf(err, errcap, "stream timeout");
            aborted = 2;
            break;
        }
    }

    if (aborted) {
        if (aborted == 1) ret = 0;
        goto out;
    }
    if (stream_failed(p.stream)) {
        snprintf(err, errcap, "%.150s", stream_error(p.stream));
        goto out;
    }

    {
        int64_t total = stream_size(p.stream);
        int64_t target = 1024 * 1024;
        if (total > 0 && total < target) target = total;
        t0 = av_gettime();
        while (!stream_failed(p.stream) && !stream_eof(p.stream) &&
               stream_buffered(p.stream) < target) {
            if (!appletMainLoop()) { ret = 0; goto out; }
            padUpdate(&pad);
            u64 d = padGetButtonsDown(&pad);
            if (d & (HidNpadButton_B | HidNpadButton_Plus)) { ret = 0; goto out; }
            present_loading(&p, av_gettime() / 1000, "Буферизация...");
            svcSleepThread(33000000ULL);
            if (av_gettime() - t0 > 12000000)
                break;
        }
    }

    if (stream_failed(p.stream)) {
        snprintf(err, errcap, "%.150s", stream_error(p.stream));
        goto out;
    }

    unsigned char *iobuf = av_malloc(32768);
    if (!iobuf) {
        snprintf(err, errcap, "out of memory (avio)");
        goto out;
    }
    p.avio = avio_alloc_context(iobuf, 32768, 0, p.stream,
                                stream_read, NULL, stream_seek);
    if (!p.avio) {
        av_free(iobuf);
        snprintf(err, errcap, "avio_alloc_context failed");
        goto out;
    }
    p.avio->seekable = AVIO_SEEKABLE_NORMAL;

    p.fmt = avformat_alloc_context();
    if (!p.fmt) {
        snprintf(err, errcap, "avformat_alloc_context failed");
        goto out;
    }
    p.fmt->pb = p.avio;
    p.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    applog("player: opening stream container total=%lld",
           (long long)stream_size(p.stream));
    int oret = avformat_open_input(&p.fmt, NULL, NULL, NULL);
    if (oret < 0) {
        char ferr[128] = {0};
        av_strerror(oret, ferr, sizeof(ferr));
        applog("player: open_input r=%d (%s)", oret, ferr);
        snprintf(err, errcap, "ffmpeg: cannot open stream (%s)", ferr);
        goto out;
    }
    applog("player: stream info");
    if (avformat_find_stream_info(p.fmt, NULL) < 0) {
        snprintf(err, errcap, "ffmpeg: cannot read stream info");
        goto out;
    }

    p.duration = p.fmt->duration > 0 ? (double)p.fmt->duration / AV_TIME_BASE : 0.0;
    applog("player: duration=%.2f", p.duration);

    const AVCodec *vcodec = NULL;
    const AVCodec *acodec = NULL;
    p.vindex = av_find_best_stream(p.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 0);
    p.aindex = av_find_best_stream(p.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);
    applog("player: streams v=%d a=%d", p.vindex, p.aindex);
    if (p.vindex >= 0) {
        AVStream *vs = p.fmt->streams[p.vindex];
        AVRational fr = av_guess_frame_rate(p.fmt, vs, NULL);
        applog("player: vfps=%d/%d = %.2f", fr.num, fr.den,
               fr.den ? av_q2d(fr) : 0.0);
    }

    if (p.vindex < 0 || !vcodec) {
        snprintf(err, errcap, "ffmpeg: no video stream");
        goto out;
    }

    AVStream *vst = p.fmt->streams[p.vindex];
    p.vtb = vst->time_base;
    p.vctx = avcodec_alloc_context3(vcodec);
    if (!p.vctx || avcodec_parameters_to_context(p.vctx, vst->codecpar) < 0) {
        snprintf(err, errcap, "ffmpeg: video decoder init failed");
        goto out;
    }
    p.vctx->thread_count = 4;
    p.vctx->thread_type = FF_THREAD_FRAME;
    if (avcodec_open2(p.vctx, vcodec, NULL) < 0) {
        snprintf(err, errcap, "ffmpeg: cannot open video decoder");
        goto out;
    }
    p.hw_decoding = 0;
    applog("player: decoder software (nvdec off)");

    if (p.aindex >= 0 && acodec) {
        AVStream *ast = p.fmt->streams[p.aindex];
        p.atb = ast->time_base;
        p.actx = avcodec_alloc_context3(acodec);
        if (p.actx && avcodec_parameters_to_context(p.actx, ast->codecpar) >= 0 &&
            avcodec_open2(p.actx, acodec, NULL) >= 0)
            has_audio = 1;
        else {
            avcodec_free_context(&p.actx);
            p.actx = NULL;
        }
    }

    {
        int w = p.vctx->width, h = p.vctx->height;
        double sar = 1.0;
        if (p.vctx->sample_aspect_ratio.num && p.vctx->sample_aspect_ratio.den)
            sar = av_q2d(p.vctx->sample_aspect_ratio);
        double dar = (double)w / (double)h * sar;
        double target = (double)FB_WIDTH / (double)FB_HEIGHT;

        if (dar >= target) {
            p.dw = FB_WIDTH;
            p.dh = (int)(FB_WIDTH / dar + 0.5);
        } else {
            p.dh = FB_HEIGHT;
            p.dw = (int)(FB_HEIGHT * dar + 0.5);
        }
        p.dw &= ~1;
        p.dh &= ~1;
        if (p.dw <= 0 || p.dh <= 0) {
            p.dw = w;
            p.dh = h;
        }
        p.x0 = (FB_WIDTH - p.dw) / 2;
        p.y0 = (FB_HEIGHT - p.dh) / 2;
    }
    applog("player: fb ok scale=%dx%d", p.dw, p.dh);

    if (has_audio) {
        if (!audio_open()) {
            has_audio = 0;
        } else {

            AVChannelLayout out_ch;
            av_channel_layout_default(&out_ch, 2);
            if (swr_alloc_set_opts2(&p.swr, &out_ch, AV_SAMPLE_FMT_S16, s_rate,
                                    &p.actx->ch_layout, p.actx->sample_fmt,
                                    p.actx->sample_rate, 0, NULL) < 0 ||
                swr_init(p.swr) < 0) {
                av_channel_layout_uninit(&out_ch);
                snprintf(err, errcap, "audio resampler init failed");
                goto out;
            }
            av_channel_layout_uninit(&out_ch);

            for (int i = 0; i < ABUF_COUNT; i++) {
                s_data[i] = memalign(0x1000, ABUF_BYTES);
                s_free[i] = 1;
                if (!s_data[i]) {
                    snprintf(err, errcap, "out of memory (audio)");
                    goto out;
                }
            }
            abuf_cap = 65536;
            abuf = malloc(abuf_cap);
            if (!abuf) {
                snprintf(err, errcap, "out of memory (audio buf)");
                goto out;
            }
        }
    }

    if (start_pos > 0.5) {
        p.trim_until = start_pos;
        int64_t ts = (int64_t)(start_pos / av_q2d(p.vtb));
        int sr = av_seek_frame(p.fmt, p.vindex, ts, AVSEEK_FLAG_BACKWARD);
        applog("player: start seek %.2f ts=%lld r=%d", start_pos, (long long)ts, sr);
        if (p.vctx)
            avcodec_flush_buffers(p.vctx);
        if (p.actx)
            avcodec_flush_buffers(p.actx);
    }

    pkt = av_packet_alloc();
    held = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        snprintf(err, errcap, "out of memory (ffmpeg)");
        goto out;
    }

    p.buffering = 1;
    present_status(&p, "Буферизация...");

    /* Prefill a few packets. */
    for (int guard = 0; guard < 300 && !eof; guard++) {
        if (vq_len > 0 && s_pending >= (u32)(s_rate / 2))
            break;
        int rr = av_read_frame(p.fmt, pkt);
        if (rr < 0) {
            applog("player: prefill read r=%d", rr);
            eof = 1;
            break;
        }
        if (pkt->stream_index == p.vindex) {
            int vsend = avcodec_send_packet(p.vctx, pkt);
            while (vsend == AVERROR(EAGAIN)) {
                AVFrame *tmp = av_frame_alloc();
                if (!tmp)
                    break;
                if (avcodec_receive_frame(p.vctx, tmp) < 0) {
                    av_frame_free(&tmp);
                    break;
                }
                enqueue_decoded(&p, tmp, vq, vq_pts, &vq_len, p.pos);
                vsend = avcodec_send_packet(p.vctx, pkt);
            }
            if (vsend >= 0) {
                while (avcodec_receive_frame(p.vctx, frame) >= 0)
                    enqueue_decoded(&p, frame, vq, vq_pts, &vq_len, p.pos);
            }
        } else if (pkt->stream_index == p.aindex && has_audio) {
            if (avcodec_send_packet(p.actx, pkt) >= 0) {
                while (avcodec_receive_frame(p.actx, frame) >= 0) {
                    audio_emit(&p, frame, &abuf, &abuf_cap);
                }
            }
        }
        av_packet_unref(pkt);
    }

    if (eof && vq_len == 0) {
        snprintf(err, errcap, "stream: no video frames");
        goto out;
    }
    p.pos = start_pos > 0 ? start_pos : 0.0;
    if (start_pos <= 0.5 && vq_len > 0)
        p.pos = vq_pts[0];
    p.vbase = p.pos;
    p.audio_base = p.pos;
    applog("player: prefill vq=%d pos=%.2f", vq_len, p.pos);

    stream_set_nonblock(p.stream, 1);

    p.last_us = av_gettime();
    p.last_video_us = av_gettime();
    p.audio_base = p.pos;
    p.osd_until = av_gettime() + 3000000ULL;

    appletSetAutoSleepDisabled(true);
    Result boost = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    applog("player: cpu boost 0x%x", boost);

    int was_bg = 0;
    u64 last_save = 0;
    u64 last_watchdog = 0;

    for (;;) {
        if (!appletMainLoop()) {
            if (appletGetFocusState() == AppletFocusState_Background) {
                if (!p.paused) {
                    p.paused = 1;
                    audio_reset(&p);
                }
                stream_suspend(p.stream);
                was_bg = 1;
                p.last_us = av_gettime();
                present_status(&p, "Пауза (сон)");
                svcSleepThread(100000000ULL);
                continue;
            }
            break;
        }

        AppletFocusState fs = appletGetFocusState();
        if (fs != AppletFocusState_InFocus) {
            if (!p.paused) {
                p.paused = 1;
                audio_reset(&p);
            }
            if (resume_key)
                resume_save(resume_key, p.pos);
            stream_suspend(p.stream);
            was_bg = 1;
            p.last_us = av_gettime();
            if (av_gettime() - p.last_present > 300000)
                present_status(&p, "Пауза (сон)");
            svcSleepThread(50000000ULL);
            continue;
        } else if (was_bg) {
            was_bg = 0;
            stream_resume(p.stream);
            p.paused = 0;
            p.last_us = av_gettime();
            audio_reset(&p);
            snprintf(p.msg, sizeof(p.msg), "Продолжаем");
            p.osd_until = av_gettime() + 2000000ULL;
            present(&p);
        }

        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);

        if (down & (HidNpadButton_Plus | HidNpadButton_B))
            break;

        if (down & HidNpadButton_A) {
            p.paused = !p.paused;
            p.last_us = av_gettime();
            p.osd_until = av_gettime() + 3000000ULL;
            if (resume_key)
                resume_save(resume_key, p.pos);
            if (p.paused) {
                audio_reset(&p);
                snprintf(p.msg, sizeof(p.msg), "Пауза");
            } else {
                snprintf(p.msg, sizeof(p.msg), "Продолжаем");
            }
            present(&p);
        }

        if (resume_key && down & (HidNpadButton_Left | HidNpadButton_Right |
                                  HidNpadButton_L | HidNpadButton_R))
            resume_save(resume_key, p.pos);

        if (down & HidNpadButton_Left)
            seek_to(&p, p.pos - 10.0);
        if (down & HidNpadButton_Right)
            seek_to(&p, p.pos + 10.0);
        if (down & HidNpadButton_L)
            seek_to(&p, p.pos - 60.0);
        if (down & HidNpadButton_R)
            seek_to(&p, p.pos + 60.0);
        if (down & (HidNpadButton_Left | HidNpadButton_Right |
                    HidNpadButton_L | HidNpadButton_R)) {
            for (int i = 0; i < vq_len; i++)
                av_frame_free(&vq[i]);
            vq_len = 0;
            if (held)
                av_packet_unref(held);
            held_used = 0;
            eof = 0;
            video_flushed = 0;
        }

        if (down & (HidNpadButton_Up | HidNpadButton_Down)) {
            p.volume += (down & HidNpadButton_Up) ? 0.1f : -0.1f;
            if (p.volume < 0) p.volume = 0;
            if (p.volume > 1) p.volume = 1;
            audoutSetAudioOutVolume(p.volume);
            snprintf(p.msg, sizeof(p.msg), "Громкость %d%%", (int)(p.volume * 100));
            p.osd_until = av_gettime() + 2000000ULL;
            present(&p);
        }

        audio_release();
        u64 now = av_gettime();
        if (resume_key && p.pos > 3.0 && now - last_save > 10000000) {
            resume_save(resume_key, p.pos);
            last_save = now;
        }
        if (p.msg[0] && now > p.osd_until)
            p.msg[0] = '\0';

        int starving = (!eof && vq_len == 0 &&
                        now - p.last_video_us > 500000);
        if (!p.paused) {
            if (starving) {
                p.last_us = now;
                if (!p.starving) {
                    p.starving = 1;
                    audio_reset(&p);
                }
                if (now - p.last_present > 33000)
                    present_loading(&p, now / 1000, "Буферизация...");
            } else {
                if (p.starving) {
                    p.starving = 0;
                    p.last_us = now;
                }
                if (p.buffering) {
                    p.buffering = 0;
                    p.last_us = now;
                }

                double wclock = p.pos + (double)(now - p.last_us) / 1000000.0;
                double clock = wclock;
                if (has_audio && s_aud_ready) {
                    u64 played = 0;
                    if (R_SUCCEEDED(audoutGetAudioOutPlayedSampleCount(&played))) {
                        double ac = p.audio_base + (double)played / (double)s_rate;
                        if (ac > p.last_ac + 0.0001) {
                            p.last_ac = ac;
                            p.last_ac_us = now;
                        }
                        /* Use the audio clock only while it is actually
                         * advancing; a starved audio queue must not freeze
                         * the picture. */
                        if (now - p.last_ac_us < 300000 &&
                            fabs(ac - wclock) < 1.0 && ac > p.pos - 0.30)
                            clock = ac;
                    }
                }
                p.pos = clock;
                p.last_us = now;
            }
        } else {
            p.last_us = now;
        }

        if (p.paused) {
            if (now < p.osd_until && now - p.last_present > 150000)
                present(&p);
            svcSleepThread(30000000ULL);
            continue;
        }

        while (vq_len > 0) {
            double pts = vq_pts[0];
            if (pts > p.pos + 0.02)
                break;
            if (pts >= p.pos - 0.35)
                render_frame(&p, vq[0]);
            av_frame_free(&vq[0]);
            for (int i = 1; i < vq_len; i++) {
                vq[i - 1] = vq[i];
                vq_pts[i - 1] = vq_pts[i];
            }
            vq_len--;
            vq[vq_len] = NULL;
        }

        /* Feed audio / demux. */
        if (!eof) {
            for (int pk = 0; pk < 2 && !eof; pk++) {
                /* A video packet is held back while the frame queue is
                 * full.  Stop demuxing entirely until it drains: the audio
                 * queue has ~0.5s of slack, so a short pause is harmless. */
                if (held_used) {
                    if (vq_len >= VQ_LOW)
                        break;
                    decode_video(&p, held, vq, vq_pts, &vq_len);
                    av_packet_unref(held);
                    held_used = 0;
                }

                int need_audio = has_audio &&
                                 s_pending < (u32)s_rate * 3 / 2 &&
                                 audio_free_slot() >= 0;
                int need_video = vq_len < VQ_LOW;

                if (!need_audio && !need_video)
                    break;

                int r = av_read_frame(p.fmt, pkt);
                if (r == AVERROR(EAGAIN))
                    break;
                if (r < 0) {
                    if (r == AVERROR_EOF || stream_eof(p.stream)) {
                        applog("player: read_frame eof r=%d", r);
                        eof = 1;
                    } else {
                        applog("player: read_frame transient r=%d", r);
                    }
                    break;
                }

                if (pkt->stream_index == p.vindex) {
                    if (!held_used && vq_len >= VQ_LOW && held) {
                        /* Do not let the decoder run seconds ahead: keep one
                         * packet until the frame queue drains a bit. */
                        av_packet_move_ref(held, pkt);
                        held_used = 1;
                    } else {
                        decode_video(&p, pkt, vq, vq_pts, &vq_len);
                    }
                } else if (pkt->stream_index == p.aindex && has_audio) {
                    if (avcodec_send_packet(p.actx, pkt) >= 0) {
                        while (avcodec_receive_frame(p.actx, frame) >= 0)
                            audio_emit(&p, frame, &abuf, &abuf_cap);
                    }
                }
                av_packet_unref(pkt);
            }
        } else {
            if (!video_flushed) {
                avcodec_send_packet(p.vctx, NULL);
                video_flushed = 1;
            }
            if (has_audio && !audio_flushed) {
                avcodec_send_packet(p.actx, NULL);
                audio_flushed = 1;
            }
            while (avcodec_receive_frame(p.vctx, frame) >= 0)
                enqueue_decoded(&p, frame, vq, vq_pts, &vq_len, p.pos);
            if (has_audio && audio_flushed && audio_free_slot() >= 0) {
                while (avcodec_receive_frame(p.actx, frame) >= 0) {
                    audio_emit(&p, frame, &abuf, &abuf_cap);
                }
            }
            if (has_audio)
                audio_flush_tail();
        }

        /* Watchdog: if the picture has been stuck for several seconds,
         * re-seek to the current position to rebuild the pipeline. */
        if (!p.paused && !eof && now - p.last_video_us > 6000000 &&
            now - last_watchdog > 5000000 &&
            (vq_len == 0 || vq_pts[0] > p.pos + 2.0)) {
            applog("player: watchdog stall pos=%.1f vq=%d",
                   p.pos, vq_len);
            for (int i = 0; i < vq_len; i++)
                av_frame_free(&vq[i]);
            vq_len = 0;
            if (held)
                av_packet_unref(held);
            held_used = 0;
            double t = p.pos;
            seek_to(&p, t);
            p.last_video_us = av_gettime();
            p.last_us = p.last_video_us;
            last_watchdog = p.last_video_us;
        }

        if (eof && vq_len == 0 && (!has_audio || s_pending == 0))
            break;

        if (vq_len > 0 && vq_pts[0] > p.pos) {
            double d = vq_pts[0] - p.pos;
            if (d > 0.02)
                svcSleepThread(15000000ULL);
            else
                svcSleepThread((u64)(d * 1000000.0) + 1000);
        } else {
            svcSleepThread(2000000ULL);
        }

        if ((now < p.osd_until) && now - p.last_present > 200000)
            present(&p);
    }

    ret = 0;

out:
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    appletSetAutoSleepDisabled(false);

    if (resume_key && p.pos > 3.0) {
        if (p.duration > 1.0 && p.pos > p.duration - 15.0)
            resume_clear(resume_key);
        else
            resume_save(resume_key, p.pos);
    }

    if (end_pos)
        *end_pos = p.pos;

    AVIOContext *avio = p.avio;
    p.avio = NULL;
    if (p.fmt) {
        p.fmt->pb = NULL; /* custom IO: we own the AVIOContext */
        avformat_close_input(&p.fmt);
    }
    if (avio) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
    }
    if (p.stream)
        stream_close(p.stream);

    if (p.vctx) {
        av_buffer_unref(&p.vctx->hw_device_ctx);
        avcodec_free_context(&p.vctx);
    }
    if (p.actx)
        avcodec_free_context(&p.actx);
    if (p.sws)
        sws_freeContext(p.sws);
    if (p.swr)
        swr_free(&p.swr);

    av_packet_free(&pkt);
    av_packet_free(&held);
    av_frame_free(&frame);
    for (int i = 0; i < VQ_MAX; i++)
        if (vq[i])
            av_frame_free(&vq[i]);

    free(abuf);
    free(p.canvas);

    audio_close();
    for (int i = 0; i < ABUF_COUNT; i++) {
        free(s_data[i]);
        s_data[i] = NULL;
        s_free[i] = 0;
        s_frames[i] = 0;
    }
    s_fill = -1;
    s_used = 0;
    s_written = 0;
    s_pending = 0;

    if (p.fb.has_init)
        framebufferClose(&p.fb);

    return ret;
}
