# CSS layout from stylesheets: flex rows, floats and clear (a float that clears too, a float
# in the middle of a line), grids, media queries in the range syntax, hidden table cells,
# GitHub's "visually hidden" headings, an empty :is() (it matches nothing: GitHub has one),
# margin/padding/background as advice (a dark background under our dark text is left out, a
# width narrows and centres a column but never to a sliver), flex order and align-self, a clipped carousel and
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
.more{float:right}
.wide{display:none} .narrow{display:none}
@media (width >= 600px){.wide{display:block}}
@media (width<=calc(30rem - .02px)){.narrow{display:block}}
@media (300px < width <= 700px){.wide{display:none}}
.cell-small{display:none}
:is(){display:none}
.card{background:#e8f0ff;margin:0 40px;padding:20px}
.dark{background:#101820}
.column{max-width:300px;margin:12px auto;background:#ffe0c0}
.tiny{width:50px;background:#c0ffe0}
.ord{display:flex;align-items:flex-start} .o1{order:2;background:#f0c0f0} .o2{order:1;background:#c0f0c0}
.tallbox{height:60px;background:#202080} .end{order:3;align-self:flex-end;background:#f0d0a0}
.sr-only{width:1px;height:1px;clip-path:rect(0 0 0 0);overflow-wrap:normal;border:0;padding:0;position:absolute;overflow:hidden}
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
<div class=clr style="background:#e0e0e0">A heading <span class=more style="background:#c06000">more</span></div>
<div class=wide style="background:#00a0ff">range</div>
<div class=narrow style="background:#a0ff00">narrow</div>
<table><tr><td class=cell-small style="background:#ff00a0">small</td><td style="background:#a000ff">large</td></tr></table>
<h2 class=sr-only style="background:#00ffa0">Latest commit</h2>
<div class=card>A card with a light background</div>
<div class=dark>Dark background under dark text</div>
<div class=column>A column of comfortable width</div>
<div class=tiny>Not a sliver</div>
<div class=ord><div class=o1>first in the source</div><div class=o2>second<br>in the<br>source</div><div class=end>align-self</div></div>
</body></html>'''

COLOURS = {'red': (255, 0, 0), 'green': (0, 192, 0), 'blue': (0, 0, 255), 'float': (255, 0, 255), 'cleared': (0, 255, 255),
           'g1': (255, 255, 0), 'g2': (255, 128, 0), 'g3': (128, 0, 255), 'g4': (0, 128, 128),
           'slide1': (128, 64, 0), 'slide2': (0, 64, 128), 'centred': (255, 0, 128),
           'tall': (255, 96, 96), 'second': (96, 255, 96), 'after': (96, 96, 255),
           'heading': (224, 224, 224), 'more': (192, 96, 0), 'range': (0, 160, 255), 'narrow': (160, 255, 0),
           'cell-small': (255, 0, 160), 'cell-large': (160, 0, 255), 'sr-only': (0, 255, 160),
           'card': (232, 240, 255), 'dark': (16, 24, 32), 'column': (255, 224, 192), 'tiny': (192, 255, 224),
           'o1': (240, 192, 240), 'o2': (192, 240, 192), 'end': (240, 208, 160)}
HIDDEN = ('slide2', 'narrow', 'cell-small', 'sr-only', 'dark')  # boxes that must not be drawn


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
    subprocess.run([EXE, 'file:///' + work.replace('\\', '/') + '/page.html', '--size', '800x1600',
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
    missing = [k for k in COLOURS if k not in box and k not in HIDDEN]
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
        h, mo = box['heading'], box['more']
        check('a right float in the middle of a line goes at the right of that line (not below it)',
              near(mo[2], right) and mo[1] < h[1] + 8, found)
        check('media range syntax: (width >= 600px) applies, (width <= calc(30rem - .02px)) and (300px < width <= 700px) don\'t',
              'range' in box and 'narrow' not in box, found)
        check('a table cell hidden by a stylesheet takes no column', 'cell-small' not in box and near(box['cell-large'][0], left, 6), found)
        check('a "visually hidden" heading (1px, clip-path: rect(0 0 0 0)) is not shown', 'sr-only' not in box, found)
        cd = box['card']
        check('margin: 0 40px and a light background', near(cd[0], left + 40) and near(cd[2], right - 40), found)
        check('... padding: 20px (the box is 20 px taller than its text on each side)', cd[3] - cd[1] >= 55, found)
        check('a dark background under our dark text is left out (it would be unreadable)', 'dark' not in box, found)
        co = box['column']
        check('max-width: 300px with margin auto: a centred column', near(co[2] - co[0], 300, 4) and near((co[0] + co[2]) / 2, (left + right) / 2, 4), found)
        check('width: 50px on a block of text is not a sliver', near(box['tiny'][0], left) and near(box['tiny'][2], right), found)
        check('flex order: the item with order 1 comes first', box['o2'][2] <= box['o1'][0], found)
        check('align-self: flex-end puts that item at the bottom of the row', box['end'][1] > box['o2'][1] + 20 and near(box['end'][3], box['o2'][3], 4), found)
finally:
    shutil.rmtree(work, ignore_errors=True)
print('ALL OK' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
