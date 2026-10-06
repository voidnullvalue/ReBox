"""Generic module API contracts against the pre-existing local Jellyfin mock."""
import importlib.util
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import time
import unittest
import urllib.error
import urllib.request
import runtime

ROOT = runtime.ROOT
class Jellyfin(unittest.TestCase):
    start = runtime.Runtime.start
    stop = runtime.Runtime.stop
    get = runtime.Runtime.get

    def setUp(self):
        runtime.Runtime.setUp(self)
        module = self.root / 'modules/jellyfin'
        (module / 'bin').mkdir(parents=True)
        shutil.copy(ROOT / '../modules/jellyfin/module.json', module / 'module.json')
        shutil.copy(ROOT / 'build/jellyfin-module', module / 'bin/module')
        state = self.root / 'module-state'
        state.mkdir()
        (state / 'jellyfin.json').write_text('{"schema":1,"enabled":true}')
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            self.mock_port = s.getsockname()[1]
        self.approve = self.base / 'approved'
        self.requests = self.base / 'requests'
        env = {**os.environ, 'MOCK_APPROVE_FILE': str(self.approve), 'MOCK_REQUEST_LOG': str(self.requests)}
        mock = subprocess.Popen([sys.executable, str(ROOT / '../jellyfin/remote/test/mock_jellyfin.py'), str(self.mock_port)], env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: (mock.terminate(), mock.wait(timeout=5), mock.stdout.close()))
        self.assertIn(b'mock jellyfin', mock.stdout.readline())
        self.data = self.root / 'module-data/jellyfin'
        config = self.data / 'config'
        config.mkdir(parents=True)
        self.device_id = '1234567890abcdef1234567890abcdef'
        (config / 'config.json').write_text(json.dumps({'server': f'http://127.0.0.1:{self.mock_port}', 'device_id': self.device_id, 'user_id': 'u1234abcd', 'user_name': 'void'}))
        (config / 'token').write_text('goodtoken-goodtoken-goodtoken')
        (config / 'token').chmod(0o600)
        self.start()
        self.master = (state / 'management.secret').read_text()
        self.prefix = '/api/modules/jellyfin'

    def request(self, path, body=None, authorized=False, binary=False):
        headers = {'Content-Type': 'application/json'}
        if authorized:
            headers['Authorization'] = 'Bearer ' + self.master
        req = urllib.request.Request(self.url + path, data=None if body is None else json.dumps(body).encode(), headers=headers)
        with urllib.request.urlopen(req, timeout=15) as response:
            return response.read() if binary else json.load(response)

    def test_existing_auth_and_generic_root(self):
        self.assertTrue(self.get(self.prefix + '/status')['authenticated'])
        root = self.get(self.prefix + '/browse')
        self.assertEqual([x['title'] for x in root['items'][:2]], ['All titles', 'Continue Watching'])
        self.assertEqual(root['items'][2]['title'], 'Movies')
        self.assertTrue(all(x['kind'] == 'folder' for x in root['items']))
        items = self.get(self.prefix + '/browse?parent=%40all')['items']
        self.assertEqual(items[0]['duration'], 7200)
        self.assertEqual(items[0]['artwork'], 'vid1')
        self.assertTrue(items[0]['playable'])
        self.assertFalse(items[2]['playable'])
        self.assertEqual(json.loads((self.data / 'config/config.json').read_text())['device_id'], self.device_id)
        self.assertEqual((self.data / 'config/token').read_text(), 'goodtoken-goodtoken-goodtoken')

    def test_settings_auth_and_art(self):
        fields = self.get(self.prefix + '/settings')['fields']
        self.assertEqual(fields[0]['key'], 'videoBitrate')
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.request(self.prefix + '/settings', {'videoBitrate': 8000000})
        self.assertEqual(error.exception.code, 401); error.exception.close()
        self.request(self.prefix + '/settings', {'videoBitrate': 8000000}, True)
        self.stop(); self.start()
        self.assertEqual(self.get(self.prefix + '/settings')['fields'][0]['value'], 8000000)
        art = self.request(self.prefix + '/art/vid1', binary=True)
        self.assertTrue(art.startswith(b'\xff\xd8\xff'))
        self.request(self.prefix + '/actions/auth.logout', {}, True)
        self.assertFalse(self.get(self.prefix + '/status')['authenticated'])
        code = self.request(self.prefix + '/actions/auth.start', {}, True)
        self.assertEqual(code['code'], 'MOCKQC')
        self.assertEqual(code['pollAction'], 'auth.poll')
        self.approve.touch()
        response = self.request(self.prefix + '/actions/auth.poll', {}, True)
        self.assertTrue(response['authenticated'])

    def test_play_pause_seek_and_live_proxy_stop(self):
        state = self.get(self.prefix + '/play', {'itemId': 'vid1'})
        self.assertTrue(state['playing'])
        self.assertTrue(state['transport']['seek'])
        self.get('/api/playback/pause', {})
        self.assertTrue(self.get('/api/state')['paused'])
        self.get('/api/playback/resume', {})
        state = self.get('/api/playback/seek', {'position': 123})
        self.assertEqual(state['generation'], 2)
        self.assertEqual(state['elapsed'], 123)
        status = self.get(self.prefix + '/status')
        token = status['playback']['session']
        stream = urllib.request.urlopen(self.url + '/module-stream/jellyfin/' + token, timeout=5)
        self.addCleanup(stream.close)
        self.assertEqual(stream.read(188)[0], 0x47)
        before = time.monotonic()
        self.assertTrue(self.get('/api/state')['playing'])
        self.assertTrue(self.get('/api/system/status')['ready'])
        self.get('/api/playback/stop', {})
        self.assertLess(time.monotonic() - before, 3)
        self.assertFalse(self.get('/api/state')['playing'])
        self.assertFalse(self.get(self.prefix + '/status')['playing'])
        with self.assertRaises(urllib.error.HTTPError) as error:
            urllib.request.urlopen(self.url + '/module-stream/jellyfin/' + token)
        self.assertEqual(error.exception.code, 404); error.exception.close()
        self.assertIn('StartTimeTicks=1230000000', self.requests.read_text())

    def test_legacy_adapters(self):
        self.assertTrue(self.get('/api/capabilities')['jellyfin'])
        self.assertFalse(self.get('/api/capabilities')['iptv'])
        self.assertEqual(self.get('/api/libraries')['libraries'][0]['name'], 'Movies')
        self.assertEqual(self.get('/api/items')['items'][0]['id'], 'vid1')
        self.assertIn('items', self.get('/api/jellyfin/resume'))
        self.get('/api/settings', {'jellyfinVideoBitrate': 16000000})
        self.assertEqual(self.get('/api/settings')['settings']['jellyfinVideoBitrate'], 16000000)
        self.assertTrue(self.get('/api/auth/status')['authenticated'])
        self.assertTrue(self.get('/api/play', {'itemId': 'vid1'})['playing'])
        self.assertEqual(self.get('/api/seek', {'seconds': 200})['elapsed'], 200)
        self.assertEqual(self.get('/api/seek', {'delta': -30})['elapsed'], 170)
        self.get('/api/transport', {'action': 'stop'})
        self.request(self.prefix + '/disable', {}, True)
        self.assertFalse(self.get('/api/capabilities')['jellyfin'])
        with self.assertRaises(urllib.error.HTTPError) as error:
            self.get('/api/libraries')
        self.assertEqual(error.exception.code, 409); error.exception.close()

    def test_long_pause_replaces_session(self):
        self.get(self.prefix + '/play', {'itemId': 'vid1', 'startSeconds': 100})
        self.get('/api/playback/pause', {})
        old = self.get(self.prefix + '/status')['playback']['session']
        time.sleep(21)
        state = self.get('/api/playback/resume', {})
        self.assertEqual(state['generation'], 2)
        self.assertLess(state['elapsed'], 105)
        self.assertGreaterEqual(state['elapsed'], 100)
        self.assertNotEqual(self.get(self.prefix + '/status')['playback']['session'], old)
        self.get('/api/playback/stop', {})

    def test_core_shuts_down_with_stream_open(self):
        self.get(self.prefix + '/play', {'itemId': 'vid1'})
        token = self.get(self.prefix + '/status')['playback']['session']
        stream = urllib.request.urlopen(self.url + '/module-stream/jellyfin/' + token, timeout=5)
        self.addCleanup(stream.close)
        stream.read(188)
        self.stop()
        self.assertEqual(self.daemon.returncode, 0)

if __name__ == '__main__':
    unittest.main()
