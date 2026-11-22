#pragma once

#include <stddef.h>

typedef struct {
    char token[256];
    char last_ref[512];
    int  preferred_quality; /* 1080/720/480/360/240, 0 = auto */
} app_config_t;

/* Loads key=value config. Returns 0 if the file was read, -1 otherwise.
 * Missing keys keep their defaults. */
int config_load(app_config_t *cfg, const char *path);

int config_save(const app_config_t *cfg, const char *path);
