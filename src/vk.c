#include "vk.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/cJSON.h"

static void copy_field(char *dst, size_t cap, const cJSON *obj, const char *name)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsString(it) && it->valuestring) {
        snprintf(dst, cap, "%s", it->valuestring);
    }
}

static int number_field(const cJSON *obj, const char *name)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsNumber(it) ? it->valueint : 0;
}

/* "video-123_456" -> owner=-123, id=456 */
static int parse_pair(const char *s, int *owner_id, int *video_id)
{
    char *end = NULL;
    long owner, id;

    if (*s == 'c' && strncmp(s, "clip", 4) == 0)
        s += 4;

    owner = strtol(s, &end, 10);
    if (end == s || *end != '_')
        return -1;

    s = end + 1;
    id = strtol(s, &end, 10);
    if (end == s)
        return -1;

    if (*end != '\0' && *end != '?' && *end != '&' && *end != '/')
        return -1;

    if (owner == 0 || id <= 0)
        return -1;

    *owner_id = (int)owner;
    *video_id = (int)id;
    return 0;
}

int vk_parse_ref(const char *input, int *owner_id, int *video_id)
{
    const char *p;

    if (!input || !*input)
        return -1;

    while (isspace((unsigned char)*input))
        input++;

    /* Scan for "video" / "clip" markers, preferring the last one. */
    p = input;
    for (const char *q = input; (q = strstr(q, "video")) != NULL; q += 5) {
        p = q + 5;
    }
    if (p != input) {
        if (parse_pair(p, owner_id, video_id) == 0)
            return 0;
    }

    for (const char *q = input; (q = strstr(q, "clip")) != NULL; q += 4) {
        p = q + 4;
    }
    if (p != input && parse_pair(p, owner_id, video_id) == 0)
        return 0;

    /* Bare "-123_456". */
    return parse_pair(input, owner_id, video_id);
}

int vk_build_get_path(char *dst, size_t cap, int owner_id, int video_id,
                      const char *token)
{
    int n = snprintf(dst, cap,
                     "/method/video.get?v=5.199&videos=%d_%d&access_token=%s",
                     owner_id, video_id, token);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

int vk_parse_response(const char *json, vk_video_t *out, char *err, size_t errcap)
{
    cJSON *root, *response, *items, *item, *files;

    if (err && errcap)
        err[0] = '\0';

    root = cJSON_Parse(json);
    if (!root) {
        if (err)
            snprintf(err, errcap, "Ответ VK не является JSON");
        return -1;
    }

    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (error) {
        int code = number_field(error, "error_code");
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(error, "error_msg");
        if (err) {
            snprintf(err, errcap, "VK API %d: %s", code,
                     cJSON_IsString(msg) ? msg->valuestring : "unknown error");
        }
        cJSON_Delete(root);
        return -1;
    }

    response = cJSON_GetObjectItemCaseSensitive(root, "response");
    items = response ? cJSON_GetObjectItemCaseSensitive(response, "items") : NULL;
    item = cJSON_IsArray(items) ? cJSON_GetArrayItem(items, 0) : NULL;
    if (!item) {
        if (err)
            snprintf(err, errcap, "Видео не найдено (нет доступа или удалено)");
        cJSON_Delete(root);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->owner_id = number_field(item, "owner_id");
    out->id       = number_field(item, "id");
    out->duration = number_field(item, "duration");
    out->width    = number_field(item, "width");
    out->height   = number_field(item, "height");
    out->live     = number_field(item, "live");

    const cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
    if (cJSON_IsString(title) && title->valuestring)
        snprintf(out->title, sizeof(out->title), "%s", title->valuestring);

    files = cJSON_GetObjectItemCaseSensitive(item, "files");
    if (files) {
        copy_field(out->mp4_240,  sizeof(out->mp4_240),  files, "mp4_240");
        copy_field(out->mp4_360,  sizeof(out->mp4_360),  files, "mp4_360");
        copy_field(out->mp4_480,  sizeof(out->mp4_480),  files, "mp4_480");
        copy_field(out->mp4_720,  sizeof(out->mp4_720),  files, "mp4_720");
        copy_field(out->mp4_1080, sizeof(out->mp4_1080), files, "mp4_1080");
        copy_field(out->hls,      sizeof(out->hls),      files, "hls");
        copy_field(out->external, sizeof(out->external), files, "external");
    }

    cJSON_Delete(root);
    return 0;
}

int vk_max_quality(const vk_video_t *v)
{
    if (v->mp4_1080[0]) return 1080;
    if (v->mp4_720[0])  return 720;
    if (v->mp4_480[0])  return 480;
    if (v->mp4_360[0])  return 360;
    if (v->mp4_240[0])  return 240;
    return 0;
}

int vk_effective_quality(const vk_video_t *v, int quality)
{
    if (quality <= 0)
        return vk_max_quality(v);

    if (quality >= 1080 && v->mp4_1080[0]) return 1080;
    if (quality >= 720  && v->mp4_720[0])  return 720;
    if (quality >= 480  && v->mp4_480[0])  return 480;
    if (quality >= 360  && v->mp4_360[0])  return 360;
    if (v->mp4_240[0]) return 240;
    return 0;
}

const char *vk_pick_url(const vk_video_t *v, int quality)
{
    static const char *empty = "";

    if (quality <= 0)
        quality = vk_max_quality(v);

    switch (quality) {
    case 1080: if (v->mp4_1080[0]) return v->mp4_1080; /* fallthrough */
    case 720:  if (v->mp4_720[0])  return v->mp4_720;  /* fallthrough */
    case 480:  if (v->mp4_480[0])  return v->mp4_480;  /* fallthrough */
    case 360:  if (v->mp4_360[0])  return v->mp4_360;  /* fallthrough */
    case 240:  if (v->mp4_240[0])  return v->mp4_240;  /* fallthrough */
    default:   break;
    }

    if (v->hls[0])      return v->hls;
    if (v->external[0]) return v->external;
    return empty;
}
