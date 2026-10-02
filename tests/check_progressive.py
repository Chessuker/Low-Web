# Pages shown while they download slowly (tests/slow_server.py, here on a free port): a page
# whose content is one big table shows rows before the table is complete; a page without
# <main> shows its first screen before the download ends; a <main> that comes late still
# ends in reader view. And each looks in the end exactly as when loaded at once.
import os, re, shutil, socket, subprocess, sys, tempfile, time
from PIL import Image, ImageChops

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'bin', 'lowweb.exe')
work = tempfile.mkdtemp(prefix='lw-prog-')
rows = ''.join('<tr><td>%d</td><td>Row %d name</td><td>ข้อมูล %d</td><td>%d.%02d</td></tr>' % (i, i, i, i * 7, i % 100) for i in range(700))
pages = {
    'table.html': '<!doctype html><html><head><meta charset=utf-8><title>t</title></head><body><h1>Big table</h1>'
                  '<table border=1>' + rows + '</table><p>after the table</p></body></html>',
    'main.html': '<!doctype html><html><head><meta charset=utf-8><title>m</title></head><body><nav>' +
                 ''.join('<a href="/p%d">navigation link %d</a> ' % (i, i) for i in range(1500)) +
                 '</nav><main><h1>The article</h1><p>' + 'Words of the article itself. ' * 80 + '</p></main></body></html>',
}
for name, html in pages.items():
    with open(os.path.join(work, name), 'w', encoding='utf-8') as f:
        f.write(html)
s = socket.socket()
s.bind(('127.0.0.1', 0))
port = s.getsockname()[1]
s.close()
server = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tests', 'slow_server.py'), str(port), work],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + name + ('' if ok else '\n      ' + str(detail)))
    fails += not ok


def run(url, script, end):
    log = os.path.join(work, 'log.txt')
    subprocess.run([EXE, url, '--size', '800x500', '--no-cache', '--log', log, '--script', script,
                    '--screenshot', os.path.join(work, end)], timeout=120)
    return open(log, encoding='utf-8', errors='replace').read()


def area(name):  # below the tab strip and toolbar
    im = Image.open(os.path.join(work, name)).convert('RGB')
    return im.crop((0, 80, im.width, im.height))


try:
    time.sleep(0.5)
    slow = 'http://127.0.0.1:%d/%%s?chunk=2000&delay=0.05&seed=1' % port  # ~20 KB/s
    for name in pages:  # how each looks loaded at once
        run('file:///' + work.replace('\\', '/') + '/' + name, 'wait 800', 'whole-' + name + '.bmp')

    log = run(slow % 'table.html', 'async; wait 1600; shot ' + os.path.join(work, 'early.bmp') + '; sync; wait 500', 'table-end.bmp')
    first = re.search(r'\[page \+(\d+) ms\] viewer: first screen after', log)
    done = re.search(r'\[page \+(\d+) ms\] viewer: parse ', log)
    early = area('early.bmp').convert('L').crop((0, 90, 400, 420))
    dark = sum(early.histogram()[:200])  # table borders and text
    check('a page in one big table: the first screen comes before the download ends',
          first and done and int(first.group(1)) < int(done.group(1)) - 500, (first and first.group(0), done and done.group(0)))
    check('... with rows of the table that is still arriving', dark > 2000, dark)
    check('... and in the end it looks as when loaded at once',
          ImageChops.difference(area('table-end.bmp'), area('whole-table.html.bmp')).getbbox() is None)

    log = run(slow % 'main.html', 'async; wait 1200; shot ' + os.path.join(work, 'nav.bmp') + '; sync; wait 500', 'main-end.bmp')
    check('a <main> that arrives late: the full page is shown meanwhile',
          ImageChops.difference(area('nav.bmp'), area('whole-main.html.bmp')).getbbox() is not None)
    check('... and then reader view, as when loaded at once',
          ImageChops.difference(area('main-end.bmp'), area('whole-main.html.bmp')).getbbox() is None)
finally:
    server.kill()
    shutil.rmtree(work, ignore_errors=True)
print('ALL OK' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
