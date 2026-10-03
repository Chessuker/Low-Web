# The right-click menu: the viewer's items for what is under the mouse (a link, an image,
# the selection), the browser's own below them, and what picking them does (copy a link's
# address, save an image, copy the selection). Needs no internet (and Pillow).
import os, shutil, subprocess, sys, tempfile
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'bin', 'lowweb.exe')
work = tempfile.mkdtemp(prefix='lw-menu-')
fails = 0

PAGE = ('<!doctype html><title>Menu</title>'
        '<p><a href="next.html">a link to the next page</a></p>'
        '<p><img src="pic.png" width="120" height="60"></p>'
        '<p>Plain words to select</p>')


def check(name, ok, detail=''):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + name + ('' if ok else '\n      ' + str(detail)))
    fails += not ok


def run(script):
    log = os.path.join(work, 'log.txt')
    saves = os.path.join(work, 'saved')
    os.makedirs(saves, exist_ok=True)
    subprocess.run([EXE, base + 'page.html', '--size', '800x500', '--log', log, '--save-dir', saves, '--script', script], timeout=120)
    lines = [l.rstrip('\n') for l in open(log, encoding='utf-8', errors='replace')]
    menus = [[i.strip() for i in l.split('|')[1:]] for l in lines if l.startswith('[menu]')]
    clips = [l[len('[clipboard] '):] for l in lines if l.startswith('[clipboard]')]
    return menus, clips, lines


try:
    with open(os.path.join(work, 'page.html'), 'w', encoding='utf-8') as f:
        f.write(PAGE)
    with open(os.path.join(work, 'next.html'), 'w', encoding='utf-8') as f:
        f.write('<!doctype html><title>Next</title><p>The next page')
    Image.new('RGB', (120, 60), (200, 40, 40)).save(os.path.join(work, 'pic.png'))
    base = 'file:///' + work.replace('\\', '/') + '/'
    # (in the view: the link at y ~44, the image at ~69..129, the plain words at ~157)
    menus, clips, lines = run('wait 600; rclick 100 44; menupick Copy link address; rclick 60 100; menupick Save image as...; wait 800; '
                              'rclick 60 157; drag 22 157 150 157; rclick 60 157; menupick Copy')
    m = menus + [[]] * 4
    check('on a link: open it in a new tab, copy its address', m[0][:2] == ['Open link in new tab', 'Copy link address'], m[0])
    check('... and the browser\'s own items after the page\'s (Back greyed: nothing to go back to)',
          m[0][-3:] == ['~Back', '~Forward', 'Reload'] and '-' in m[0], m[0])
    check('"Copy link address" copies the whole address', clips[:1] == [base + 'next.html'], clips)
    check('on an image: open, save, copy its address', m[1][:3] == ['Open image in new tab', 'Save image as...', 'Copy image address'], m[1])
    saved = os.path.join(work, 'saved', 'pic.png')
    same = os.path.exists(saved) and open(saved, 'rb').read() == open(os.path.join(work, 'pic.png'), 'rb').read()
    check('"Save image as..." saves the image\'s file', same, os.listdir(os.path.join(work, 'saved')))
    check('on plain text with nothing selected: Copy greyed out', '~Copy' in m[2] and 'Open link in new tab' not in m[2], m[2])
    check('with a selection: Copy copies it', 'Copy' in m[3] and clips[1:2] and 'Plain words' in clips[1], (m[3], clips))
    check('no script errors', not [l for l in lines if l.startswith('[script]')], [l for l in lines if l.startswith('[script]')])
finally:
    shutil.rmtree(work, ignore_errors=True)
print('ALL OK' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
