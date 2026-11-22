#pragma once

#include <stddef.h>

#define VK_MAX_URL   2048
#define VK_MAX_TITLE 256

typedef struct {
    int  owner_id;
    int  id;
    int  duration;   /* seconds */
    int  width;
    int  height;
    int  live;
    char title[VK_MAX_TITLE];

    char mp4_240[VK_MAX_URL];
    char mp4_360[VK_MAX_URL];
    char mp4_480[VK_MAX_URL];
    char mp4_720[VK_MAX_URL];
    char mp4_1080[VK_MAX_URL];
    char hls[VK_MAX_URL];
    char external[VK_MAX_URL];
} vk_video_t;

/*
 * Accepts any of:
 *   video-123456_789012
 *   -123456_789012
 *   https://vk.com/video-123456_789012
 *   https://vkvideo.ru/video-123456_789012
 *   https://vk.com/video?z=video-123456_789012
 *   https://vk.com/feed?z=video-123456_789012
 * Returns 0 on success.
 */
int vk_parse_ref(const char *input, int *owner_id, int *video_id);

/* Builds "/method/video.get?videos=<oid>_<id>&v=5.199&access_token=..." */
int vk_build_get_path(char *dst, size_t cap, int owner_id, int video_id,
                      const char *token);

/*
 * Parses a video.get response. Returns 0 on success, -1 on API/JSON error
 * (err receives a human readable message).
 */
int vk_parse_response(const char *json, vk_video_t *out, char *err, size_t errcap);

/* Highest available quality: 1080/720/480/360/240, or 0 if nothing usable. */
int vk_max_quality(const vk_video_t *v);

/* Quality that vk_pick_url() will actually return for the requested one
 * (falls back to lower resolutions). Returns 0 for HLS/external-only. */
int vk_effective_quality(const vk_video_t *v, int quality);

/* Returns URL for the requested quality or the next best one (never NULL). */
const char *vk_pick_url(const vk_video_t *v, int quality);
