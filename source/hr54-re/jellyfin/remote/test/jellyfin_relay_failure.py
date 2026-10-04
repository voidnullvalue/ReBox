#!/usr/bin/env python3
"""Isolated regressions for decoder claims, failed upstream media, and stale state."""
import http.server, importlib.util, json, os, pathlib, signal, socket, subprocess, tempfile, threading, time, urllib.request, urllib.error
R = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('mock', R/'test/mock_jellyfin.py')
mock = importlib.util.module_from_spec(spec); spec.loader.exec_module(mock)
class Handler(mock.Handler):
    mode = 'reject'
    def do_GET(self):
        if self.path.startswith('/Items/') and not self.path.endswith('/PlaybackInfo'):
            item = self.path.split('/')[2]
            return self.reply_json({'Id':item,'Name':'Buffered Episode','MediaType':'Video',
                'LocationType':'Original','RunTimeTicks':30000000 if self.mode in ('end','fast') else None,
                'MediaSources':[{'MediaStreams':[{'Type':'Video'}]}]})
        if self.path.startswith('/videos/'):
            if self.mode == 'reject': return self.reply_json({'error':'transcode failed'}, 404)
            self.send_response(200); self.end_headers()
            if self.mode == 'empty': return
            if self.mode == 'invalid': self.wfile.write(b'not media'); return
            self.wfile.write(b'\x47' + b'T'*187); self.wfile.flush()
            if self.mode != "fast": time.sleep(1)
            return
        super().do_GET()
with tempfile.TemporaryDirectory(prefix='jf-relay-failure-') as td:
    t = pathlib.Path(td)
    harness = t/'state.c'
    harness.write_text('#define main receiver_main\n#include "' + str(R/'hr54_jf.c') + '"\n#undef main\n#include <assert.h>\n' + r'''
int main(int argc, char **argv) {
    persist_root = argv[1]; S = calloc(1, sizeof *S);
    S->streams[0].in_use = 1; S->streams[0].claimed = 1;
    strcpy(S->streams[0].token, "new");
    assert(wait_for_claim("new", .2) == 0);
    S->streams[0].ready = 1;
    assert(wait_for_claim("new", .2) == 1);
    S->streams[0].duration = 120;
    S->play_live = 1; /* Frigate -> Jellyfin */
    playback_begin_locked("new", "Episode", "vid1", 0, 1);
    assert(S->play_live == 0 && active_slot_locked());
    struct sb out = {0}; assert(transport_action("pause", &out) == 0);
    assert(S->streams[0].paused); free(out.p);
    jellyfin_relay_finished("old"); assert(S->playing);
    S->streams[0].paused = 0; S->play_started = mono_now() - 30;
    assert(S->play_duration == 120);
    double remaining = jellyfin_drain_remaining_locked("new", mono_now());
    assert(remaining > 104 && remaining <= 105);
    S->play_base = 90;
    remaining = jellyfin_drain_remaining_locked("new", mono_now());
    assert(remaining > 14 && remaining <= 15); /* offset is not waited twice */
    assert(jellyfin_drain_remaining_locked("old", mono_now()) == 0);
    S->streams[0].draining = 1; memset(&out, 0, sizeof out);
    assert(transport_action("pause", &out) == -1);
    assert(transport_action("resume", &out) == -1);
    S->streams[0].closed = 1;
    assert(jellyfin_drain_remaining_locked("new", mono_now()) == 0);
    S->streams[0].closed = 0; S->play_duration = 0;
    remaining = jellyfin_drain_remaining_locked("new", mono_now());
    assert(remaining > 14 && remaining <= 15); /* missing runtime has bounded fallback */
    assert(jellyfin_drain_remaining_locked("new", mono_now() - 16) == 0);
    S->streams[0].closed = 1;
    assert(wait_for_claim("new", .2) == -1);
    struct sb profile = {0}; playbackinfo_body(&profile, "vid1", 21, jf_quality_safe(0));
    struct jval *v = json_parse(profile.p, profile.len);
    assert(jnum(jget(v, "StartTimeTicks"), -1) == 210000000);
    assert(!jbool(jget(v, "AllowVideoStreamCopy"), 1));
    jfree(v); free(profile.p);
    memset(&profile, 0, sizeof profile); playbackinfo_body(&profile, "vid1", 0, jf_quality_safe(0));
    v = json_parse(profile.p, profile.len);
    assert(jbool(jget(v, "AllowVideoStreamCopy"), 0));
    jfree(v); free(profile.p);
    native_frontend = 1;
    playback_begin_locked("native", "Native", "vid1", 0, 1);
    assert(S->return_to_tv == 0); /* old Android flag is safe in native mode */
    native_frontend = 0;
    playback_begin_locked("legacy", "Legacy", "vid1", 0, 1);
    assert(S->return_to_tv == 1); /* retain API compatibility */
    puts("PASS decoder claim is not readiness, failed claim rejected, camera -> Jellyfin transport, old relay cannot end new session, native presentation ignores legacy return flag");
    return 0;
}
''')
    subprocess.run(['cc','-std=c11','-O1','-o',str(t/'state'),str(harness)],check=True)
    subprocess.run([str(t/'state'),str(t)],check=True)
    mock.APPROVE_FILE = str(t/'approved'); pathlib.Path(mock.APPROVE_FILE).touch()
    mock.REQUEST_LOG = str(t/'requests')
    upstream = http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
    upstream.daemon_threads = True
    threading.Thread(target=upstream.serve_forever,daemon=True).start()
    subprocess.run(['cc','-std=c11','-O1','-o',str(t/'backend'),'-DJF_DECODER_STARTUP_GRACE=0.2',str(R/'hr54_jf.c')],check=True)
    wrapper = t/'play.sh'
    wrapper.write_text('#!/bin/sh\nexec >/dev/null 2>&1\ncurl -s "$1" >/dev/null &\n')
    wrapper.chmod(0o700)
    sock = socket.socket(); sock.bind(('127.0.0.1',0)); port = sock.getsockname()[1]; sock.close()
    proc = subprocess.Popen([str(t/'backend'),str(R/'static'),'127.0.0.1',str(upstream.server_port),str(port),'--no-launcher'],env=dict(os.environ,JF_PERSIST_ROOT=str(t/'persist'),JF_PLAY_CMD=str(wrapper)),stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
    def api(path, body=None):
        req = urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=None if body is None else json.dumps(body).encode(),headers={'Content-Type':'application/json'})
        try:
            with urllib.request.urlopen(req,timeout=20) as r:return r.status,json.load(r)
        except urllib.error.HTTPError as e:return e.code,json.load(e)
    try:
        time.sleep(.4)
        api('/api/auth/start',{}); assert api('/api/auth/poll')[1]['authenticated']
        for mode in ['reject','empty','invalid']:
            Handler.mode = mode
            code,result = api('/api/play',{'itemId':'vid1','returnToTv':True})
            assert code==502 and not result['ok'],(mode,code,result)
            state = api('/api/state')[1]
            assert not state['playing'] and not any(state['transport'].values()),(mode,state)
        Handler.mode = 'end'
        assert api('/api/play',{'itemId':'vid1','returnToTv':True})[1]['playing']
        time.sleep(1.3)
        state = api('/api/state')[1]
        assert state['playing'] and state['draining'] and state['duration']==3,state
        assert state['transport']['stop'] and state['transport']['seek'] and not state['transport']['pause'],state
        assert api('/api/playback/pause',{})[0]==409
        assert api('/api/playback/resume',{})[0]==409
        time.sleep(2.3)
        state = api('/api/state')[1]
        assert not state['playing'] and not any(state['transport'].values()),state
        assert api('/api/play',{'itemId':'vid1','returnToTv':True})[1]['playing']
        time.sleep(1.3);assert api('/api/state')[1]['draining']
        assert api('/api/playback/stop',{})[1]['stopped']
        assert not api('/api/state')[1]['playing']
        Handler.mode = 'fast'
        result = api('/api/play',{'itemId':'vid1','returnToTv':True})[1]
        assert result['playing'],result
        state = api('/api/state')[1];assert state['playing'] and state['draining'],state
        assert api('/api/playback/stop',{})[1]['stopped']
        Handler.mode = 'unknown'
        assert api('/api/play',{'itemId':'vid1','returnToTv':True})[1]['playing']
        time.sleep(1.6);assert not api('/api/state')[1]['playing']
        print('PASS failed media rejected; clean EOF retains buffered playback until runtime/startup drain; truthful transport, explicit stop cancellation, unknown-runtime fallback')
    finally:
        os.killpg(proc.pid,signal.SIGTERM); proc.wait(timeout=5); upstream.shutdown()
