#include "http.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

typedef struct {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    int tls;
} conn_t;

typedef struct {
    char scheme[8];
    char host[256];
    char port[8];
    char path[4096];
} url_t;

typedef struct {
    FILE *file;
    unsigned char **mem;
    size_t len;
    size_t cap;
    size_t max;
    int oom;
} sink_t;

static int sink_write(sink_t *s, const void *p, size_t n)
{
    if (s->file)
        return fwrite(p, 1, n, s->file) == n ? 0 : -1;

    if (!s->mem)
        return 0;

    if (s->len + n > s->max) {
        s->oom = 1;
        return -1;
    }
    if (s->len + n + 1 > s->cap) {
        size_t cap = s->cap ? s->cap : 65536;
        while (cap < s->len + n + 1)
            cap *= 2;
        if (cap > s->max + 1)
            cap = s->max + 1;
        unsigned char *nb = realloc(*s->mem, cap);
        if (!nb) {
            s->oom = 1;
            return -1;
        }
        *s->mem = nb;
        s->cap = cap;
    }
    memcpy(*s->mem + s->len, p, n);
    s->len += n;
    return 0;
}

static int parse_url(const char *url, url_t *u)
{
    const char *p = url;
    const char *host_start, *host_end;

    memset(u, 0, sizeof(*u));

    if (strncmp(p, "https://", 8) == 0) {
        snprintf(u->scheme, sizeof(u->scheme), "https");
        snprintf(u->port, sizeof(u->port), "443");
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        snprintf(u->scheme, sizeof(u->scheme), "http");
        snprintf(u->port, sizeof(u->port), "80");
        p += 7;
    } else {
        return -1;
    }

    host_start = p;
    host_end = p;
    while (*host_end && *host_end != '/' && *host_end != ':' && *host_end != '?')
        host_end++;

    if (host_end == host_start || (size_t)(host_end - host_start) >= sizeof(u->host))
        return -1;
    memcpy(u->host, host_start, (size_t)(host_end - host_start));

    p = host_end;
    if (*p == ':') {
        p++;
        const char *port_start = p;
        while (*p && *p != '/' && *p != '?')
            p++;
        if ((size_t)(p - port_start) >= sizeof(u->port))
            return -1;
        memcpy(u->port, port_start, (size_t)(p - port_start));
    }

    if (*p == '?') {
        snprintf(u->path, sizeof(u->path), "/%s", p);
    } else if (*p == '/') {
        snprintf(u->path, sizeof(u->path), "%s", p);
    } else {
        snprintf(u->path, sizeof(u->path), "/");
    }

    return u->path[0] ? 0 : -1;
}

static int conn_open(conn_t *c, const url_t *u, char *err, size_t errcap)
{
    int ret;

    memset(c, 0, sizeof(*c));
    mbedtls_net_init(&c->net);
    mbedtls_ssl_init(&c->ssl);

    ret = mbedtls_net_connect(&c->net, u->host, u->port, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        snprintf(err, errcap, "connect %s:%s failed (-0x%04x)", u->host, u->port,
                 (unsigned)-ret);
        mbedtls_net_free(&c->net);
        mbedtls_ssl_free(&c->ssl);
        return -1;
    }

    if (strcmp(u->scheme, "https") == 0) {
        mbedtls_ssl_config *conf = net_ssl_config();
        if (!conf) {
            snprintf(err, errcap, "TLS is not initialized");
            mbedtls_net_free(&c->net);
            return -1;
        }
        ret = mbedtls_ssl_setup(&c->ssl, conf);
        if (ret != 0) {
            snprintf(err, errcap, "ssl_setup failed (-0x%04x)", (unsigned)-ret);
            mbedtls_net_free(&c->net);
            mbedtls_ssl_free(&c->ssl);
            return -1;
        }
        mbedtls_ssl_set_hostname(&c->ssl, u->host);
        mbedtls_ssl_set_bio(&c->ssl, &c->net, mbedtls_net_send,
                            mbedtls_net_recv, NULL);

        while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
                ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
                    unsigned vr = mbedtls_ssl_get_verify_result(&c->ssl);
                    snprintf(err, errcap, "TLS certificate verification "
                             "failed (flags=0x%x, check console clock)", vr);
                } else {
                    snprintf(err, errcap, "TLS handshake failed (-0x%04x)",
                             (unsigned)-ret);
                }
                mbedtls_net_free(&c->net);
                mbedtls_ssl_free(&c->ssl);
                return -1;
            }
        }
        c->tls = 1;
    }

    return 0;
}

static void conn_close(conn_t *c)
{
    if (c->tls)
        mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_net_free(&c->net);
    mbedtls_ssl_free(&c->ssl);
}

static int conn_read(conn_t *c, unsigned char *buf, size_t len)
{
    int ret;
    do {
        ret = c->tls ? mbedtls_ssl_read(&c->ssl, buf, len)
                     : mbedtls_net_recv(&c->net, buf, len);
    } while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
             ret == MBEDTLS_ERR_SSL_WANT_WRITE);
    return ret;
}

static int conn_write(conn_t *c, const unsigned char *buf, size_t len)
{
    int ret;
    do {
        ret = c->tls ? mbedtls_ssl_write(&c->ssl, buf, len)
                     : mbedtls_net_send(&c->net, buf, len);
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

static int download_once(const url_t *u, sink_t *sink,
                         http_progress_cb cb, void *ud,
                         char *new_url, size_t new_url_cap,
                         char *err, size_t errcap)
{
    conn_t c;
    char request[8192];
    unsigned char *hdr = NULL;
    unsigned char *buf = NULL;
    int rc = -1;

    hdr = malloc(16384);
    buf = malloc(65536);
    if (!hdr || !buf) {
        snprintf(err, errcap, "out of memory");
        goto out;
    }

    if (conn_open(&c, u, err, errcap) != 0)
        goto out;

    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "User-Agent: vkvideo-switch/0.3\r\n"
             "Accept: */*\r\n"
             "\r\n",
             u->path, u->host);

    size_t sent = 0, reqlen = strlen(request);
    while (sent < reqlen) {
        int n = conn_write(&c, (const unsigned char *)request + sent, reqlen - sent);
        if (n <= 0) {
            snprintf(err, errcap, "request write failed");
            goto close_conn;
        }
        sent += (size_t)n;
    }

    size_t hlen = 0;
    while (hlen < 16384 - 1) {
        int n = conn_read(&c, hdr + hlen, 16384 - 1 - hlen);
        if (n <= 0) {
            snprintf(err, errcap, "connection closed before headers");
            goto close_conn;
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
        goto close_conn;
    }
    body += 4;
    size_t already = hlen - (size_t)(body - (char *)hdr);

    int status = 0;
    if (sscanf((char *)hdr, "HTTP/%*d.%*d %d", &status) != 1) {
        snprintf(err, errcap, "malformed status line");
        goto close_conn;
    }

    if (status >= 300 && status < 400) {
        const char *loc = find_header((char *)hdr, "location");
        if (loc) {
            char value[2048];
            size_t i = 0;
            while (*loc && *loc != '\r' && *loc != '\n' && i + 1 < sizeof(value))
                value[i++] = *loc++;
            value[i] = '\0';

            if (strncmp(value, "http://", 7) == 0 || strncmp(value, "https://", 8) == 0) {
                snprintf(new_url, new_url_cap, "%s", value);
            } else if (value[0] == '/') {
                snprintf(new_url, new_url_cap, "%s://%s:%s%s", u->scheme, u->host,
                         u->port, value);
            } else {
                snprintf(err, errcap, "unsupported redirect: %.80s", value);
                goto close_conn;
            }
            rc = 1;
            goto close_conn;
        }
    }

    if (status != 200) {
        snprintf(err, errcap, "HTTP status %d", status);
        goto close_conn;
    }

    uint64_t total = 0;
    const char *clen = find_header((char *)hdr, "content-length");
    if (clen)
        total = strtoull(clen, NULL, 10);

    const char *te = find_header((char *)hdr, "transfer-encoding");
    if (te && strncmp(te, "chunked", 7) == 0) {
        snprintf(err, errcap, "server sent chunked encoding (unsupported)");
        goto close_conn;
    }

    uint64_t got = 0;
    if (already) {
        if (sink_write(sink, body, already) != 0) {
            snprintf(err, errcap, "write failed");
            goto close_conn;
        }
        got = already;
    }

    while (!(total && got >= total)) {
        int n = conn_read(&c, buf, 65536);
        if (n == 0)
            break;
        if (n < 0) {
            if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
                n == MBEDTLS_ERR_NET_CONN_RESET)
                break;
            snprintf(err, errcap, "read failed (-0x%04x)", (unsigned)-n);
            goto close_conn;
        }
        if (sink_write(sink, buf, (size_t)n) != 0) {
            snprintf(err, errcap, sink->oom ? "response too large" : "write failed");
            goto close_conn;
        }
        got += (uint64_t)n;
        if (cb)
            cb(ud, got, total);
        if (total && got >= total)
            break; /* CDN may keep the connection open despite Connection: close */
    }

    rc = 0;

close_conn:
    conn_close(&c);
out:
    free(hdr);
    free(buf);
    return rc;
}

static int http_fetch(const char *url, sink_t *sink,
                      http_progress_cb cb, void *ud,
                      char *err, size_t errcap)
{
    char current[4096];
    char next[4096];

    if (err && errcap)
        err[0] = '\0';

    if (net_init() != 0) {
        snprintf(err, errcap, "TLS initialization failed");
        return -1;
    }

    snprintf(current, sizeof(current), "%s", url);

    for (int hop = 0; hop < 6; hop++) {
        url_t u;
        if (parse_url(current, &u) != 0) {
            snprintf(err, errcap, "unsupported URL: %.120s", current);
            return -1;
        }

        next[0] = '\0';
        int r = download_once(&u, sink, cb, ud, next, sizeof(next), err, errcap);
        if (r == 0)
            return 0;
        if (r == 1) {
            snprintf(current, sizeof(current), "%s", next);
            continue;
        }
        return -1;
    }

    snprintf(err, errcap, "too many redirects");
    return -1;
}

int http_download(const char *url, const char *dest_path,
                  http_progress_cb cb, void *ud,
                  char *err, size_t errcap)
{
    sink_t sink;
    memset(&sink, 0, sizeof(sink));

    sink.file = fopen(dest_path, "wb");
    if (!sink.file) {
        snprintf(err, errcap, "cannot open %s for writing", dest_path);
        return -1;
    }

    int r = http_fetch(url, &sink, cb, ud, err, errcap);
    if (fclose(sink.file) != 0 && r == 0) {
        snprintf(err, errcap, "SD write failed");
        r = -1;
    }
    return r;
}

int http_get_mem(const char *url, unsigned char **out, size_t *out_len,
                 size_t max_len, char *err, size_t errcap)
{
    sink_t sink;
    memset(&sink, 0, sizeof(sink));

    if (out && out_len) {
        *out = NULL;
        *out_len = 0;
        sink.mem = out;
        sink.max = max_len ? max_len : (2 * 1024 * 1024);
    }

    int r = http_fetch(url, &sink, NULL, NULL, err, errcap);
    if (r != 0 && out && *out) {
        free(*out);
        *out = NULL;
    }
    if (r == 0 && out_len)
        *out_len = sink.len;
    return r;
}

#else /* !__SWITCH__ */

int http_download(const char *url, const char *dest_path,
                  http_progress_cb cb, void *ud,
                  char *err, size_t errcap)
{
    (void)url; (void)dest_path; (void)cb; (void)ud;
    snprintf(err, errcap, "download is only available on the Switch build");
    return -1;
}

int http_get_mem(const char *url, unsigned char **out, size_t *out_len,
                 size_t max_len, char *err, size_t errcap)
{
    (void)url; (void)out; (void)out_len; (void)max_len;
    snprintf(err, errcap, "download is only available on the Switch build");
    return -1;
}

#endif
