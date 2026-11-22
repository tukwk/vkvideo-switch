#pragma once

/* Per-video resume positions, stored on SD. Returns -1 if unknown. */
double resume_load(const char *key);
void resume_save(const char *key, double pos);
void resume_clear(const char *key);
