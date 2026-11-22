#pragma once

#include <stddef.h>
#include <stdint.h>

typedef void (*http_progress_cb)(void *ud, uint64_t got, uint64_t total);

/*
 * Downloads url into dest_path (overwrites), following up to 5 redirects.
 * total is 0 when the server does not report Content-Length.
 * Returns 0 on success.
 */
int http_download(const char *url, const char *dest_path,
                  http_progress_cb cb, void *ud,
                  char *err, size_t errcap);

/* Fetches url into a malloc'd buffer (max_len bytes). Caller frees. */
int http_get_mem(const char *url, unsigned char **out, size_t *out_len,
                 size_t max_len, char *err, size_t errcap);
