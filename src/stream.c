#include "stream.h"

#include <switch.h>

#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <pthread.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/time.h>

#include "log.h"
#include "net.h"

#define STREAM_CAP (48u * 1024u * 1024u)

typedef struct {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    int tls;
    stream_t *owner;
    u8 prefix[4096];
    size_t prefix_len;
    size_t prefix_pos;
} conn_t;

struct stream {
    pthread_mutex_t mu;
    pthread_cond_t cond;

    u8 *buf;
    size_t cap;
    uint64_t start;
    size_t len;
    uint64_t pos;
    uint64_t total;

    int eof;
    int error;
    int abort;
    int restart;
    uint64_t restart_off;
    int fd;
    int64_t last_data_us;
    int64_t time_shift_us;
    int64_t suspend_start_us;
    int suspended;
    int nonblock;

    pthread_t thread;
    int thread_started;

    char url[4096];
    char err[160];
};

static int parse_url(const char *url, char *scheme, size_t scap, char *host,
                     size_t hcap, char *port, size_t pcap, char *path, size_t pathcap)
{
    const char *p = url;
    const char *hs, *he;

    memset(scheme, 0, scap);
    memset(host, 0, hcap);
    memset(port, 0, pcap);
    memset(path, 0, pathcap);

    if (strncmp(p, "https://", 8) == 0) {
        snprintf(scheme, scap, "https");
        snprintf(port, pcap, "443");
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        snprintf(scheme, scap, "http");
        snprintf(port, pcap, "80");
        p += 7;
    } else {
        return -1;
    }

    hs = p;
    he = p;
    while (*he && *he != '/' && *he != ':' && *he != '?')
        he++;
    if (he == hs || (size_t)(he - hs) >= hcap)
        return -1;
    memcpy(host, hs, (size_t)(he - hs));
    host[he - hs] = '\0';

    p = he;
    if (*p == ':') {
        p++;
        const char *ps = p;
        while (*p && *p != '/' && *p != '?')
            p++;
        if ((size_t)(p - ps) >= pcap)
            return -1;
        memcpy(port, ps, (size_t)(p - ps));
        port[p - ps] = '\0';
    }

    if (*p == '?')
        snprintf(path, pathcap, "/%s", p);
    else if (*p == '/')
        snprintf(path, pathcap, "%s", p);
    else
        snprintf(path, pathcap, "/");

    return 0;
}

static int64_t snow(stream_t *s)
{
    return av_gettime() - s->time_shift_us;
}

static int cond_wait_ms(pthread_cond_t *cond, pthread_mutex_t *mu, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(cond, mu, &ts);
}

static void conn_close(conn_t *c)
{
    if (c->tls)
        mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_net_free(&c->net);
    mbedtls_ssl_free(&c->ssl);
}

static int conn_raw_read(conn_t *c, unsigned char *buf, size_t len)
{
    int ret;
    int64_t start = snow(c->owner);
    do {
        ret = c->tls ? mbedtls_ssl_read(&c->ssl, buf, len)
                     : mbedtls_net_recv(&c->net, buf, len);
        if ((ret == MBEDTLS_ERR_SSL_WANT_READ ||
             ret == MBEDTLS_ERR_SSL_WANT_WRITE) &&
            snow(c->owner) - start > 25000000)
            return MBEDTLS_ERR_NET_RECV_FAILED;
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
             ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    return ret;
}

static int conn_read(conn_t *c, unsigned char *buf, size_t len)
{
    if (c->prefix_pos < c->prefix_len) {
        size_t avail = c->prefix_len - c->prefix_pos;
        size_t n = avail < len ? avail : len;
        memcpy(buf, c->prefix + c->prefix_pos, n);
        c->prefix_pos += n;
        if (c->prefix_pos == c->prefix_len)
            c->prefix_pos = c->prefix_len = 0;
        return (int)n;
    }
    return conn_raw_read(c, buf, len);
}

static int conn_write(conn_t *c, const unsigned char *buf, size_t len)
{
    int ret;
    int64_t start = snow(c->owner);
    do {
        ret = c->tls ? mbedtls_ssl_write(&c->ssl, buf, len)
                     : mbedtls_net_send(&c->net, buf, len);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
            ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (snow(c->owner) - start > 60000000)
                return MBEDTLS_ERR_NET_SEND_FAILED;
            svcSleepThread(2000000ULL);
        }
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
             ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    return ret;
}

static const char *find_header(const char *headers, const char *name)
{
    static char lower[16384];
    size_t n = strlen(headers);
    if (n >= sizeof(lower))
        n = sizeof(lower) - 1;
    for (size_t i = 0; i < n; i++) {
        char ch = headers[i];
        lower[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    }
    lower[n] = '\0';

    char needle[64];
    snprintf(needle, sizeof(needle), "\n%s:", name);
    const char *p = strstr(lower, needle);
    if (!p)
        return NULL;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t')
        p++;
    return headers + (p - lower);
}

/* Follows redirects and opens a connection positioned at `off`.
 * Returns 200 or 206 and leaves the connection open. */
static int tcp_connect(const char *host, const char *port, int *out_fd,
                       char *err, size_t errcap)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; /* Switch IPv6 routes are often unusable */
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0 || !res) {
        snprintf(err, errcap, "DNS %.60s failed (%d)", host, rc);
        return -1;
    }

    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        char ip[64] = "?";
        getnameinfo(ai->ai_addr, ai->ai_addrlen, ip, sizeof(ip),
                    NULL, 0, NI_NUMERICHOST);

        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            applog("connect: socket(%s) failed errno=%d", ip, errno);
            continue;
        }

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            applog("connect: %.60s:%.8s -> %.40s ok", host, port, ip);
            *out_fd = fd;
            freeaddrinfo(res);
            return 0;
        }

        applog("connect: %.60s:%.8s -> %.40s failed errno=%d", host, port, ip, errno);
        close(fd);
    }

    freeaddrinfo(res);
    snprintf(err, errcap, "connect %.60s:%.8s failed", host, port);
    return -1;
}

static int conn_open(stream_t *s, conn_t *c, uint64_t off,
                     uint64_t *total_out, char *err, size_t errcap)
{
    char scheme[8], host[256], port[8], path[4096];
    int ret;

    for (int hop = 0; hop < 5; hop++) {
        if (parse_url(s->url, scheme, sizeof(scheme), host, sizeof(host),
                      port, sizeof(port), path, sizeof(path)) != 0) {
            snprintf(err, errcap, "bad stream URL");
            return -1;
        }

        memset(c, 0, sizeof(*c));
        c->owner = s;
        mbedtls_net_init(&c->net);
        mbedtls_ssl_init(&c->ssl);

        int fd = -1;
        if (tcp_connect(host, port, &fd, err, errcap) != 0) {
            conn_close(c);
            return -1;
        }
        c->net.fd = fd;
        struct timeval tv = { .tv_sec = 15, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        s->fd = fd;

        if (strcmp(scheme, "https") == 0) {
            mbedtls_ssl_config *conf = net_ssl_config();
            if (!conf) {
                snprintf(err, errcap, "TLS not initialized");
                conn_close(c);
                return -1;
            }
            ret = mbedtls_ssl_setup(&c->ssl, conf);
            if (ret != 0) {
                snprintf(err, errcap, "ssl_setup failed (-0x%04x)", (unsigned)-ret);
                conn_close(c);
                return -1;
            }
            mbedtls_ssl_set_hostname(&c->ssl, host);
            mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send,
                                mbedtls_net_recv, NULL);

            while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
                if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
                    ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                    if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                        unsigned vr = mbedtls_ssl_get_verify_result(&c->ssl);
                        snprintf(err, errcap, "TLS certificate verification "
                                 "failed (flags=0x%x, check console clock)", vr);
                        applog("tls: stream cert verify failed host=%s flags=0x%x",
                               host, vr);
                    } else {
                        snprintf(err, errcap, "TLS handshake failed (-0x%04x)",
                                 (unsigned)-ret);
                    }
                    conn_close(c);
                    return -1;
                }
            }
            c->tls = 1;
        }

        char request[8192];
        if (off)
            snprintf(request, sizeof(request),
                     "GET %s HTTP/1.0\r\nHost: %s\r\n"
                     "User-Agent: vkvideo-switch/0.4\r\nAccept: */*\r\n"
                     "Range: bytes=%llu-\r\nConnection: close\r\n\r\n",
                     path, host, (unsigned long long)off);
        else
            snprintf(request, sizeof(request),
                     "GET %s HTTP/1.0\r\nHost: %s\r\n"
                     "User-Agent: vkvideo-switch/0.4\r\nAccept: */*\r\n"
                     "Connection: close\r\n\r\n",
                     path, host);

        size_t sent = 0, reqlen = strlen(request);
        while (sent < reqlen) {
            int n = conn_write(c, (const unsigned char *)request + sent, reqlen - sent);
            if (n <= 0) {
                snprintf(err, errcap, "request write failed");
                conn_close(c);
                return -1;
            }
            sent += (size_t)n;
        }

        unsigned char *hdr = malloc(16384);
        if (!hdr) {
            snprintf(err, errcap, "out of memory");
            conn_close(c);
            return -1;
        }

        size_t hlen = 0;
        while (hlen < 16384 - 1) {
            int n = conn_raw_read(c, hdr + hlen, 16384 - 1 - hlen);
            if (n <= 0) {
                snprintf(err, errcap, "no HTTP headers");
                free(hdr);
                conn_close(c);
                return -1;
            }
            hlen += (size_t)n;
            hdr[hlen] = '\0';
            if (strstr((char *)hdr, "\r\n\r\n"))
                break;
        }
        hdr[hlen] = '\0';

        char *body = strstr((char *)hdr, "\r\n\r\n");
        if (!body) {
            snprintf(err, errcap, "malformed HTTP response");
            free(hdr);
            conn_close(c);
            return -1;
        }
        body += 4;
        size_t already = hlen - (size_t)(body - (char *)hdr);
        if (already > sizeof(c->prefix)) {
            snprintf(err, errcap, "HTTP header too large");
            free(hdr);
            conn_close(c);
            return -1;
        }
        memcpy(c->prefix, body, already);
        c->prefix_len = already;
        c->prefix_pos = 0;

        int status = 0;
        sscanf((char *)hdr, "HTTP/%*d.%*d %d", &status);

        if (status >= 300 && status < 400) {
            const char *loc = find_header((char *)hdr, "location");
            if (loc) {
                char value[2048];
                size_t i = 0;
                while (*loc && *loc != '\r' && *loc != '\n' && i + 1 < sizeof(value))
                    value[i++] = *loc++;
                value[i] = '\0';
                if (strncmp(value, "http://", 7) == 0 ||
                    strncmp(value, "https://", 8) == 0) {
                    snprintf(s->url, sizeof(s->url), "%s", value);
                    free(hdr);
                    conn_close(c);
                    continue;
                }
            }
        }

        if (status != 200 && status != 206) {
            snprintf(err, errcap, "HTTP status %d", status);
            free(hdr);
            conn_close(c);
            return -1;
        }

        if (off && status == 200) {
            snprintf(err, errcap, "server ignored Range");
            free(hdr);
            conn_close(c);
            return -1;
        }

        uint64_t total = 0;
        const char *cr = find_header((char *)hdr, "content-range");
        if (cr) {
            const char *slash = strchr(cr, '/');
            if (slash)
                total = strtoull(slash + 1, NULL, 10);
        }
        if (!total) {
            const char *cl = find_header((char *)hdr, "content-length");
            if (cl)
                total = strtoull(cl, NULL, 10) + off;
        }
        if (total_out)
            *total_out = total;

        free(hdr);
        return status;
    }

    snprintf(err, errcap, "too many redirects");
    return -1;
}

/* True circular buffer: data always starts at physical offset
 * (start % cap).  Reclaiming consumed bytes is O(1) - we only move the
 * logical start forward, no memmove of the whole buffer. */
static int stream_append(stream_t *s, const u8 *p, size_t n)
{
    pthread_mutex_lock(&s->mu);

    for (;;) {
        if (s->abort || s->restart) {
            pthread_mutex_unlock(&s->mu);
            return 0;
        }
        if (s->len + n <= s->cap)
            break;

        /* Drop already-consumed bytes by moving the logical window. */
        if (s->pos > s->start) {
            uint64_t drop = s->pos - s->start;
            if (drop > s->len)
                drop = s->len;
            if (drop > 0) {
                s->start += drop;
                s->len -= (size_t)drop;
                pthread_cond_broadcast(&s->cond);
            }
        }
        if (s->len + n <= s->cap)
            break;

        pthread_cond_wait(&s->cond, &s->mu);
    }

    size_t off = (size_t)((s->start + s->len) % s->cap);
    size_t first = s->cap - off;
    if (first > n)
        first = n;
    memcpy(s->buf + off, p, first);
    if (n > first)
        memcpy(s->buf, p + first, n - first);

    s->len += n;
    s->last_data_us = snow(s);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mu);
    return 1;
}

static void *stream_thread(void *arg)
{
    stream_t *s = arg;
    uint64_t off = 0;
    int attempt = 0;

    for (;;) {
        pthread_mutex_lock(&s->mu);
        if (s->abort) {
            pthread_mutex_unlock(&s->mu);
            break;
        }
        if (s->restart) {
            off = s->restart_off;
            s->start = off;
            s->len = 0;
            s->pos = off;
            s->eof = 0;
            s->restart = 0;
        }
        if (s->eof) {
            pthread_cond_wait(&s->cond, &s->mu);
            pthread_mutex_unlock(&s->mu);
            continue;
        }
        pthread_mutex_unlock(&s->mu);

        conn_t c;
        uint64_t total = 0;
        char err[128] = {0};
        int status = conn_open(s, &c, off, &total, err, sizeof(err));

        if (status < 0) {
            attempt++;
            applog("stream: open failed at %llu: %s (try %d)",
                   (unsigned long long)off, err, attempt);
            if (attempt >= 3) {
                pthread_mutex_lock(&s->mu);
                snprintf(s->err, sizeof(s->err), "%s", err);
                s->error = 1;
                s->eof = 1;
                pthread_cond_broadcast(&s->cond);
                pthread_mutex_unlock(&s->mu);
                break;
            }
            svcSleepThread(400000000ULL);
            continue;
        }
        attempt = 0;

        pthread_mutex_lock(&s->mu);
        s->total = total;
        s->fd = c.net.fd;
        pthread_mutex_unlock(&s->mu);

        applog("stream: connected off=%llu status=%d total=%llu",
               (unsigned long long)off, status, (unsigned long long)total);

        uint64_t got = 0;
        u8 tmp[65536];
        for (;;) {
            int n = conn_read(&c, tmp, sizeof(tmp));
            if (n <= 0)
                break;
            if (!stream_append(s, tmp, (size_t)n)) {
                got += (uint64_t)n; /* data read but discarded after restart */
                break;
            }
            got += (uint64_t)n;
            if (total && off + got >= total)
                break;
        }

        conn_close(&c);

        pthread_mutex_lock(&s->mu);
        s->fd = -1;
        int restart = s->restart;
        int abort = s->abort;
        uint64_t new_off = off + got;

        if (!restart && !abort) {
            if (s->total && new_off < s->total) {
                /* CDN closed before the file was fully transferred: reconnect
                 * at the current offset instead of pretending we hit EOF. */
                applog("stream: early close at %llu/%llu, reconnecting",
                       (unsigned long long)new_off, (unsigned long long)s->total);
                s->eof = 0;
                off = new_off;
                pthread_cond_broadcast(&s->cond);
                pthread_mutex_unlock(&s->mu);
                svcSleepThread(150000000ULL);
                continue;
            }
            s->eof = 1;
            if (!s->total)
                s->total = new_off;
            applog("stream: download complete off=%llu total=%llu",
                   (unsigned long long)new_off, (unsigned long long)s->total);
        }
        off = new_off;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->mu);

        if (abort)
            break;
    }

    return NULL;
}

static void request_restart(stream_t *s)
{
    if (s->fd >= 0)
        shutdown(s->fd, SHUT_RDWR);
    pthread_cond_broadcast(&s->cond);
}

stream_t *stream_start(const char *url, char *err, size_t errcap)
{
    stream_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cond, NULL);

    s->cap = STREAM_CAP;
    s->buf = malloc(s->cap);
    if (!s->buf) {
        s->cap = 16u * 1024u * 1024u;
        s->buf = malloc(s->cap);
    }
    if (!s->buf) {
        snprintf(err, errcap, "out of memory (stream buffer)");
        free(s);
        return NULL;
    }

    snprintf(s->url, sizeof(s->url), "%s", url);
    s->fd = -1;
    s->last_data_us = snow(s);

    if (pthread_create(&s->thread, NULL, stream_thread, s) != 0) {
        snprintf(err, errcap, "cannot start downloader thread");
        free(s->buf);
        free(s);
        return NULL;
    }
    s->thread_started = 1;
    applog("stream: started");
    return s;
}

int stream_ready(stream_t *s)
{
    if (!s) return 0;
    pthread_mutex_lock(&s->mu);
    int r = (s->len > 0 || s->eof || s->error);
    pthread_mutex_unlock(&s->mu);
    return r;
}

int stream_failed(stream_t *s)
{
    if (!s) return 1;
    pthread_mutex_lock(&s->mu);
    int r = s->error;
    pthread_mutex_unlock(&s->mu);
    return r;
}

int stream_eof(stream_t *s)
{
    if (!s) return 1;
    pthread_mutex_lock(&s->mu);
    int r = s->eof;
    pthread_mutex_unlock(&s->mu);
    return r;
}

int stream_buffered(stream_t *s)
{
    if (!s) return 0;
    pthread_mutex_lock(&s->mu);
    int r = (int)s->len;
    pthread_mutex_unlock(&s->mu);
    return r;
}

stream_t *stream_open(const char *url, char *err, size_t errcap)
{
    stream_t *s = stream_start(url, err, errcap);
    if (!s)
        return NULL;

    pthread_mutex_lock(&s->mu);
    int64_t deadline = av_gettime() + 20000000;
    while (s->len == 0 && !s->eof && !s->error) {
        if (av_gettime() > deadline) {
            snprintf(s->err, sizeof(s->err), "stream open timeout");
            s->error = 1;
            break;
        }
        cond_wait_ms(&s->cond, &s->mu, 200);
    }
    pthread_mutex_unlock(&s->mu);

    if (s->error) {
        snprintf(err, errcap, "%.150s", s->err);
        stream_close(s);
        return NULL;
    }

    applog("stream: ready start=%llu len=%zu total=%llu",
           (unsigned long long)s->start, s->len, (unsigned long long)s->total);
    return s;
}

void stream_close(stream_t *s)
{
    if (!s)
        return;

    pthread_mutex_lock(&s->mu);
    s->abort = 1;
    request_restart(s);
    pthread_mutex_unlock(&s->mu);

    if (s->thread_started)
        pthread_join(s->thread, NULL);

    free(s->buf);
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cond);
    free(s);
}

int stream_read(void *opaque, uint8_t *buf, int size)
{
    stream_t *s = opaque;
    if (size <= 0)
        return 0;

    pthread_mutex_lock(&s->mu);

    for (;;) {
        if (s->abort) {
            pthread_mutex_unlock(&s->mu);
            return AVERROR_EXIT;
        }

        if (s->pos < s->start) {
            s->restart_off = s->pos;
            s->restart = 1;
            request_restart(s);
            pthread_cond_wait(&s->cond, &s->mu);
            continue;
        }

        if (s->pos < s->start + s->len) {
            size_t n = (size_t)(s->start + s->len - s->pos);
            if (n > (size_t)size)
                n = (size_t)size;
            size_t off = (size_t)(s->pos % s->cap);
            size_t first = s->cap - off;
            if (first > n)
                first = n;
            memcpy(buf, s->buf + off, first);
            if (n > first)
                memcpy(buf + first, s->buf, n - first);
            s->pos += n;
            pthread_cond_broadcast(&s->cond);
            pthread_mutex_unlock(&s->mu);
            return (int)n;
        }

        if (s->error) {
            pthread_mutex_unlock(&s->mu);
            return AVERROR(EIO);
        }

        if (s->eof) {
            pthread_mutex_unlock(&s->mu);
            return 0;
        }

        if (s->nonblock) {
            /* Give the downloader a short window, then let the player render
             * queued frames instead of blocking the whole loop on the network. */
            cond_wait_ms(&s->cond, &s->mu, 15);
            if (s->pos < s->start + s->len)
                continue;
            if (s->eof) {
                pthread_mutex_unlock(&s->mu);
                return 0;
            }
            pthread_mutex_unlock(&s->mu);
            return AVERROR(EAGAIN);
        }

        if (!s->suspended && snow(s) - s->last_data_us > 20000000) {
            s->error = 1;
            pthread_mutex_unlock(&s->mu);
            return AVERROR(ETIMEDOUT);
        }

        cond_wait_ms(&s->cond, &s->mu, 200);
    }
}

int64_t stream_seek(void *opaque, int64_t offset, int whence)
{
    stream_t *s = opaque;
    int64_t target;

    pthread_mutex_lock(&s->mu);

    if (whence == AVSEEK_SIZE) {
        int64_t r = s->total ? (int64_t)s->total : AVERROR_UNKNOWN;
        pthread_mutex_unlock(&s->mu);
        return r;
    }

    if (whence == SEEK_SET)
        target = offset;
    else if (whence == SEEK_CUR)
        target = (int64_t)s->pos + offset;
    else if (whence == SEEK_END) {
        if (!s->total) {
            pthread_mutex_unlock(&s->mu);
            return AVERROR_UNKNOWN;
        }
        target = (int64_t)s->total + offset;
    } else {
        pthread_mutex_unlock(&s->mu);
        return AVERROR(EINVAL);
    }

    if (target < 0)
        target = 0;

    if ((uint64_t)target >= s->start &&
        (uint64_t)target <= s->start + s->len) {
        s->pos = (uint64_t)target;
        pthread_mutex_unlock(&s->mu);
        return target;
    }

    if (s->total && (uint64_t)target >= s->total) {
        s->pos = s->total;
        pthread_mutex_unlock(&s->mu);
        return (int64_t)s->total;
    }

    /* The demuxer often seeks a little past the downloaded edge (to the
     * next sample or index entry).  Wait for the downloader to reach it
     * instead of tearing the connection down: reconnect storms here made
     * seeking unusable. */
    if ((uint64_t)target > s->start + s->len &&
        s->len < s->cap && !s->eof && !s->error) {
        if (s->pos > s->start) {
            uint64_t drop = s->pos - s->start;
            if (drop > s->len)
                drop = s->len;
            s->start += drop;
            s->len -= (size_t)drop;
            pthread_cond_broadcast(&s->cond);
        }
        int64_t wdeadline = av_gettime() + 4000000;
        while (!s->abort && !s->error && !s->eof &&
               !((uint64_t)target >= s->start &&
                 (uint64_t)target <= s->start + s->len)) {
            if (av_gettime() > wdeadline)
                break;
            cond_wait_ms(&s->cond, &s->mu, 20);
        }
        if (!s->abort && !s->error &&
            (uint64_t)target >= s->start &&
            (uint64_t)target <= s->start + s->len) {
            s->pos = (uint64_t)target;
            pthread_mutex_unlock(&s->mu);
            return target;
        }
    }

    applog("stream: seek %lld outside window [%llu,%llu), restart",
           (long long)target, (unsigned long long)s->start,
           (unsigned long long)(s->start + s->len));
    s->restart_off = (uint64_t)target;
    s->restart = 1;
    s->eof = 0;
    request_restart(s);

    int64_t deadline = av_gettime() + 6000000;
    while (!s->abort && !s->error &&
           !(s->start == (uint64_t)target && s->len > 0)) {
        if (av_gettime() > deadline) {
            s->error = 1;
            break;
        }
        cond_wait_ms(&s->cond, &s->mu, 200);
    }

    if (s->abort) {
        pthread_mutex_unlock(&s->mu);
        return AVERROR_EXIT;
    }
    if (s->error) {
        pthread_mutex_unlock(&s->mu);
        return AVERROR(EIO);
    }

    s->pos = (uint64_t)target;
    pthread_mutex_unlock(&s->mu);
    return target;
}

void stream_suspend(stream_t *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    if (!s->suspended) {
        s->suspended = 1;
        s->suspend_start_us = av_gettime();
        applog("stream: suspend");
    }
    pthread_mutex_unlock(&s->mu);
}

void stream_resume(stream_t *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    if (s->suspended) {
        int64_t slept = av_gettime() - s->suspend_start_us;
        s->time_shift_us += slept;
        s->suspended = 0;
        s->last_data_us = snow(s);
        applog("stream: resume shift=%lld ms", (long long)(s->time_shift_us / 1000));
        /* After sleep the TCP connection is usually dead.  Nudge it so the
         * downloader reconnects at its current offset instead of waiting for
         * a timeout. */
        if (slept > 2000000 && s->fd >= 0) {
            applog("stream: dropping stale connection after sleep");
            shutdown(s->fd, SHUT_RDWR);
        }
        pthread_cond_broadcast(&s->cond);
    }
    pthread_mutex_unlock(&s->mu);
}

void stream_set_nonblock(stream_t *s, int on)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    s->nonblock = on;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mu);
}

void stream_touch(stream_t *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    s->last_data_us = snow(s);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mu);
}

int64_t stream_size(stream_t *s)
{
    return s ? (int64_t)s->total : 0;
}

const char *stream_error(stream_t *s)
{
    return s ? s->err : "";
}
