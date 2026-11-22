#include "resume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__

#include "log.h"

#define POS_PATH "sdmc:/switch/vkvideo/positions.txt"
#define POS_TMP  "sdmc:/switch/vkvideo/positions.tmp"

static int parse_line(const char *line, const char *key, double *out)
{
    size_t kl = strlen(key);
    if (strncmp(line, key, kl) != 0 || line[kl] != '=')
        return 0;
    *out = atof(line + kl + 1);
    return 1;
}

double resume_load(const char *key)
{
    if (!key || !*key)
        return -1;

    FILE *f = fopen(POS_PATH, "r");
    if (!f)
        return -1;

    char line[256];
    double val = -1;
    while (fgets(line, sizeof(line), f))
        parse_line(line, key, &val);

    fclose(f);
    return val;
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return -1;
    }
    char buf[4096];
    size_t n;
    int ok = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = -1; break; }
    fclose(in);
    fclose(out);
    return ok;
}

static void rewrite(const char *key, double new_pos, int remove_key)
{
    FILE *in = fopen(POS_PATH, "r");
    FILE *out = fopen(POS_TMP, "w");
    if (!out) {
        if (in) fclose(in);
        return;
    }

    char line[256];
    int wrote = 0;
    while (in && fgets(line, sizeof(line), in)) {
        if (key && strncmp(line, key, strlen(key)) == 0 &&
            line[strlen(key)] == '=') {
            if (!remove_key)
                fprintf(out, "%s=%.3f\n", key, new_pos);
            wrote = 1;
        } else {
            fputs(line, out);
        }
    }
    if (in)
        fclose(in);

    if (!wrote && !remove_key && key)
        fprintf(out, "%s=%.3f\n", key, new_pos);

    fclose(out);
    remove(POS_PATH);
    if (rename(POS_TMP, POS_PATH) != 0) {
        if (copy_file(POS_TMP, POS_PATH) == 0)
            remove(POS_TMP);
        else
            applog("resume: write failed");
    }
}

void resume_save(const char *key, double pos)
{
    if (!key || !*key || pos < 0)
        return;
    rewrite(key, pos, 0);
}

void resume_clear(const char *key)
{
    if (!key || !*key)
        return;
    rewrite(key, 0, 1);
}

#else

double resume_load(const char *key) { (void)key; return -1; }
void resume_save(const char *key, double pos) { (void)key; (void)pos; }
void resume_clear(const char *key) { (void)key; }

#endif
