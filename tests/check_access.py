# What a page may fetch (net.h: Access): a page from the internet must not reach this
# computer or the local network, nor the user's files; a file page only its own folder.
# Runs a small local server and fetches through bin/fetchtest.exe as different pages would.
import http.server, os, shutil, socketserver, subprocess, sys, tempfile, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FETCH = os.path.join(ROOT, 'bin', 'fetchtest.exe')
hits = []


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def do_GET(self):
        hits.append(self.path)
        if self.path == '/to-file':
            body, status, extra = b'', 302, [('Location', 'file:///C:/Windows/win.ini')]
        elif self.path == '/cached':
            body, status, extra = b'kept for a while', 200, [('Cache-Control', 'max-age=600')]
        else:
            body, status, extra = b'hello from this computer', 200, []
        self.send_response(status)
        for k, v in extra:
            self.send_header(k, v)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


def fetch(*args):
    out = subprocess.run([FETCH, '--exact'] + list(args), capture_output=True, text=True, timeout=60).stdout
    return out


fails = 0


def check(name, ok, out=''):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + name)
    if not ok:
        fails += 1
        print('      ' + out.strip().replace('\n', '\n      '))


def allowed(out):
    return '  200  ' in out


def blocked(out):
    return 'ERROR: blocked:' in out


srv = S(('127.0.0.1', 0), H)
port = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()
local = 'http://127.0.0.1:%d' % port

# ---- address spaces
out = subprocess.run([FETCH, '--zone', '127.0.0.1', '0.0.0.0', '10.1.2.3', '172.16.0.1', '172.32.0.1', '192.168.1.1',
                      '169.254.1.1', '100.64.0.1', '8.8.8.8', '::1', '::', 'fe80::1', 'fd00::1', '::ffff:192.168.0.1',
                      '::ffff:1.1.1.1', '2606:4700::1111'], capture_output=True, text=True).stdout
want = {'127.0.0.1': 'local', '0.0.0.0': 'local', '10.1.2.3': 'private', '172.16.0.1': 'private', '172.32.0.1': 'public',
        '192.168.1.1': 'private', '169.254.1.1': 'private', '100.64.0.1': 'private', '8.8.8.8': 'public', '::1': 'local',
        '::': 'local', 'fe80::1': 'private', 'fd00::1': 'private', '::ffff:192.168.0.1': 'private',
        '::ffff:1.1.1.1': 'public', '2606:4700::1111': 'public'}
got = {l.split('->')[0].strip(): l.split('->')[1].strip() for l in out.splitlines() if '->' in l}
check('address spaces of %d IP addresses' % len(want), got == want, out)

# ---- servers on this computer
out = fetch(local + '/x')
check('the user (no page) reaches this computer', allowed(out) and 'zone=local' in out, out)
out = fetch('--page', local + '/', 'local', local + '/x')
check('a page on this computer reaches this computer', allowed(out), out)
n = len(hits)
out = fetch('--page', 'https://example.com/', 'public', local + '/x')
check('a page from the internet may not reach 127.0.0.1', blocked(out) and len(hits) == n, out)
out = fetch('--page', 'https://example.com/', 'public', 'http://localhost:%d/x' % port)
check('... nor "localhost" (checked on the resolved address)', blocked(out) and len(hits) == n, out)
out = fetch('--page', 'https://example.com/', 'public', 'http://[::1]:%d/x' % port)
check('... nor [::1]', blocked(out) and len(hits) == n, out)
out = fetch('--page', 'http://192.168.1.10/', 'private', local + '/x')
check('a page on the local network may not reach this computer', blocked(out) and len(hits) == n, out)

# ---- redirects are checked hop by hop
out = fetch('--page', local + '/', 'local', local + '/to-file')
check('a redirect to a file:// address is blocked for a web page', blocked(out), out)

# ---- the cache must not hand over what the server would not
cdir = tempfile.mkdtemp(prefix='lw-access-')
try:
    out = fetch('--cache', cdir, local + '/cached')
    check('stored in the cache by the user', allowed(out), out)
    out = fetch('--cache', cdir, local + '/cached')
    check('... and served from it', 'from the cache' in out and 'zone=local' in out, out)
    out = fetch('--cache', cdir, '--page', 'https://example.com/', 'public', local + '/cached')
    check('a page from the internet doesn\'t get it from the cache either', blocked(out), out)
finally:
    shutil.rmtree(cdir, ignore_errors=True)

# ---- files
d = tempfile.mkdtemp(prefix='lw-files-')
try:
    os.makedirs(os.path.join(d, 'site', 'sub'))
    for rel in ['site/index.html', 'site/sub/a.txt', 'secret.txt']:
        with open(os.path.join(d, rel), 'w') as f:
            f.write('content of ' + rel)
    base = 'file:///' + d.replace('\\', '/')
    page = base + '/site/index.html'
    out = fetch('--page', 'https://example.com/', 'public', base + '/secret.txt')
    check('a page from the internet may not open files', blocked(out), out)
    out = fetch('--page', local + '/', 'local', base + '/secret.txt')
    check('... nor may one on this computer', blocked(out), out)
    out = fetch('--page', page, 'local', base + '/site/sub/a.txt')
    check('a file page opens files in its folder and below', allowed(out) and 'zone=local' in out, out)
    out = fetch('--page', page, 'local', base + '/secret.txt')
    check('... but not above it', blocked(out), out)
    out = fetch('--page', page, 'local', base + '/site/../secret.txt')
    check('... not with ".."', blocked(out), out)
    out = fetch('--page', page, 'local', base + '/site/%2e%2e/secret.txt')
    check('... not with "%2e%2e"', blocked(out), out)
    out = fetch('--page', page, 'local', base + '/site/sub%5c..%5c..%5csecret.txt')
    check('... not with encoded backslashes', blocked(out), out)
    out = fetch('--page', page, 'local', base + '/site-other/x.txt')
    check('... not a folder whose name only starts the same', blocked(out), out)
    out = fetch(base + '/secret.txt')
    check('the user opens any file', allowed(out), out)
finally:
    shutil.rmtree(d, ignore_errors=True)

srv.shutdown()
print('all passed' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
