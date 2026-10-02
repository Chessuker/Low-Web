# Serves files slowly, in random-sized pieces, to test showing pages while they download.
#   python tests/slow_server.py [PORT] [FOLDER]      (default 8765, build/slow)
#   http://127.0.0.1:8765/NAME?gzip=1&chunked=1&chunk=8000&delay=0.02&seed=1&noranges=1
# Answers "Range: bytes=N-" with 206 (unless noranges=1) and prints one line per request:
#   GET /NAME from N           (N = 0 without a range)
import gzip, http.server, random, socketserver, sys, time, urllib.parse, os
ROOT = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else 'build/slow')
TYPES = {'.css': 'text/css', '.wasm': 'application/wasm', '.mp4': 'video/mp4', '.webm': 'video/webm', '.mp3': 'audio/mpeg', '.m4a': 'audio/mp4',
         '.png': 'image/png', '.jpg': 'image/jpeg'}
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def do_GET(self):
        u = urllib.parse.urlparse(self.path); q = dict(urllib.parse.parse_qsl(u.query))
        path = os.path.join(ROOT, u.path.lstrip('/'))
        if not os.path.isfile(path):
            self.send_response(404); self.send_header('Content-Length', '0'); self.end_headers(); return
        body = open(path, 'rb').read()
        gz = q.get('gzip') == '1'
        if gz: body = gzip.compress(body, 6)
        rnd = random.Random(int(q.get('seed', '1')))
        chunk = int(q.get('chunk', '8000')); delay = float(q.get('delay', '0.02'))
        rng = self.headers.get('Range', '')
        start = 0
        if rng.startswith('bytes=') and rng.endswith('-') and q.get('noranges') != '1' and not gz:
            start = int(rng[6:-1])
        print('GET %s from %d' % (u.path, start), flush=True)
        total = len(body)
        if start:
            if start >= total:
                self.send_response(416); self.send_header('Content-Range', 'bytes */%d' % total)
                self.send_header('Content-Length', '0'); self.end_headers(); return
            self.send_response(206); self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, total - 1, total))
            body = body[start:]
        else:
            self.send_response(200)
        if q.get('noranges') != '1': self.send_header('Accept-Ranges', 'bytes')
        ext = os.path.splitext(path)[1].lower()
        self.send_header('Content-Type', TYPES.get(ext, 'text/html; charset=utf-8'))
        if gz: self.send_header('Content-Encoding', 'gzip')
        chunked = q.get('chunked') == '1'
        if chunked: self.send_header('Transfer-Encoding', 'chunked')
        else: self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        i = 0
        try:
            while i < len(body):
                n = rnd.randint(1, chunk); piece = body[i:i + n]; i += n
                if chunked: self.wfile.write(b'%x\r\n' % len(piece) + piece + b'\r\n')
                else: self.wfile.write(piece)
                self.wfile.flush(); time.sleep(delay)
            if chunked: self.wfile.write(b'0\r\n\r\n')
        except (ConnectionError, OSError):
            print('GET %s stopped at %d' % (u.path, start + i), flush=True)  # the client went away
    def log_message(self, *a): pass
class S(socketserver.ThreadingMixIn, http.server.HTTPServer): daemon_threads = True
S(('127.0.0.1', int(sys.argv[1]) if len(sys.argv) > 1 else 8765), H).serve_forever()
