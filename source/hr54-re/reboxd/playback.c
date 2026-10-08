#include "playback.h"
#include "native_app.h"
#include "module_rpc.h"
#include "module_auth.h"
#include "system.h"
#include <sys/time.h>
#include <arpa/inet.h>

typedef struct {
    char module[64], item[256], title[256], session[256], token[128], url[2048];
    int playing, preparing, paused, live, stop, pause, resume, seek;
    int claimed, opening, cancelled;
    unsigned generation;
    uint64_t epoch;
    double started, duration, position, paused_at, paused_total;
    ReboxModule provider;
} Playback;
static Playback active;
static pthread_mutex_t playback_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t decoder_lock = PTHREAD_MUTEX_INITIALIZER;
static int listen_port = 8130;
static uint64_t next_epoch;
static char instance[33];
void rb_playback_port(int port) { listen_port = port; if(rb_random(instance,32))exit(1); }
static Playback snapshot(void) {
    pthread_mutex_lock(&playback_lock);
    Playback s = active;
    pthread_mutex_unlock(&playback_lock);
    return s;
}
int rb_playback_busy(const char *id) {
    Playback s = snapshot();
    return (s.playing || s.preparing) && (!id || !strcmp(id, s.module));
}
static int stop_decoder(uint64_t epoch) {
    pthread_mutex_lock(&decoder_lock);
    int rc = snapshot().epoch == epoch ? rb_decoder_stop() : 0;
    pthread_mutex_unlock(&decoder_lock);
    return rc;
}
static int safe_string(struct jval *v, char *out, size_t cap) {
    const char *s = jstr(v);
    if (!s || strlen(s) >= cap || strpbrk(s, "\r\n")) return -1;
    strcpy(out, s); return 0;
}
static int token_valid(const char *s) {
    size_t n = strlen(s);
    return n > 0 && n < 128 && strspn(s, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == n;
}
static int current(uint64_t epoch) {
    Playback s = snapshot();
    return s.epoch == epoch && (s.playing || s.preparing) && !s.cancelled;
}
static void cleanup(const ReboxModule *m, const char *session) {
    struct sb body = {0}; sb_puts(&body, "{");
    if (session && *session) { sb_puts(&body, "\"session\":"); sb_json_str(&body, session); }
    sb_puts(&body, "}"); RbReply reply;
    if (!rb_rpc(m, "POST", "/playback/stop", body.p, &reply, 5)) rb_reply_free(&reply);
    free(body.p);
}
static void cancel(uint64_t epoch) {
    pthread_mutex_lock(&playback_lock);
    if (active.epoch == epoch) {
        active.cancelled = 1;
        active.playing = active.preparing = active.paused = 0;
        active.module[0] = 0;
    }
    pthread_mutex_unlock(&playback_lock);
}
static double elapsed(const Playback *s) {
    double now = s->paused ? s->paused_at : mono_now();
    return s->position + now - s->started - s->paused_total;
}
#ifndef REBOX_HOST_TEST
static int start_decoder(const Playback *next) {
    pthread_mutex_lock(&decoder_lock);
    if (!current(next->epoch)) goto failed;
    char url[2048];
    if(*next->url)strcpy(url,next->url);
    else snprintf(url, sizeof url, "http://127.0.0.1:%d/module-stream/%s/%s", listen_port, next->module, next->token);
    fprintf(stderr,"reboxd: %s player=/var/opt/hr54/bin/hr54-play-url stream=%s URL=%s\n",next->module,*next->url?"direct HTTP":"moduleProxy MPEG-TS",*next->url?"[redacted]":"http://127.0.0.1/module-stream/[module]/[session]");
    pid_t child = fork();
    if (!child) {
        char *argv[] = {"/var/opt/hr54/bin/hr54-play-url", url, NULL};
        char *env[] = {"PATH=/bin:/usr/bin", "HR54_NATIVE_FRONTEND=1", NULL};
        for (int i = 3; i < 65536; i++) close(i);
        execve(argv[0], argv, env); _exit(127);
    }
    if (child < 0){fprintf(stderr,"reboxd: %s player fork failed errno=%d\n",next->module,errno);goto failed;}
    int status = 0; double deadline = mono_now() + 60; pid_t result;
    do {
        result = waitpid(child, &status, WNOHANG);
        if (result == child) break;
        nap(.05);
    } while (mono_now() < deadline && current(next->epoch));
    if (result != child) {
        /* An unreaped direct child cannot have its PID reused. */
        kill(child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        fprintf(stderr,"reboxd: %s player cancelled or timed out\n",next->module);
        goto failed;
    }
    fprintf(stderr,"reboxd: %s player exit=%d signal=%d\n",next->module,WIFEXITED(status)?WEXITSTATUS(status):-1,WIFSIGNALED(status)?WTERMSIG(status):0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) goto failed;
    deadline = mono_now() + 15;
    while (current(next->epoch)) {
        if (*next->url || snapshot().claimed) break;
        if (mono_now() > deadline){fprintf(stderr,"reboxd: %s player did not claim MPEG-TS stream\n",next->module);goto failed;}
        nap(.05);
    }
    pthread_mutex_unlock(&decoder_lock); return 0;
failed:
    pthread_mutex_unlock(&decoder_lock); return -1;
}
#endif
/* URLs are argv values, never shell fragments. Reject control characters,
 * credentials and malformed authorities before passing a direct plan on. */
static int http_url(const char *url) {
    const char *host=!strncmp(url,"http://",7)?url+7:!strncmp(url,"https://",8)?url+8:NULL;
    if(!host||!*host)return 0;
    for(const unsigned char *p=(const unsigned char *)url;*p;p++)if(*p<=32||*p==127)return 0;
    size_t n=strcspn(host,"/?#");if(!n||n>255)return 0;
    char authority[256];memcpy(authority,host,n);authority[n]=0;char *port=NULL;
    if(*authority=='['){char *end=strchr(authority,']');if(!end)return 0;*end=0;unsigned char address[16];if(inet_pton(AF_INET6,authority+1,address)!=1)return 0;if(end[1]){if(end[1]!=':')return 0;port=end+2;}}
    else{port=strchr(authority,':');if(port)*port++=0;if(!*authority)return 0;for(const unsigned char *p=(unsigned char *)authority;*p;p++)if(!isalnum(*p)&&*p!='.'&&*p!='-')return 0;}
    if(port){if(!*port)return 0;for(const char *p=port;*p;p++)if(*p<'0'||*p>'9')return 0;char *end;long number=strtol(port,&end,10);if(*end||number<1||number>65535)return 0;}
    return 1;
}
/* A replacement plan is the only way a module may request a decoder restart.
 * Both initial play and transport replacement pass the same validation. */
static int commit_plan(Playback *next, struct jval *plan, struct sb *out) {
    struct jval *stream = jget(plan, "stream"), *transport = jget(plan, "transport");
    const char *type = jstr(jget(plan, "type")), *kind = jstr(jget(stream, "kind"));
    if (!type || strcmp(type, "stream") || !kind ||
        (strcmp(kind,"moduleProxy")&&strcmp(kind,"http")) ||
        (!strcmp(kind,"moduleProxy")&&(safe_string(jget(stream,"token"),next->token,sizeof next->token)||!token_valid(next->token))) ||
        (!strcmp(kind,"http")&&(safe_string(jget(stream,"url"),next->url,sizeof next->url)||!http_url(next->url))) ||
        safe_string(jget(plan, "session"), next->session, sizeof next->session) || !*next->session ||
        safe_string(jget(plan, "title"), next->title, sizeof next->title)) return 502;
    next->stop = jbool(jget(transport, "stop"), 0);
    next->pause = jbool(jget(transport, "pause"), 0);
    next->resume = jbool(jget(transport, "resume"), 0);
    next->seek = jbool(jget(transport, "seek"), 0);
    next->duration = jnum(jget(plan, "duration"), 0);
    next->position = jnum(jget(plan, "position"), next->position);
    next->live = jbool(jget(plan, "live"), 0);
    if (next->duration < 0 || next->duration > 604800 || next->position < 0 || next->position > 604800) return 502;
    pthread_mutex_lock(&playback_lock);
    if (active.epoch != next->epoch || active.cancelled) { pthread_mutex_unlock(&playback_lock); return 409; }
    active = *next;
    active.preparing = 1;
    pthread_mutex_unlock(&playback_lock);
#ifndef REBOX_HOST_TEST
    if (start_decoder(next)) return 502;
#endif
    pthread_mutex_lock(&playback_lock);
    if (active.cancelled || active.epoch != next->epoch) { pthread_mutex_unlock(&playback_lock); return 409; }
    active.preparing = 0; active.playing = 1;
    active.started = mono_now(); active.generation++;
    pthread_mutex_unlock(&playback_lock);
    rb_playback_json(out); return 200;
}
static Playback begin(const ReboxModule *m, const char *item, double position, uint64_t expected) {
    Playback next = {0}; next.provider = *m;
    strcpy(next.module, m->id); strcpy(next.item, item); next.position = position;
    pthread_mutex_lock(&playback_lock);
    if (expected && (active.epoch != expected || active.cancelled)) { pthread_mutex_unlock(&playback_lock); return next; }
    next.generation = active.generation; next.epoch = ++next_epoch; next.preparing = 1;
    active = next;
    pthread_mutex_unlock(&playback_lock);
    return next;
}
int rb_play(ReboxRegistry *r, ReboxModule *m, const char *body, struct sb *out) {
    if(rb_native_busy(NULL))return 409;
    if (!m->installed) return 404;
    if (!m->enabled || !m->healthy || !m->playback) return 409;
    struct jval *request = json_parse(body, strlen(body)); char item[256];
    double position = jnum(jget(request, "startSeconds"), 0);
    int bad = safe_string(jget(request, "itemId"), item, sizeof item) || !*item || position < 0 || position > 604800;
    jfree(request); if (bad) return 400;
    if (rb_playback_busy(NULL)) {
        struct sb stopped = {0}; int rc = rb_transport(r, "stop", "{}", &stopped);
        free(stopped.p); if (rc != 200) return rc;
    }
    Playback next = begin(m, item, position, 0);
    RbReply reply; int rc = 502;
    if (!rb_rpc(m, "POST", "/play", body, &reply, 120)) {
        struct jval *plan = json_parse(reply.body.p, reply.body.len);
        rc = reply.status == 200 ? commit_plan(&next, plan, out) : reply.status;
        if(rc!=200)fprintf(stderr,"reboxd: %s preparation failed moduleHTTP=%d playbackHTTP=%d\n",m->id,reply.status,rc);
        jfree(plan); rb_reply_free(&reply);
    }else fprintf(stderr,"reboxd: %s playback preparation RPC failed\n",m->id);
    if (rc != 200) { cancel(next.epoch); cleanup(m, next.session); stop_decoder(next.epoch); }
    return rc;
}
int rb_transport(ReboxRegistry *r, const char *operation, const char *body, struct sb *out) {
    (void)r;
    Playback s = snapshot();
    int stop = !strcmp(operation, "stop");
    int supported = (stop && (s.stop || s.preparing)) || (!strcmp(operation, "pause") && s.pause) ||
        (!strcmp(operation, "resume") && s.resume) || (!strcmp(operation, "seek") && s.seek);
    if ((!s.playing && !s.preparing) || !supported) return 409;
    struct jval *request = json_parse(body, strlen(body));
    if (!request || request->t != J_OBJ || jget(request, "session")) { jfree(request); return 400; }
    size_t len = strlen(body); while (len && isspace((unsigned char)body[len - 1])) len--;
    struct sb params = {0}; sb_putn(&params, body, len - 1);
    if (*s.session) { if (request->n) sb_puts(&params, ","); sb_puts(&params, "\"session\":"); sb_json_str(&params, s.session); }
    sb_puts(&params, "}"); jfree(request);
    if (stop) cancel(s.epoch);
    char path[64]; snprintf(path, sizeof path, "/playback/%s", operation);
    RbReply reply; int rc = 502;
    if (!rb_rpc(&s.provider, "POST", path, params.p, &reply, stop ? 5 : 120)) {
        rc = reply.status;
        struct jval *v = json_parse(reply.body.p, reply.body.len);
        const char *type = jstr(jget(v, "type"));
        if (!stop && rc == 200 && type) {
            if (!current(s.epoch)) rc = 409;
            else {
                /* Cancel the old proxy before publishing a replacement token. */
                Playback next = begin(&s.provider, s.item, elapsed(&s), s.epoch);
                rc = next.epoch ? commit_plan(&next, v, out) : 409;
                if (rc != 200) { cancel(next.epoch); cleanup(&s.provider, next.session); stop_decoder(next.epoch); }
            }
        } else if (!stop && rc == 200) {
            pthread_mutex_lock(&playback_lock);
            if (active.epoch == s.epoch && !active.cancelled) {
                if (!strcmp(operation, "pause") && !active.paused) { active.paused = 1; active.paused_at = mono_now(); }
                if (!strcmp(operation, "resume") && active.paused) { active.paused = 0; active.paused_total += mono_now() - active.paused_at; }
            } else rc = 409;
            pthread_mutex_unlock(&playback_lock);
        }
        jfree(v); rb_reply_free(&reply);
    }
    free(params.p);
    if (stop && stop_decoder(s.epoch)) rc = 502;
    if (rc == 200 && !out->len) rb_playback_json(out);
    return rc;
}
void rb_playback_tick(const ReboxRegistry *registry) {
    Playback s = snapshot();
    if (!s.playing || s.preparing) return;
    const ReboxModule *provider=NULL;
    for(size_t i=0;i<registry->count;i++)if(!strcmp(registry->modules[i].id,s.module)){provider=&registry->modules[i];break;}
    if(!provider||provider->pid!=s.provider.pid||!provider->healthy){cancel(s.epoch);stop_decoder(s.epoch);return;}
    RbReply reply;
    if (rb_rpc(&s.provider, "GET", "/status", NULL, &reply, 1)) return;
    struct jval *v = json_parse(reply.body.p, reply.body.len), *p = jget(v, "playback");
    const char *session = jstr(jget(p, "session"));
    int ended = session && !strcmp(session, s.session) && !jbool(jget(p, "playing"), 1);
    jfree(v); rb_reply_free(&reply);
    if (ended && current(s.epoch)) { cancel(s.epoch); stop_decoder(s.epoch); }
}
void rb_playback_json(struct sb *out) {
    Playback s = snapshot();
    sb_fmt(out, "{\"ok\":true,\"playing\":%s,\"preparing\":%s,\"paused\":%s,\"live\":%s,\"generation\":%u,\"source\":",
        s.playing ? "true" : "false", s.preparing ? "true" : "false", s.paused ? "true" : "false", s.live ? "true" : "false", s.generation);
    sb_json_str(out, s.module); sb_puts(out, ",\"instance\":"); sb_json_str(out,instance); sb_puts(out, ",\"itemId\":"); sb_json_str(out, s.playing ? s.item : "");
    sb_puts(out, ",\"title\":"); sb_json_str(out, s.playing ? s.title : "");
    sb_fmt(out, ",\"elapsed\":%.0f,\"duration\":%.0f,\"transport\":{\"stop\":%s,\"pause\":%s,\"resume\":%s,\"seek\":%s}}",
        s.playing ? elapsed(&s) : 0, s.playing ? s.duration : 0, s.playing && s.stop ? "true" : "false",
        s.playing && s.pause ? "true" : "false", s.playing && s.resume ? "true" : "false", s.playing && s.seek ? "true" : "false");
}
static int stream_write(int fd,const void *bytes,size_t length,unsigned epoch){
 const char *p=bytes;double deadline=mono_now()+30;
 while(length&&current(epoch)){
  ssize_t n=send(fd,p,length,MSG_DONTWAIT|MSG_NOSIGNAL);
  if(n>0){p+=n;length-=n;deadline=mono_now()+30;continue;}
  if(n<0&&errno==EINTR)continue;
  if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){if(snapshot().paused)deadline=mono_now()+30;if(mono_now()>deadline)return -1;struct pollfd wait={fd,POLLOUT,0};int ready=poll(&wait,1,100);if(ready<0&&errno!=EINTR)return -1;continue;}
  return -1;
 }return length?-1:0;
}
void rb_stream_proxy(int client, const char *path) {
    char id[64]; const char *token = strchr(path, '/'); size_t n = token ? (size_t)(token - path) : 0;
    if (!n || n >= sizeof id || !token_valid(token + 1)) { rb_http_error(client, 404, "unknown stream"); return; }
    memcpy(id, path, n); id[n] = 0; token++;
    pthread_mutex_lock(&playback_lock);
    Playback s = active;
    int valid = !*s.url && *s.token && !strcmp(id, s.module) && !strcmp(token, s.token) && !s.opening && !s.claimed && !s.cancelled && (s.playing || s.preparing);
    if (valid) active.opening = 1;
    pthread_mutex_unlock(&playback_lock);
    if (!valid) { rb_http_error(client, 404, "expired stream"); return; }
    char route[160]; snprintf(route, sizeof route, "/stream/%s", token);
    int fd = rb_rpc_open(&s.provider, "GET", route, NULL, 1);
    if (fd < 0) { rb_http_error(client, 502, "module stream unavailable"); cancel(s.epoch); cleanup(&s.provider, s.session); return; }
    char head[8193]; size_t used = 0; int ready = 0, failed = 0;
    double deadline = mono_now() + 35;
    while (current(s.epoch) && mono_now() < deadline && used < sizeof head - 1) {
        struct pollfd p = {fd, POLLIN, 0}; int rc = poll(&p, 1, 100);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) { failed = 1; break; }
        if (!rc) continue;
        ssize_t k = read(fd, head + used, 1); if (k != 1) { failed = 1; break; }
        used++;
        if (used >= 4 && !memcmp(head + used - 4, "\r\n\r\n", 4)) { ready = 1; break; }
    }
    head[used] = 0; int status = 0;
    if (!ready || sscanf(head, "HTTP/%*s %d", &status) != 1 || status != 200 || strcasestr(head, "Transfer-Encoding:")) failed = 1;
    int sent = 0, eof = 0; char bytes[32768]; deadline = mono_now() + 35;
    while (!failed && current(s.epoch)) {
        /* A paused module may intentionally stop producing bytes indefinitely. */
        if (snapshot().paused) deadline = mono_now() + 35;
        if (mono_now() > deadline) { failed = 1; break; }
        struct pollfd p = {fd, POLLIN, 0}; int rc = poll(&p, 1, 100);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) { failed = 1; break; }
        if (!rc) continue;
        ssize_t k = read(fd, bytes, sizeof bytes);
        if (!k) { eof = 1; break; }
        if (k < 0) { failed = 1; break; }
        if (!sent) {
            if ((unsigned char)bytes[0] != 0x47) { failed = 1; break; }
            const char *h = "HTTP/1.0 200 OK\r\nContent-Type: video/mp2t\r\nConnection: close\r\n\r\n";
            if (stream_write(client, h, strlen(h), s.epoch)) { failed = 1; break; }
            sent = 1;
            pthread_mutex_lock(&playback_lock);
            if (active.epoch == s.epoch) active.claimed = 1;
            pthread_mutex_unlock(&playback_lock);
        }
        if (stream_write(client, bytes, k, s.epoch)) { failed = 1; break; }
        deadline = mono_now() + 35;
    }
    close(fd);
    if (!sent) rb_http_error(client, 502, "module stream failed");
    if(failed)fprintf(stderr,"reboxd: %s proxy failed upstreamHTTP=%d headerReady=%d mediaStarted=%d\n",s.module,status,ready,sent);
    /* EOF precedes decoder drain. The module reports final completion through
     * status; any I/O failure cancels only this preparation/session epoch. */
    if ((failed || (!eof && current(s.epoch))) && current(s.epoch)) {
        cancel(s.epoch); cleanup(&s.provider, s.session); stop_decoder(s.epoch);
    }
}
