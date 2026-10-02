# Selecting, copying and pasting text, and the tab strip as the title bar. Runs
# bin/lowweb.exe in script mode, which has a clipboard of its own (the user's is not
# touched): "clip TEXT" fills it, and every copy is logged as "[clipboard] ..." (newlines
# as \n). Needs no internet.
import os, shutil, subprocess, sys, tempfile

sys.stdout.reconfigure(encoding='utf-8')  # Thai in the messages

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'bin', 'lowweb.exe')
work = tempfile.mkdtemp(prefix='lw-select-')
with open(os.path.join(work, 'page.html'), 'w', encoding='utf-8') as f:
    f.write('<!doctype html><html><head><title>sel</title></head><body>\n<h1>Hello world</h1>\n'
            '<p>The quick brown fox jumps over the <a href="#x">lazy dog</a>, then <b>runs</b> away.</p>\n'
            '<p>ภาษาไทย ทดสอบการเลือกข้อความ</p>\n<pre>line one\n  line two</pre>\n'
            '<form><input name=q value="abc"> <input type=password name=p value="secret"></form>\n</body></html>')
with open(os.path.join(work, 'note.txt'), 'w', encoding='utf-8') as f:
    f.write('first line of text\n\tindented with a tab\nthird line\n')
base = 'file:///' + work.replace('\\', '/') + '/'
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + name + ('' if ok else '\n      ' + detail.strip().replace('\n', '\n      ')))
    fails += not ok


def browse(url, script):
    log = os.path.join(work, 'log.txt')
    subprocess.run([EXE, url, '--size', '800x500', '--log', log, '--script', script,
                    '--screenshot', os.path.join(work, 'end.bmp')], timeout=120)
    text = open(log, encoding='utf-8', errors='replace').read()
    return text, [l[len('[clipboard] '):] for l in text.splitlines() if l.startswith('[clipboard] ')]


try:
    # (coordinates are in the view, 800 px wide; the page: heading, paragraph at y ~112, ...)
    log, clips = browse(base + 'page.html', '; '.join([
        'wait 800', 'key 65 ctrl', 'key 67 ctrl',             # Ctrl+A, Ctrl+C: all of it
        'drag 80 112 330 112', 'key 67 ctrl',                 # a drag over part of a line
        'click 400 300', 'key 67 ctrl',                       # a click elsewhere clears it: nothing copied
        'click 100 257', 'key 65 ctrl', 'key 67 ctrl',        # the text field: Ctrl+A, Ctrl+C
        'clip pasted\ntext', 'key 86 ctrl', 'key 65 ctrl', 'key 67 ctrl',  # Ctrl+V over the selection
        'key 35', 'key 86 ctrl', 'key 65 ctrl', 'key 88 ctrl',  # End, paste again (appends), then cut all
        'click 300 257', 'key 65 ctrl', 'key 67 ctrl',        # a password field doesn't give its text away
        'wait 100']))
    want_all = ('Hello world\\n\\nThe quick brown fox jumps over the lazy dog, then runs away.\\n\\n'
                'ภาษาไทย ทดสอบการเลือกข้อความ\\n\\nline one\\n  line two')
    check('Ctrl+A, Ctrl+C copies the page, with its line breaks', clips[:1] == [want_all], '\n'.join(clips))
    check('dragging selects part of a line (a link inside too)', clips[1:2] == ['k brown fox jumps over the lazy dog'], '\n'.join(clips))
    check('a click clears the selection', len(clips) >= 3 and clips[2] == 'abc', '\n'.join(clips))
    check('Ctrl+A, Ctrl+C in a text field copies its value', clips[2:3] == ['abc'], '\n'.join(clips))
    check('Ctrl+V replaces the selected value (a newline becomes a space)', clips[3:4] == ['pasted text'], '\n'.join(clips))
    check('Ctrl+X cuts (after a paste that appended)', clips[4:5] == ['pasted textpasted text'], '\n'.join(clips))
    check('a password field copies nothing', len(clips) == 5, '\n'.join(clips))

    log, clips = browse(base + 'note.txt', 'wait 500; drag 30 22 150 42; key 67 ctrl; key 65 ctrl; key 67 ctrl; wait 100')
    check('a text file: a drag over two lines (a tab shown as spaces)', clips[:1] == ['irst line of text\\n        indented w'], '\n'.join(clips))
    check('... and Ctrl+A copies all of it', clips[1:2] == ['first line of text\\n        indented with a tab\\nthird line\\n'], '\n'.join(clips))

    log, clips = browse('http://127.0.0.1:1/', 'wait 1500; key 65 ctrl; key 67 ctrl; wait 100')
    check("an error page's text can be copied", clips[:1] and clips[0].startswith("Can't reach this page\\n\\ncould not connect"), '\n'.join(clips))

    # the tab strip is the title bar: 2 = HTCAPTION (moves the window), 1 = ours, 12 = HTTOP (resizes)
    log, _ = browse(base + 'page.html', 'wait 500; hittest 600 20; hittest 100 20; hittest 780 18; hittest 400 2; hittest 400 60; '
                                        'tabclick 731 18; wait 500; hittest 400 2; wait 200')
    hits = [l.split('-> ')[1] for l in log.splitlines() if l.startswith('[hittest]')]
    check('empty strip: caption; tab, window button, toolbar: ours; top edge: resize',
          hits[:5] == ['2', '1', '1', '12', '1'], '\n'.join(hits))
    check('the maximize button maximizes (then the top edge is caption)', hits[5:6] == ['2 (maximized)'], '\n'.join(hits))
finally:
    shutil.rmtree(work, ignore_errors=True)
print('ALL OK' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
