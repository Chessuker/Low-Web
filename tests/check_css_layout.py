# CSS layout from stylesheets: flex rows, floats and clear (a float that clears too), grids, a clipped carousel and
# justify-content. Opens a page of coloured boxes in bin/lowweb.exe, takes a screenshot and
# finds each box by its colour. Needs no internet (and Pillow).
import os, shutil, subprocess, sys, tempfile
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'bin', 'lowweb.exe')
work = tempfile.mkdtemp(prefix='lw-css-')
fails = 0

PAGE = '''<!doctype html><html><head><title>css</title><style>
.row{display:flex;gap:10px}
.a{flex:1} .b{flex:2} .c{width:100px}
.fr{float:right;width:200px}
.clr{clear:both} .cr{clear:right}
.g{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
.clip{overflow:hidden} .track{display:flex} .slide{flex-shrink:0;width:500px}
.mid{display:flex;justify-content:center}
</style></head><body>
<div class=row><div class=a style="background:#ff0000">a</div><div class=b style="background:#00c000">b</div><div class=c style="background:#0000ff">c</div></div>
<div class=fr style="background:#ff00ff">float<br>right<br>box</div>
<p>Text beside the float.</p>
<div class=clr style="background:#00ffff">cleared</div>
<div class=g><div style="background:#ffff00">1</div><div style="background:#ff8000">2</div><div style="background:#8000ff">3</div><div style="background:#008080">4</div></div>
<div class=clip><div class=track><div class=slide style="background:#804000">one</div><div class=slide style="background:#004080">two</div></div></div>
<div class=mid><div style="background:#ff0080">centred</div></div>
<div class=fr style="background:#ff6060">tall<br>float<br>one<br>two</div>
<p>Heading</p>
<div class="fr cr" style="background:#60ff60">second</div>
<div style="background:#6060ff">after</div>
</body></html>'''

COLOURS = {'red': (255, 0, 0), 'green': (0, 192, 0), 'blue': (0, 0, 255), 'float': (255, 0, 255), 'cleared': (0, 255, 255),
           'g1': (255, 255, 0), 'g2': (255, 128, 0), 'g3': (128, 0, 255), 'g4': (0, 128, 128),
           'slide1': (128, 64, 0), 'slide2': (0, 64, 128), 'centred': (255, 0, 128),
           'tall': (255, 96, 96), 'second': (96, 255, 96), 'after': (96, 96, 255)}


def check(name, ok, detail=''):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + name + ('' if ok else '\n      ' + detail))
    fails += not ok


def near(a, b, tol=3):
    return abs(a - b) <= tol


try:
    with open(os.path.join(work, 'page.html'), 'w', encoding='utf-8') as f:
        f.write(PAGE)
    shot = os.path.join(work, 'shot.bmp')
    subprocess.run([EXE, 'file:///' + work.replace('\\', '/') + '/page.html', '--size', '800x900',
                    '--script', 'wait 600', '--screenshot', shot], timeout=120)
    im = Image.open(shot).convert('RGB')
    px = im.load()
    want = {v: k for k, v in COLOURS.items()}
    box = {}  # colour name -> [x0, y0, x1, y1] (x1, y1 exclusive)
    for y in range(im.height):
        for x in range(im.width):
            k = want.get(px[x, y])
            if k:
                b = box.setdefault(k, [x, y, x + 1, y + 1])
                b[0] = min(b[0], x); b[1] = min(b[1], y); b[2] = max(b[2], x + 1); b[3] = max(b[3], y + 1)
    found = ', '.join('%s %s' % (k, v) for k, v in sorted(box.items()))
    missing = [k for k in COLOURS if k not in box and k != 'slide2']
    check('every box is drawn', not missing, 'missing: %s; found: %s' % (missing, found))
    if not missing:
        r, g, b = box['red'], box['green'], box['blue']
        w = lambda k: box[k][2] - box[k][0]
        left, right = r[0], b[2]  # the page's content edges
        check('flex row: one line, a fixed 100 px item, the rest shared 1:2, 10 px gaps',
              r[1] == g[1] == b[1] and near(w('blue'), 100) and near(w('green'), 2 * w('red'), 3) and
              near(g[0] - r[2], 10) and near(b[0] - g[2], 10), found)
        f, c = box['float'], box['cleared']
        check('float: right, 200 px wide, below the row', near(f[2], right) and near(w('float'), 200) and f[1] >= r[3], found)
        dark = any(sum(px[x, y]) < 200 for y in range(f[1], min(f[1] + 30, f[3])) for x in range(left, f[0] - 10))
        check('... text flows beside it', dark, found)
        check('clear: the next block starts below the float', c[1] >= f[3] - 8 and near(c[0], left), found)
        g1, g2, g3, g4 = box['g1'], box['g2'], box['g3'], box['g4']
        check('grid: three equal columns, the fourth item on a second row',
              g1[1] == g2[1] == g3[1] and near(w('g1'), w('g2')) and near(w('g2'), w('g3')) and
              near(g4[0], g1[0]) and g4[1] > g1[3] and near(g3[2], right), found)
        check('carousel (overflow: hidden): only the slide that fits is shown',
              near(w('slide1'), 500) and 'slide2' not in box, found)
        m = box['centred']
        check('justify-content: center (a box as wide as its text, in the middle)',
              w('centred') < (right - left) / 4 and near((m[0] + m[2]) / 2, (left + right) / 2, 4), found)
        t, s2, a = box['tall'], box['second'], box['after']
        check('a float with clear goes below the float before it, what follows stays up beside that one',
              s2[1] >= t[3] - 8 and a[1] < s2[1] and a[1] < t[3], found)
finally:
    shutil.rmtree(work, ignore_errors=True)
print('ALL OK' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
