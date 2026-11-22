#include "net.h"

#include <stdio.h>

#ifdef __SWITCH__
#include <switch.h>
#include <string.h>
#include <stdlib.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

static mbedtls_entropy_context  g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static mbedtls_ssl_config       g_conf;
static int g_ready;

int net_init(void)
{
    static const char pers[] = "vkvideo-switch";
    int ret;
    if (g_ready)
        return 0;

    /* mbedTLS' Switch port polls entropy via csrngGetRandomBytes(). */
    csrngInitialize();

    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    mbedtls_ssl_config_init(&g_conf);

    ret = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                                (const unsigned char *)pers, sizeof(pers) - 1);
    if (ret != 0)
        return -1;

    ret = mbedtls_ssl_config_defaults(&g_conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
        return -1;

    /* TODO(v0.2): pin a CA bundle instead of disabling verification. */
    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_drbg);

    g_ready = 1;
    return 0;
}

void *net_ssl_config(void)
{
    return g_ready ? (void *)&g_conf : NULL;
}

void net_exit(void)
{
    if (!g_ready)
        return;
    mbedtls_ssl_config_free(&g_conf);
    mbedtls_ctr_drbg_free(&g_drbg);
    mbedtls_entropy_free(&g_entropy);
    csrngExit();
    g_ready = 0;
}

static int read_response(mbedtls_ssl_context *ssl, char *out, size_t cap,
                         size_t *out_len, char *err, size_t errcap)
{
    char *buf = malloc(cap);
    size_t total = 0;
    int ret;

    if (!buf) {
        snprintf(err, errcap, "out of memory");
        return -1;
    }

    while (total < cap - 1) {
        ret = mbedtls_ssl_read(ssl, (unsigned char *)buf + total, cap - 1 - total);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (ret <= 0)
            break;
        total += (size_t)ret;
    }
    buf[total] = '\0';

    char *body = strstr(buf, "\r\n\r\n");
    if (!body) {
        snprintf(err, errcap, "no HTTP headers in response");
        free(buf);
        return -1;
    }
    body += 4;

    if (strncmp(buf, "HTTP/1.1 200", 12) != 0 && strncmp(buf, "HTTP/1.0 200", 12) != 0) {
        char status[64] = {0};
        memcpy(status, buf, 48);
        snprintf(err, errcap, "HTTP error: %.40s", status);
        free(buf);
        return -1;
    }

    size_t blen = total - (size_t)(body - buf);
    if (blen >= cap) {
        snprintf(err, errcap, "response too large");
        free(buf);
        return -1;
    }
    memcpy(out, body, blen);
    out[blen] = '\0';
    if (out_len)
        *out_len = blen;

    free(buf);
    return 0;
}

int net_https_get(const char *host, const char *path,
                  char *out, size_t cap, size_t *out_len,
                  char *err, size_t errcap)
{
    mbedtls_net_context server;
    mbedtls_ssl_context ssl;
    char request[2048];
    int ret, rc = -1;

    if (!g_ready && net_init() != 0) {
        snprintf(err, errcap, "TLS init failed");
        return -1;
    }

    mbedtls_net_init(&server);
    mbedtls_ssl_init(&ssl);

    ret = mbedtls_net_connect(&server, host, "443", MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        snprintf(err, errcap, "connect %s: -0x%04x", host, (unsigned)-ret);
        goto out;
    }

    ret = mbedtls_ssl_setup(&ssl, &g_conf);
    if (ret != 0) {
        snprintf(err, errcap, "ssl_setup: -0x%04x", (unsigned)-ret);
        goto out;
    }

    mbedtls_ssl_set_hostname(&ssl, host);
    mbedtls_ssl_set_bio(&ssl, &server, mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            snprintf(err, errcap, "handshake: -0x%04x", (unsigned)-ret);
            goto out;
        }
    }

    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "User-Agent: vkvideo-switch/0.1\r\n"
             "Accept: application/json\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host);

    size_t sent = 0, reqlen = strlen(request);
    while (sent < reqlen) {
        ret = mbedtls_ssl_write(&ssl, (const unsigned char *)request + sent,
                                reqlen - sent);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (ret <= 0) {
            snprintf(err, errcap, "write: -0x%04x", (unsigned)-ret);
            goto out;
        }
        sent += (size_t)ret;
    }

    rc = read_response(&ssl, out, cap, out_len, err, errcap);

out:
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_net_free(&server);
    mbedtls_ssl_free(&ssl);
    return rc;
}

#else /* !__SWITCH__ */

int net_init(void) { return 0; }
void net_exit(void) {}
void *net_ssl_config(void) { return NULL; }

int net_https_get(const char *host, const char *path,
                  char *out, size_t cap, size_t *out_len,
                  char *err, size_t errcap)
{
    (void)host;
    (void)path;
    (void)out;
    (void)cap;
    (void)out_len;
    snprintf(err, errcap, "network is only available on the Switch build");
    return -1;
}

#endif
