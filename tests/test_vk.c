#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "vk.h"

static const char *SAMPLE_OK =
    "{\"response\":{\"count\":1,\"items\":[{"
    "\"id\":789012,\"owner_id\":-123456,"
    "\"title\":\"Тестовое видео\",\"duration\":754,"
    "\"width\":1920,\"height\":1080,\"live\":0,"
    "\"files\":{"
    "\"mp4_240\":\"https://example.com/240.mp4?extra=a\","
    "\"mp4_360\":\"https://example.com/360.mp4?extra=b\","
    "\"mp4_480\":\"https://example.com/480.mp4?extra=c\","
    "\"mp4_720\":\"https://example.com/720.mp4?extra=d\","
    "\"mp4_1080\":\"https://example.com/1080.mp4?extra=e\","
    "\"hls\":\"https://example.com/index.m3u8?extra=f\""
    "}}]}}";

static const char *SAMPLE_ERROR =
    "{\"error\":{\"error_code\":5,\"error_msg\":\"User authorization failed\"}}";

static const char *SAMPLE_EMPTY = "{\"response\":{\"count\":0,\"items\":[]}}";

static void test_refs(void)
{
    int oid = 0, vid = 0;

    assert(vk_parse_ref("video-123456_789012", &oid, &vid) == 0);
    assert(oid == -123456 && vid == 789012);

    assert(vk_parse_ref("-123456_789012", &oid, &vid) == 0);
    assert(oid == -123456 && vid == 789012);

    assert(vk_parse_ref("https://vk.com/video-123456_789012", &oid, &vid) == 0);
    assert(oid == -123456 && vid == 789012);

    assert(vk_parse_ref("https://vkvideo.ru/video-123456_789012?list=abc",
                        &oid, &vid) == 0);
    assert(oid == -123456 && vid == 789012);

    assert(vk_parse_ref("https://vk.com/video?z=video-123456_789012",
                        &oid, &vid) == 0);
    assert(oid == -123456 && vid == 789012);

    assert(vk_parse_ref("https://vk.com/feed?z=video123_456", &oid, &vid) == 0);
    assert(oid == 123 && vid == 456);

    assert(vk_parse_ref("https://vk.com/clip-123_456", &oid, &vid) == 0);
    assert(oid == -123 && vid == 456);

    assert(vk_parse_ref("https://vk.com/feed", &oid, &vid) != 0);
    assert(vk_parse_ref("", &oid, &vid) != 0);
    assert(vk_parse_ref("video0_0", &oid, &vid) != 0);
}

static void test_path(void)
{
    char path[2048];
    assert(vk_build_get_path(path, sizeof(path), -123456, 789012, "tok") == 0);
    assert(strstr(path, "videos=-123456_789012") != NULL);
    assert(strstr(path, "access_token=tok") != NULL);
    assert(strstr(path, "/method/video.get") == path);
}

static void test_parse(void)
{
    vk_video_t v;
    char err[256];

    assert(vk_parse_response(SAMPLE_OK, &v, err, sizeof(err)) == 0);
    assert(v.owner_id == -123456 && v.id == 789012);
    assert(v.duration == 754);
    assert(strcmp(v.title, "Тестовое видео") == 0);
    assert(vk_max_quality(&v) == 1080);
    assert(strcmp(vk_pick_url(&v, 720), "https://example.com/720.mp4?extra=d") == 0);
    assert(strcmp(vk_pick_url(&v, 0), "https://example.com/1080.mp4?extra=e") == 0);

    assert(vk_parse_response(SAMPLE_ERROR, &v, err, sizeof(err)) != 0);
    assert(strstr(err, "5") != NULL);
    assert(strstr(err, "authorization") != NULL);

    assert(vk_parse_response(SAMPLE_EMPTY, &v, err, sizeof(err)) != 0);

    assert(vk_parse_response("not json", &v, err, sizeof(err)) != 0);
}

static void test_fallbacks(void)
{
    vk_video_t v;
    memset(&v, 0, sizeof(v));
    snprintf(v.mp4_360, sizeof(v.mp4_360), "https://example.com/360.mp4");
    snprintf(v.hls, sizeof(v.hls), "https://example.com/index.m3u8");

    assert(vk_max_quality(&v) == 360);
    assert(strcmp(vk_pick_url(&v, 1080), "https://example.com/360.mp4") == 0);

    memset(&v, 0, sizeof(v));
    snprintf(v.hls, sizeof(v.hls), "https://example.com/index.m3u8");
    assert(vk_max_quality(&v) == 0);
    assert(strcmp(vk_pick_url(&v, 0), "https://example.com/index.m3u8") == 0);
}

int main(void)
{
    test_refs();
    test_path();
    test_parse();
    test_fallbacks();
    puts("vk core tests: OK");
    return 0;
}
