# Cookies: the jar's rules (bin/cookietest.exe: domains, paths, Secure, lifetimes,
# SameSite, third parties, prefixes, saving) and the HTTP side with a local server: a
# cookie set on a redirect is sent to the next page, and a cached answer that varies by
# cookie is not reused once the cookie changes.
import http.server, os, shutil, socketserver, subprocess, sys, threading

work = os.path.abspath('build/cookietest')
shutil.rmtree(work, ignore_errors=True)
os.makedirs(work)
failed = 0

r = subprocess.run([os.path.abspath('bin/cookietest.exe'), os.path.join(work, 'unit.txt')], capture_output=True, text=True)
print(r.stdout.strip())
failed += r.returncode != 0


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def reply(self, status, body=b'', headers=()):
        self.send_response(status)
        for k, v in headers:
            self.send_header(k, v)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        got = (self.headers.get('Cookie') or '(none)').encode()
        if self.path == '/login':
            self.reply(302, b'', [('Location', '/home'), ('Set-Cookie', 'sid=abc; Path=/; HttpOnly'),
                                  ('Set-Cookie', 'theme=dark; Path=/; Max-Age=3600')])
        elif self.path == '/login2':
            self.reply(200, b'ok', [('Set-Cookie', 'sid=xyz; Path=/')])
        elif self.path == '/home':
            self.reply(200, b'home sees ' + got)
        elif self.path == '/vary':
            self.reply(200, b'vary sees ' + got, [('Cache-Control', 'max-age=60'), ('Vary', 'Accept-Encoding, Cookie')])
        else:
            self.reply(404)

    def log_message(self, *a):
        pass


class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


server = S(('127.0.0.1', 0), H)
threading.Thread(target=server.serve_forever, daemon=True).start()
base = f'http://127.0.0.1:{server.server_address[1]}'
out = subprocess.run([os.path.abspath('bin/fetchtest.exe'), '--cookies', os.path.join(work, 'jar.txt'), '--cache', os.path.join(work, 'cache'),
                      '--exact', base + '/login', base + '/vary', base + '/login2', base + '/vary'],
                     capture_output=True, text=True, timeout=60).stdout
bodies = [l.strip().strip('"') for l in out.splitlines() if l.startswith('  "')]


def check(what, ok, detail):
    global failed
    print(f'{"ok  " if ok else "FAIL"} {what}' + ('' if ok else f'   ({detail})'))
    failed += not ok


check('cookies set on a redirect reach the next page', len(bodies) > 0 and bodies[0] == 'home sees sid=abc; theme=dark', bodies)
check('a cached answer that varies by cookie ...', len(bodies) > 1 and bodies[1] == 'vary sees sid=abc; theme=dark', bodies)
check('... is not reused when the cookie changes', len(bodies) > 3 and bodies[3] == 'vary sees sid=xyz; theme=dark', bodies)
jar = open(os.path.join(work, 'jar.txt'), encoding='utf-8').read()
check('only the persistent cookie is in the file', 'theme\tdark' in jar and 'sid' not in jar, jar)

server.shutdown()
shutil.rmtree(work, ignore_errors=True)
print('ALL OK' if not failed else f'{failed} FAILED')
sys.exit(1 if failed else 0)
