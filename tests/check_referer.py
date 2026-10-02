# What the browser says about itself and where it comes from: User-Agent (Chrome's form, with
# Low-web at the end) and Referer as browsers send it by default (strict-origin-when-cross-
# origin): the whole address (no #fragment) to the same origin, only the origin to another
# one, nothing for an address the user typed. Two local servers on free ports are the two
# origins; they write down the headers of every request. Needs no internet.
import http.server, os, shutil, socket, subprocess, sys, tempfile, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'bin', 'lowweb.exe')
work = tempfile.mkdtemp(prefix='lw-referer-')
seen = []  # (port, path, user-agent, referer)
PNG = bytes.fromhex('89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d4944415478da63f8cfc0f01f0005000201'
                    'a5f5f1e50000000049454e44ae426082')


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


A, B = free_port(), free_port()
PAGES = {
    '/page.html': '<!doctype html><title>a</title><p><img src="same.png" width=20 height=20> '
                  '<img src="http://127.0.0.1:%d/other.png" width=20 height=20></p><p><a href="http://127.0.0.1:%d/next.html">to the other '
                  'site</a></p><p><a href="/near.html">to this site</a></p>' % (B, B),
    '/next.html': '<!doctype html><title>b</title><p>The other site.</p>',
    '/near.html': '<!doctype html><title>c</title><p>This site.</p>',
}


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def do_GET(self):
        path = self.path.split('?')[0]
        seen.append((self.server.server_port, self.path, self.headers.get('User-Agent', ''), self.headers.get('Referer')))
        body, ctype = (PNG, 'image/png') if path.endswith('.png') else (PAGES.get(path, '').encode(), 'text/html')
        self.send_response(200 if path.endswith('.png') or path in PAGES else 404)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


servers = [http.server.ThreadingHTTPServer(('127.0.0.1', p), H) for p in (A, B)]
for s in servers:
    threading.Thread(target=s.serve_forever, daemon=True).start()
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + name + ('' if ok else '\n      ' + str(detail)))
    fails += not ok


def run(url, script):
    subprocess.run([EXE, url, '--size', '800x500', '--no-cache', '--script', script], timeout=120)


def find(port, path):
    return [s for s in seen if s[0] == port and s[1].split('?')[0] == path]


try:
    page = 'http://127.0.0.1:%d/page.html?q=1#part' % A
    # (the links: the second line of the page at y ~70 in the view, the third at ~107)
    run(page, 'wait 800; click 50 70; wait 800')
    run(page, 'wait 800; click 50 107; wait 800')
    p, same, other, nxt, near = find(A, '/page.html'), find(A, '/same.png'), find(B, '/other.png'), find(B, '/next.html'), find(A, '/near.html')
    ua = p[0][2] if p else ''
    check('User-Agent: Chrome\'s form, and Low-web', ua.startswith('Mozilla/5.0 (Windows NT 10.0; Win64; x64)') and 'Chrome/' in ua and
          ua.endswith('Low-web/0.1'), ua)
    check('a typed address: no Referer', p and all(x[3] is None for x in p), p)
    full = 'http://127.0.0.1:%d/page.html?q=1' % A
    check('a picture from the same origin: the whole address (without #part)', same and same[0][3] == full, same)
    check('a picture from another origin: only the origin', other and other[0][3] == 'http://127.0.0.1:%d/' % A, other)
    check('a link to another origin: only the origin', nxt and nxt[0][3] == 'http://127.0.0.1:%d/' % A, nxt)
    check('a link within the origin: the whole address', near and near[0][3] == full, near)
finally:
    for s in servers:
        s.shutdown()
    shutil.rmtree(work, ignore_errors=True)
print('ALL OK' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
