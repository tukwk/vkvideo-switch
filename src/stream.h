#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct stream stream_t;

/* Progressive HTTP(S) stream with a bounded ring buffer and Range-based
 * seeking. The downloader runs in a background thread. */
stream_t *stream_open(const char *url, char *err, size_t errcap);
stream_t *stream_start(const char *url, char *err, size_t errcap);
int stream_ready(stream_t *s);      /* first bytes arrived, eof or error */
int stream_failed(stream_t *s);
int stream_eof(stream_t *s);
int stream_buffered(stream_t *s);
void stream_close(stream_t *s);

int     stream_read(void *opaque, uint8_t *buf, int size);
int64_t stream_seek(void *opaque, int64_t offset, int whence);

int64_t stream_size(stream_t *s);
const char *stream_error(stream_t *s);
void stream_suspend(stream_t *s);
void stream_resume(stream_t *s);
void stream_touch(stream_t *s);
void stream_set_nonblock(stream_t *s, int on);
