# HTTP cache and keep-alive: runs a small local server whose answers carry the headers under
# test, fetches through bin/fetchtest.exe (the browser's network code), and checks what went
# over the wire: which requests reached the server, on how many connections, and whether
# the answers came from the cache.
import http.server, os, shutil, socketserver, subprocess, sys, threading

hits = []         # (path, If-None-Match, If-Modified-Since) for every request that arrived
connections = []  # one entry per TCP connection
version = {'etag': 1}


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def setup(self):
        super().setup()
        connections.append(1)
        self.die_next = False

    def send(self, status, body=b'', headers=()):
        self.send_response(status)
        chunked = ('chunked', '1') in headers
        for k, v in headers:
            if k != 'chunked':
                self.send_header(k, v)
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked')
        elif status != 304:
            self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        if chunked:
            for i in range(0, len(body), 7):
                piece = body[i:i + 7]
                self.wfile.write(b'%x\r\n' % len(piece) + piece + b'\r\n')
            self.wfile.write(b'0\r\nX-Trailer: yes\r\n\r\n')
        elif status != 304:
            self.wfile.write(body)

    def do_GET(self):
        if self.die_next:  # a kept-alive connection the server has given up on
            self.close_connection = True
            return
        hits.append((self.path, self.headers.get('If-None-Match'), self.headers.get('If-Modified-Since')))
        p = self.path
        if p == '/max-age':
            self.send(200, b'fresh for a minute', [('Cache-Control', 'max-age=60')])
        elif p == '/etag':
            tag = '"v%d"' % version['etag']
            if self.headers.get('If-None-Match') == tag:
                self.send(304, headers=[('ETag', tag), ('Cache-Control', 'no-cache')])
            else:
                self.send(200, b'version %d' % version['etag'], [('ETag', tag), ('Cache-Control', 'no-cache')])
        elif p == '/lastmod':
            lm = 'Tue, 01 Sep 2026 10:00:00 GMT'
            if self.headers.get('If-Modified-Since') == lm:
                self.send(304, headers=[('Cache-Control', 'max-age=0')])
            else:
                self.send(200, b'old news', [('Last-Modified', lm), ('Cache-Control', 'max-age=0')])
        elif p == '/no-store':
            self.send(200, b'secret', [('Cache-Control', 'no-store'), ('ETag', '"x"')])
        elif p == '/expires':
            self.send(200, b'expires later', [('Date', 'Tue, 29 Sep 2026 10:00:00 GMT'),
                                              ('Expires', 'Tue, 29 Sep 2026 11:00:00 GMT')])
        elif p == '/chunked':
            self.send(200, b'chunked body, then a trailer', [('chunked', '1'), ('Cache-Control', 'no-store')])
        elif p == '/close':
            self.close_connection = True
            self.send(200, b'bye', [('Connection', 'close')])
        elif p == '/then-die':
            self.send(200, b'ok', [])
            self.die_next = True
        elif p == '/redirect':
            self.send(301, b'', [('Location', '/max-age'), ('Cache-Control', 'max-age=60')])
        else:
            self.send(404, b'no', [])

    def log_message(self, *a):
        pass


class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


server = S(('127.0.0.1', 0), H)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()
base = f'http://127.0.0.1:{port}'
cache_dir = os.path.abspath('build/cachetest')
shutil.rmtree(cache_dir, ignore_errors=True)


def fetch(*paths, extra=()):
    """Fetches the paths in one process (one pool, one cache); returns the [net] lines."""
    r = subprocess.run([os.path.abspath('bin/fetchtest.exe'), '--cache', cache_dir, '--exact', *extra,
                        *[base + p for p in paths]], capture_output=True, text=True, timeout=60)
    return [l.strip() for l in r.stdout.splitlines() if l.strip().startswith('[net]')]


failed = 0


def check(what, ok, detail=''):
    global failed
    print(f'{"ok  " if ok else "FAIL"} {what}' + (f'  ({detail})' if detail and not ok else ''))
    failed += not ok


def reset():
    hits.clear()
    connections.clear()


# keep-alive: several requests, one connection
reset()
log = fetch('/max-age', '/lastmod', '/expires', '/chunked', '/no-store')
check('five requests share one connection (content-length, chunked with a trailer)', len(connections) == 1, f'{len(connections)} connections')
check('the log says so', sum('reused connection' in l for l in log) == 4, log)

# a server that closes: next request opens a new connection
reset()
fetch('/close', '/max-age?x')
check('"Connection: close" is honoured', len(connections) == 2, f'{len(connections)} connections')

# a kept connection that the server drops: the request is sent again on a new one
reset()
log = fetch('/then-die', '/etag')
check('a dead kept-alive connection is retried on a new one', [h[0] for h in hits] == ['/then-die', '/etag'] and len(connections) == 2,
      f'{hits} {len(connections)} connections')

# freshness: max-age and Expires are served without asking
reset()
log = fetch('/max-age', '/expires', '/redirect')
check('max-age: served from the cache', not any(h[0] == '/max-age' for h in hits), hits)
check('Expires: served from the cache', not any(h[0] == '/expires' for h in hits), hits)
reset()
fetch('/redirect')
check('a cached 301 redirect leads to the cached page, with no request at all', hits == [], hits)

# validation: ETag and Last-Modified
reset()
log = fetch('/etag', '/lastmod')
check('ETag: asks with If-None-Match, 304 served from the cache', ('/etag', '"v1"', None) in hits and 'not modified' in ' '.join(log), f'{hits} {log}')
check('Last-Modified: asks with If-Modified-Since', any(h[0] == '/lastmod' and h[2] for h in hits), hits)

# a changed resource replaces the stored one
version['etag'] = 2
reset()
out = subprocess.run([os.path.abspath('bin/fetchtest.exe'), '--cache', cache_dir, '--exact', base + '/etag'], capture_output=True, text=True).stdout
check('a changed ETag gives the new body', 'version 2' in out and 'from the cache' not in out, out)
reset()
out = subprocess.run([os.path.abspath('bin/fetchtest.exe'), '--cache', cache_dir, '--exact', base + '/etag'], capture_output=True, text=True).stdout
check('... and the new version is what is stored', hits and hits[0][1] == '"v2"' and 'from the cache' in out, f'{hits}')

# no-store is never stored
reset()
fetch('/no-store')
check('no-store: always fetched, never validated', hits == [('/no-store', None, None)], hits)

# reload modes
reset()
fetch('/max-age', extra=('--revalidate',))
check('reload: even a fresh copy is checked with the server', any(h[0] == '/max-age' for h in hits), hits)
reset()
fetch('/etag', extra=('--reload',))
check('hard reload: no conditional request', hits == [('/etag', None, None)], hits)

server.shutdown()
shutil.rmtree(cache_dir, ignore_errors=True)
print('ALL OK' if not failed else f'{failed} FAILED')
sys.exit(1 if failed else 0)
