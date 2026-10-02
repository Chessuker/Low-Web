# The back/forward history: a link the user clicks adds an entry; a page that sends the tab on
# by itself (<meta refresh>, as the redirect pages of search engines do) takes its own place,
# so Back from where it led goes to the page before it, not to it (which would send the user
# forward again). Needs no internet.
import os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'bin', 'lowweb.exe')
work = tempfile.mkdtemp(prefix='lw-history-')
pages = {
    'search.html': '<!doctype html><title>Search</title><p><a href="redirect.html">a result through a redirect page</a></p>'
                   '<p><a href="plain.html">a plain link</a></p>',
    'redirect.html': '<!doctype html><title>Redirect</title><meta http-equiv="refresh" content="0; url=target.html"><p>Redirecting...</p>',
    'target.html': '<!doctype html><title>Target</title><h1>The target</h1>',
    'plain.html': '<!doctype html><title>Plain</title><h1>A plain page</h1>',
}
for name, html in pages.items():
    with open(os.path.join(work, name), 'w', encoding='utf-8') as f:
        f.write(html)
base = 'file:///' + work.replace('\\', '/') + '/'
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + name + ('' if ok else '\n      ' + str(detail)))
    fails += not ok


def histories(script):
    log = os.path.join(work, 'log.txt')
    subprocess.run([EXE, base + 'search.html', '--size', '800x500', '--log', log, '--script', script], timeout=120)
    out = []
    for line in open(log, encoding='utf-8', errors='replace'):
        if line.startswith('[history]'):
            entries = line.split()[1:]
            out.append(([e.lstrip('*').rsplit('/', 1)[-1] for e in entries], [i for i, e in enumerate(entries) if e.startswith('*')][0]))
    return out


try:
    # (the links: the first at y ~45 in the view, the second at ~82)
    h = histories('wait 600; click 60 45; wait 1500; history; back; wait 1000; history')
    check('a redirect page takes no place in the history', h[:1] == [(['search.html', 'target.html'], 1)], h)
    check('... so Back goes to the page before it', h[1:2] == [(['search.html', 'target.html'], 0)], h)
    h = histories('wait 600; click 40 82; wait 1000; history; back; wait 800; history')
    check('a link the user clicks adds an entry; Back goes back to it', h == [(['search.html', 'plain.html'], 1),
                                                                         (['search.html', 'plain.html'], 0)], h)
finally:
    shutil.rmtree(work, ignore_errors=True)
print('ALL OK' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
