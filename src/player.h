#pragma once

#include <stddef.h>

/*
 * Plays a local MP4/MOV file full-screen using ffmpeg (software decode),
 * libnx framebuffer for video and audout for audio.
 *
 * The caller must close the console before calling this and may re-init it
 * afterwards. Returns 0 when the user stops playback, negative on error.
 */
#define PLAYER_RETRY_LOWER (-2)

int player_play(const char *url, const char *title, double start_pos,
                const char *resume_key, double *end_pos,
                char *err, size_t errcap);
