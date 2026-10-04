#define main receiver_main
#include "../hr54_jf.c"
#undef main
#include <assert.h>

int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "--request")) { /* Emit the actual native request for upstream-only probes. */
        S = calloc(1, sizeof *S);
        snprintf(S->user_id, sizeof S->user_id, "%s", argv[2]);
        struct sb body = {0}; playbackinfo_body(&body, argv[3], 0, atoi(argv[4]));
        puts(body.p); free(body.p); free(S); return 0;
    }
    assert(argc == 2);
    persist_root = argv[1]; S = calloc(1, sizeof *S);
    char config[640]; snprintf(config, sizeof config, "%s/config", persist_root);
    assert(!mkdir_p_mode(config, 0700));
    jf_quality_load();
    assert(S->jf_quality_bitrate == jf_qualities[JF_QUALITY_DEFAULT].bitrate);
    double bad[] = {0, -1, 50000000, 16000001, 12000000.5, INFINITY, NAN, 1e300};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        assert(jf_quality_index(bad[i]) < 0 && jf_quality_safe(bad[i]) == jf_qualities[JF_QUALITY_DEFAULT].bitrate);
    for (int i = -1; i < 3; i++) {
        int rate = i < 0 ? jf_quality_safe(0) : jf_qualities[i].bitrate;
        struct sb body = {0}; playbackinfo_body(&body, "title", 0, rate);
        struct jval *v = json_parse(body.p, body.len), *profile = jget(v, "DeviceProfile");
        assert(jnum(jget(v, "MaxStreamingBitrate"), 0) == rate);
        assert(jnum(jget(profile, "MaxStreamingBitrate"), 0) == rate);
        assert(jbool(jget(v, "AllowVideoStreamCopy"), 0));
        assert(!jbool(jget(v, "AllowAudioStreamCopy"), 1));
        assert(!jbool(jget(v, "EnableDirectPlay"), 1) && !jbool(jget(v, "EnableDirectStream"), 1));
        assert(jbool(jget(v, "EnableTranscoding"), 0));
        assert(jget(profile, "DirectPlayProfiles")->n == 0);
        struct jval *tp = jnth(jget(profile, "TranscodingProfiles"), 0);
        assert(!strcmp(jstr(jget(tp, "Container")), "ts"));
        assert(!strcmp(jstr(jget(tp, "Protocol")), "http"));
        assert(!strcmp(jstr(jget(tp, "VideoCodec")), "h264"));
        assert(!strcmp(jstr(jget(tp, "AudioCodec")), "ac3"));
        assert(!strcmp(jstr(jget(tp, "MaxAudioChannels")), "2"));
        jfree(v); free(body.p);
        assert(!jf_quality_save_locked(rate)); S->jf_quality_bitrate = 0;
        jf_quality_load(); assert(S->jf_quality_bitrate == rate);
        body = (struct sb){0}; playbackinfo_body(&body, "title", 30, rate);
        v = json_parse(body.p, body.len);
        assert(!jbool(jget(v, "AllowVideoStreamCopy"), 1)); /* existing seek exception */
        assert(jnum(jget(v, "MaxStreamingBitrate"), 0) == rate);
        jfree(v); free(body.p);
    }
    char path[640]; jf_quality_path(path, sizeof path);
    assert(!atomic_write(path, "{\"jellyfinVideoBitrate\":50000000}", strlen("{\"jellyfinVideoBitrate\":50000000}"), 0600));
    jf_quality_load(); assert(S->jf_quality_bitrate == jf_quality_safe(0));
    assert(!atomic_write(path, "broken", 6, 0600));
    jf_quality_load(); assert(S->jf_quality_bitrate == jf_quality_safe(0));
    snprintf(config, sizeof config, "%s/log", persist_root);
    assert(!mkdir_p_mode(config, 0700));
    const char *fake = "{\"Bitrate\":15000000,\"MediaStreams\":[{\"Type\":\"Video\",\"Codec\":\"SECRET_CODEC\",\"BitRate\":14000000,\"Width\":1920,\"Height\":1080}]}";
    struct jval *src = json_parse(fake, strlen(fake));
    jf_playback_diagnostic(src, "/videos/PRIVATE_PATH?ApiKey=SECRET_TOKEN&VideoCodec=SECRET_CODEC&AudioCodec=ac3&VideoBitrate=11616000&AudioBitrate=384000", jf_quality_safe(0), 0);
    jfree(src); snprintf(path, sizeof path, "%s/log/jf.log", persist_root);
    size_t len; char *logged = read_file(path, &len);
    assert(logged && strstr(logged, "requested ceiling=12000000") && !strstr(logged, "SECRET") && !strstr(logged, "PRIVATE_PATH"));
    free(logged);
    puts("PASS default, both ceilings at all levels, invalid values, persistence, corrupt-file fallback, HTTP/TS/H264/AC3 contract, copy and offset exception");
    free(S); return 0;
}
