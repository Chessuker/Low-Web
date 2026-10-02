# Tabs put to sleep (--sleep-tabs-after) and reading positions: a background tab's page is
# unloaded and comes back, when the tab is shown again, looking exactly as it did; back and
# reload return to the same place too. Pages that can't simply be loaded again stay awake.
# Runs bin/lowweb.exe in script mode against a local server (no internet needed).
import functools, http.server, os, shutil, subprocess, sys, tempfile, threading
from PIL import Image, ImageChops

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'bin', 'lowweb.exe')
work = tempfile.mkdtemp(prefix='lw-sleep-')
site = os.path.join(work, 'site')
os.makedirs(site)
# long pages, every part different, so a wrong scroll position shows
for name, color in (('a', '#036'), ('b', '#630')):
    parts = ''.join('<h2 style="color:%s">%s section %d</h2><p>%s</p>' % (color, name.upper(), i, ' '.join(
        '%s%d-%d' % (name, i, k) for k in range(60))) for i in range(80))
    with open(os.path.join(site, name + '.html'), 'w', encoding='utf-8') as f:
        f.write('<!doctype html><html><head><title>page %s</title></head><body>%s</body></html>' % (name, parts))
with open(os.path.join(site, 'form.html'), 'w', encoding='utf-8') as f:
    f.write('<!doctype html><title>form</title><form><input name=q></form>')


class H(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass


srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(H, directory=site))
threading.Thread(target=srv.serve_forever, daemon=True).start()
U = 'http://127.0.0.1:%d/' % srv.server_address[1]
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + name + ('' if ok else '\n      ' + detail))
    fails += not ok


def browse(url, script, *flags):
    log = os.path.join(work, 'log.txt')
    subprocess.run([EXE, url, '--size', '900x640', '--no-cache', '--log', log, '--script', script,
                    '--screenshot', os.path.join(work, 'end.bmp')] + list(flags), timeout=120)
    return open(log, encoding='utf-8', errors='replace').read()


def same(a, b):  # the page area (below tab strip and toolbar) of two screenshots
    x, y = (Image.open(os.path.join(work, n)).convert('RGB') for n in (a, b))
    return ImageChops.difference(x.crop((0, 80, x.width, x.height)), y.crop((0, 80, y.width, y.height))).getbbox() is None


try:
    def shot(n):
        return 'shot ' + os.path.join(work, n)

    log = browse(U + 'a.html', '; '.join([
        'wait 1500', 'key 34', 'key 34', 'key 34', 'key 34', 'wait 600', shot('before.bmp'),
        'key 84 ctrl', 'type ' + U + 'b.html', 'wait 3000',  # tab a sleeps meanwhile
        'mem', 'key 9 ctrl', 'wait 2500', shot('woken.bmp'), 'mem']), '--sleep-tabs-after', '1')
    check('a background tab falls asleep', 'tab asleep: ' + U + 'a.html' in log, log)
    check('... its page is unloaded', '(1 asleep)' in log, log)
    check('... and wakes up when shown', 'waking tab: ' + U + 'a.html' in log, log)
    check('... looking exactly as before, at the same place', same('before.bmp', 'woken.bmp'))

    browse(U + 'a.html', '; '.join([
        'wait 1500', 'key 34', 'key 34', 'key 34', 'wait 600', shot('before.bmp'),
        'nav ' + U + 'b.html', 'wait 1500', 'back', 'wait 2000', shot('back.bmp'),
        'key 116', 'wait 2000', shot('reload.bmp')]))
    check('back returns to the same place', same('before.bmp', 'back.bmp'))
    check('reload keeps the place', same('before.bmp', 'reload.bmp'))

    log = browse(U + 'form.html', '; '.join([
        'wait 1500', 'char x', 'key 84 ctrl', 'type ' + U + 'b.html', 'wait 2500', 'mem']), '--sleep-tabs-after', '1')
    check('a page the user typed into stays awake', '(0 asleep)' in log and 'tab asleep' not in log, log)

    www = 'file:///' + os.path.join(ROOT, 'sites', 'www').replace('\\', '/') + '/'
    log = browse(www, '; '.join(['wait 1500', 'key 84 ctrl', 'type ' + U + 'b.html', 'wait 2500', 'mem']),
                 '--sleep-tabs-after', '1')
    check('a Low-web page (its own state) stays awake', '(0 asleep)' in log and 'tab asleep' not in log, log)
finally:
    srv.shutdown()
    shutil.rmtree(work, ignore_errors=True)
print('ALL OK' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
