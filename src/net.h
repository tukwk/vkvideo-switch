#pragma once

#include <stddef.h>

int  net_init(void);
void net_exit(void);

/* mbedTLS SSL config used by both the API client and the downloader. */
void *net_ssl_config(void);

/*
 * Performs an HTTPS GET on host:443 and writes the response body into out
 * (NUL terminated). Returns 0 on success.
 */
int net_https_get(const char *host, const char *path,
                  char *out, size_t cap, size_t *out_len,
                  char *err, size_t errcap);
