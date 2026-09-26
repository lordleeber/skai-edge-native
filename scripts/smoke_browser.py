"""Real Firefox UI/media checks through the W3C WebDriver HTTP protocol."""
import base64
import io
import json
import pathlib
import time
import urllib.request
import zipfile


class BrowserProbe:
    def __init__(self, driver_url, timeout, openh264_dir=None, openh264_abi=None):
        self.url = driver_url.rstrip('/')
        self.timeout = timeout
        self.session = None
        self.openh264_dir = openh264_dir
        self.openh264_abi = openh264_abi

    def command(self, method, path, body=None):
        payload = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(self.url + path, data=payload, method=method,
                                         headers={'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=self.timeout + 5) as response:
            result = json.load(response)['value']
        if isinstance(result, dict) and 'error' in result:
            raise RuntimeError('WebDriver command failed: ' + result['error'])
        return result

    def open(self, base_url):
        options = {'args': ['-headless'], 'prefs': {'media.autoplay.default': 0}}
        if self.openh264_dir:
            plugin = pathlib.Path(self.openh264_dir)
            buffer = io.BytesIO()
            with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as archive:
                for name in ['libgmpopenh264.so', 'gmpopenh264.info']:
                    archive.write(plugin / name, 'gmp-gmpopenh264/' + plugin.name + '/' + name)
            options['profile'] = base64.b64encode(buffer.getvalue()).decode()
            options['prefs'].update({'media.gmp-gmpopenh264.version': plugin.name,
                                     'media.gmp-gmpopenh264.lastUpdate': int(time.time()),
                                     'media.gmp-gmpopenh264.enabled': True})
            if self.openh264_abi:
                options['prefs']['media.gmp-gmpopenh264.abi'] = self.openh264_abi
        value = self.command('POST', '/session', {'capabilities': {'alwaysMatch': {
            'browserName': 'firefox', 'moz:firefoxOptions': options}}})
        self.session = '/session/' + value['sessionId']
        self.command('POST', self.session + '/timeouts', {
            'script': int(self.timeout * 1000), 'pageLoad': int(self.timeout * 1000)})
        self.command('POST', self.session + '/url', {'url': base_url})

    def media(self):
        return self.command('POST', self.session + '/execute/sync', {'args': [], 'script': """
            const video = document.getElementById('live-video');
            if (video?.requestVideoFrameCallback && !window.__smokeFrameObserver) {
                window.__smokeFrameObserver = true;
                window.__smokeFrames = 0;
                const observe = () => {
                    window.__smokeFrames++;
                    video.requestVideoFrameCallback(observe);
                };
                video.requestVideoFrameCallback(observe);
            }
            return {title: document.title, width: video?.videoWidth || 0,
                    height: video?.videoHeight || 0, time: video?.currentTime || 0,
                    frames: Math.max(video?.getVideoPlaybackQuality?.().totalVideoFrames || 0,
                                     window.__smokeFrames || 0)};
        """})

    def verify(self):
        deadline = time.monotonic() + self.timeout
        first = None
        while time.monotonic() < deadline:
            current = self.media()
            if (current['title'] == 'SKAI Edge Console' and current['width'] > 0 and
                    current['height'] > 0 and current['frames'] >= 2):
                if first and current['time'] > first['time'] + 0.05 and current['frames'] > first['frames']:
                    break
                first = current
            time.sleep(0.1)
        else:
            raise RuntimeError('browser UI did not render advancing decoded video frames')
        answer = self.command('POST', self.session + '/execute/async', {'args': [], 'script': """
            const done = arguments[arguments.length - 1];
            (async () => {
                const pc = new RTCPeerConnection();
                let location;
                try {
                    pc.addTransceiver('video', {direction: 'recvonly'});
                    await pc.setLocalDescription(await pc.createOffer());
                    await new Promise((resolve, reject) => {
                        if (pc.iceGatheringState === 'complete') return resolve();
                        const timer = setTimeout(() => reject(Error('ICE timeout')), 5000);
                        pc.onicegatheringstatechange = () => {
                            if (pc.iceGatheringState === 'complete') {
                                clearTimeout(timer); resolve();
                            }
                        };
                    });
                    const response = await fetch('/api/v1/webrtc/whep', {
                        method: 'POST', headers: {'Content-Type': 'application/sdp'},
                        body: pc.localDescription.sdp, signal: AbortSignal.timeout(5000)});
                    location = response.headers.get('Location');
                    const sdp = await response.text();
                    if (response.status !== 201 || !location ||
                        !response.headers.get('Content-Type')?.startsWith('application/sdp') ||
                        !sdp.startsWith('v=0') || !sdp.includes('m=video')) throw Error('SDP');
                    await pc.setRemoteDescription({type: 'answer', sdp});
                    const removed = await fetch(location, {method: 'DELETE',
                        signal: AbortSignal.timeout(5000)});
                    location = null;
                    done({valid: true, status: response.status, deleted: removed.status === 204});
                } catch { done({valid: false}); }
                finally {
                    pc.close();
                    if (location) fetch(location, {method: 'DELETE'}).catch(() => {});
                }
            })();
        """})
        if not answer.get('valid') or answer.get('status') != 201 or not answer.get('deleted'):
            raise RuntimeError('WHEP did not return a browser-accepted SDP and deletable session')
        return {'media': current, 'whep': answer}

    def close(self):
        if self.session:
            try:
                self.command('DELETE', self.session)
            finally:
                self.session = None
