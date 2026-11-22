#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

int config_load(app_config_t *cfg, const char *path)
{
    FILE *f;
    char line[1024];

    memset(cfg, 0, sizeof(*cfg));
    cfg->preferred_quality = 0;

    f = fopen(path, "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        char *eq;

        if (!*s || *s == '#' || *s == ';')
            continue;

        eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';

        char *key = trim(s);
        char *val = trim(eq + 1);

        if (strcmp(key, "token") == 0)
            snprintf(cfg->token, sizeof(cfg->token), "%s", val);
        else if (strcmp(key, "last") == 0)
            snprintf(cfg->last_ref, sizeof(cfg->last_ref), "%s", val);
        else if (strcmp(key, "quality") == 0)
            cfg->preferred_quality = atoi(val);
    }

    fclose(f);
    return 0;
}

int config_save(const app_config_t *cfg, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;

    fprintf(f,
            "# VK Video for Nintendo Switch\n"
            "# token — access_token с правами video\n"
            "token=%s\n"
            "last=%s\n"
            "quality=%d\n",
            cfg->token, cfg->last_ref, cfg->preferred_quality);
    fclose(f);
    return 0;
}
