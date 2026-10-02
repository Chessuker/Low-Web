# Text fields (caret, selection, keys, mouse, textarea, Tab), find in page (Ctrl+F) and
# #fragments written in Thai, raw or percent-encoded. Runs bin/lowweb.exe in script mode
# with its own clipboard (copies are logged as "[clipboard] ...", newlines as \n) and its
# find bar status logged as "[find] current/count". Needs no internet.
import os, shutil, subprocess, sys, tempfile
from PIL import Image, ImageChops

sys.stdout.reconfigure(encoding='utf-8')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'bin', 'lowweb.exe')
work = tempfile.mkdtemp(prefix='lw-edit-')
body = ''.join('<p>Paragraph %d. The quick brown fox jumps over the lazy dog again and again.</p>' % i for i in range(40))
ENC = '%E0%B8%9B%E0%B8%A3%E0%B8%B0%E0%B8%A7%E0%B8%B1%E0%B8%95%E0%B8%B4'  # ประวัติ
with open(os.path.join(work, 'p.html'), 'w', encoding='utf-8', newline='\n') as f:
    f.write('<!doctype html><html><head><meta charset="utf-8"><title>t</title></head><body>\n<h1>Fields</h1>\n'
            '<form><input name=q value="abc"> <input type=password name=p value="pw"><br>'
            '<textarea name=t rows=3 cols=30></textarea></form>\n'
            '<p><a href="#ประวัติ">raw link</a> <a href="#' + ENC + '">encoded link</a></p>\n' + body +
            '<h2 id="ประวัติ">ประวัติ heading</h2><p>after the heading</p>' + body + '</body></html>')
with open(os.path.join(work, 'n.txt'), 'w', encoding='utf-8', newline='\n') as f:
    f.write('alpha beta\ngamma ALPHA\nnothing\nalphabet\n')
url = 'file:///' + work.replace('\\', '/') + '/p.html'
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + name + ('' if ok else '\n      ' + str(detail).strip().replace('\n', '\n      ')))
    fails += not ok


def browse(u, script, shot='end.bmp'):
    log = os.path.join(work, 'log.txt')
    subprocess.run([EXE, u, '--size', '800x500', '--log', log, '--script', script, '--screenshot', os.path.join(work, shot)], timeout=120)
    text = open(log, encoding='utf-8', errors='replace').read()
    return ([l[12:] for l in text.splitlines() if l.startswith('[clipboard] ')],
            [l[7:] for l in text.splitlines() if l.startswith('[find] ')], text)


def page_area(name):
    im = Image.open(os.path.join(work, name)).convert('RGB')
    return im.crop((0, 80, im.width, im.height))


try:
    # ---- text fields (view coordinates: the field q at y 113, x 21..220; the textarea below it)
    copy = 'key 65 ctrl; key 67 ctrl'
    clips, _, log = browse(url, '; '.join([
        'wait 800', 'click 200 113', 'chars  def', 'key 36', 'chars X', copy,   # click at the end, type, Home, type
        'key 37', 'key 46', 'key 35', 'key 8', copy,                             # Left (to the start), Delete, End, Backspace
        'key 35', 'key 37 ctrl', 'key 35 shift', 'key 67 ctrl',                  # Ctrl+Left: a word back; Shift+End selects
        'key 37', 'key 39', 'key 39 shift', 'key 88 ctrl',                       # Shift+Right selects a letter, Ctrl+X cuts it
        'key 35', 'chars กี้', 'key 37', 'chars Y', copy,                         # Left steps over a Thai letter with its marks
        'key 35', 'key 8', copy,                                                 # Backspace takes one mark
        'click 30 113', 'click 30 113', 'key 67 ctrl',                           # a double click selects a word
        'key 9', 'chars secret', 'key 65 ctrl', 'key 67 ctrl',                   # Tab: the password (nothing to copy)
        'key 9', 'chars line1', 'key 13', 'chars line2', 'key 38', 'key 36', 'chars >', copy,  # the textarea: Enter, Up, Home
        'clip a\nb', 'key 35 ctrl', 'key 86 ctrl', copy,                         # pasting keeps its newline
        'wait 100']))
    want = ['Xabc def', 'abc de', 'de', 'e', 'abc dYกี้', 'abc dYกี', 'abc', '>line1\\nline2', '>line1\\nline2a\\nb']
    check('typing, Home/End, arrows, Delete/Backspace, Shift-selections, Ctrl+X, Thai marks, double click', clips[:7] == want[:7],
          '\n'.join(clips))
    check('Tab goes to the next field; a password gives nothing to copy', len(clips) >= 7 and clips[7:8] == [want[7]], '\n'.join(clips))
    check('a textarea: Enter, Up, Home, and pasting with a newline', clips[7:9] == want[7:9], '\n'.join(clips))
    check('no page crashed', 'crash' not in log, log)

    # ---- the mouse in a field: drag to select
    clips, _, _ = browse(url, 'wait 800; click 200 113; chars  defghij; drag 24 113 48 113; key 67 ctrl; wait 100')
    check('dragging in a field selects', clips[:1] == ['abc'], '\n'.join(clips))

    # ---- #fragments
    browse(url + '#' + ENC, 'wait 1000', 'f1.bmp')
    browse(url + '#ประวัติ', 'wait 1000', 'f2.bmp')
    browse(url, 'wait 800; click 120 215; wait 800', 'f3.bmp')
    browse(url, 'wait 800; click 40 215; wait 800', 'f4.bmp')
    browse(url, 'wait 800', 'f0.bmp')
    same = [ImageChops.difference(page_area('f1.bmp'), page_area(n)).getbbox() is None for n in ('f2.bmp', 'f3.bmp', 'f4.bmp')]
    check('a percent-encoded Thai #fragment goes to its heading', ImageChops.difference(page_area('f1.bmp'), page_area('f0.bmp')).getbbox() is not None)
    check('... as the raw one does, and links of both kinds', all(same), same)

    # ---- find in page
    _, found, _ = browse(url, 'wait 800; key 70 ctrl; find quick brown; findnext; findnext; findprev; find FOX JUMPS OVER; '
                              'find nothing-here; findclose; wait 100')
    check('Ctrl+F: matches counted, next/previous, case ignored, none found', found == ['1/80', '2/80', '3/80', '2/80', '1/80', '0/0'], found)
    _, found, _ = browse(url.replace('p.html', 'n.txt'), 'wait 500; find alpha; findnext; findnext; findnext; wait 100')
    check("in the browser's own documents too (a text file)", found == ['1/3', '2/3', '3/3', '1/3'], found)
finally:
    shutil.rmtree(work, ignore_errors=True)
print('ALL OK' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
