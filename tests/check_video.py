# Video and audio in the HTML viewer, played by Windows (Media Foundation): click to play,
# pause, seek, the end, <audio>, a missing file, and over HTTP a seek past what has been
# downloaded (a range request, tests/slow_server.py on a free port). The video is
# tests/samples/colors.mp4: one second each of red, green, blue and yellow (and a tone), so
# a screenshot tells where it is. Needs no internet; says SKIP where Windows has no Media
# Foundation (some servers, "N" editions).
import os, re, shutil, socket, subprocess, sys, tempfile, time
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'bin', 'lowweb.exe')
TOP = 80  # the view starts below the tab strip and the toolbar (at 96 DPI)
BAR = (28, 28, 31)  # the controls' bar
COLORS = {'red': (255, 0, 0), 'green': (0, 255, 0), 'blue': (0, 0, 255), 'yellow': (255, 255, 0)}
ORDER = ['red', 'green', 'blue', 'yellow']
work = tempfile.mkdtemp(prefix='lw-video-')
for f in ('colors.mp4', 'tone.mp3'):
    shutil.copy(os.path.join(ROOT, 'tests', 'samples', f), work)
PAGE = ('<!doctype html><html><head><meta charset="utf-8"><title>video</title></head><body>\n<p>Before the video.</p>\n'
        '<video controls width="320" height="180" src="%s"></video>\n<p>Between.</p>\n<div><audio controls src="tone.mp3"></audio></div>\n'
        '<div><video controls width="320" height="180" src="missing.mp4"></video></div>\n</body></html>')
with open(os.path.join(work, 'page.html'), 'w', encoding='utf-8') as f:
    f.write(PAGE % 'colors.mp4')


def pad_mp4(src, dst, pad):
    # colors.mp4 with `pad` bytes (a "free" box) between its index (moov) and its data (mdat),
    # the data's offsets in the index moved to match: the pictures are far from the start, as
    # in a big file, so reading them before the download gets there needs a range request.
    import struct
    b = bytearray(open(src, 'rb').read())

    def boxes(lo, hi):
        while lo + 8 <= hi:
            size, kind = struct.unpack('>I4s', b[lo:lo + 8])
            yield lo, size, kind
            lo += size

    def shift(lo, hi):
        for at, size, kind in boxes(lo, hi):
            if kind in (b'trak', b'mdia', b'minf', b'stbl'):
                shift(at + 8, at + size)
            elif kind == b'stco':
                n = struct.unpack('>I', b[at + 12:at + 16])[0]
                for i in range(n):
                    o = at + 16 + 4 * i
                    b[o:o + 4] = struct.pack('>I', struct.unpack('>I', b[o:o + 4])[0] + pad)
    top = list(boxes(0, len(b)))
    moov = [t for t in top if t[2] == b'moov'][0]
    shift(moov[0] + 8, moov[0] + moov[1])
    end = moov[0] + moov[1]
    open(dst, 'wb').write(bytes(b[:end]) + struct.pack('>I4s', pad, b'free') + bytes(pad - 8) + bytes(b[end:]))


pad_mp4(os.path.join(work, 'colors.mp4'), os.path.join(work, 'far.mp4'), 3000000)
with open(os.path.join(work, 'slow.html'), 'w', encoding='utf-8') as f:
    f.write(PAGE % 'far.mp4?delay=0.02&chunk=8000')  # ~200 KB/s: 15 seconds for the whole file
fails = 0


def check(name, ok, detail=''):
    global fails
    print(('ok    ' if ok else 'FAIL  ') + name + ('' if ok else '\n      ' + str(detail).replace('\n', '\n      ')))
    fails += not ok


def run(url, script, shot='end.bmp'):
    log = os.path.join(work, 'log.txt')
    subprocess.run([EXE, url, '--size', '800x600', '--no-cache', '--log', log, '--script', script,
                    '--screenshot', os.path.join(work, shot)], timeout=120)
    return open(log, encoding='utf-8', errors='replace').read()


def view(name):
    im = Image.open(os.path.join(work, name)).convert('RGB')
    return im.crop((0, TOP, im.width, im.height))


def bars(im):  # the control bars, top to bottom: (x0, y0, x1, y1)
    px = im.load()
    rows = [y for y in range(im.height) if sum(px[x, y] == BAR for x in range(0, im.width, 2)) > 20]
    out = []
    for y in rows:
        if out and y <= out[-1][3] + 4:  # (the rows through the track have less of the bar's colour)
            out[-1][3] = y + 1
        else:
            xs = [x for x in range(im.width) if px[x, y] == BAR]
            out.append([min(xs), y, max(xs) + 1, y + 1])
    return out


def track(im, b):  # the seek track in a bar: its x range (the row through the middle of the bar)
    px = im.load()
    y = (b[1] + b[3]) // 2
    best, run = None, None  # the longest run of its grey
    for x in range(b[0], b[2] + 1):
        grey = x < b[2] and all(abs(px[x, y][i] - (90, 90, 93)[i]) < 20 for i in range(3))
        if grey and run is None: run = x
        if not grey and run is not None:
            if best is None or x - run > best[1] - best[0]: best = (run, x - 1)
            run = None
    return best


def colour_of(im, box):  # the colour filling most of the picture
    x0, y0, x1, y1 = box
    seen = {}
    for y in range(y0 + 10, y1 - 10, 6):
        for x in range(x0 + 10, x1 - 10, 6):
            c = im.getpixel((x, y))
            for name, want in COLORS.items():
                if all(abs(c[i] - want[i]) < 60 for i in range(3)):
                    seen[name] = seen.get(name, 0) + 1
    return max(seen, key=seen.get) if seen else None


def states(log):  # "[video] ID state S time T ..." -> [(id, state, time, error)]
    return [(int(m[1]), int(m[2]), float(m[3]), m[4] or '') for m in
            re.finditer(r'\[video\] (\d+) state (\d) time ([\d.]+) duration [\d.]+ size \d+x\d+ buffered -?[\d.]+(?: error: (.*))?', log)]


def expected(t):  # the colours a picture at time t may show (either side of a change)
    return {ORDER[min(3, max(0, int(t + d)))] for d in (-0.2, 0, 0.2)}


try:
    base = 'file:///' + work.replace('\\', '/') + '/'
    log = run(base + 'page.html', 'wait 800', 'before.bmp')
    if '[video] not available' in log:
        print('SKIP  this Windows has no Media Foundation: ' + log.split('[video] not available:')[1].splitlines()[0].strip())
        sys.exit(0)
    im = view('before.bmp')
    found = bars(im)
    check('a video, an <audio> and a second video: three bars of controls', len(found) == 3, found)
    v1, au, v2 = found[0], found[1], found[2]
    pic = (v1[0], v1[1] - (v1[2] - v1[0]) * 180 // 320, v1[2], v1[1])  # the picture is above its bar
    px = im.load()
    white = sum(px[x, y] == (255, 255, 255) for x in range(pic[0], pic[2]) for y in range(pic[1], pic[3]))
    check('before playing: a black picture with a play button, nothing downloaded',
          px[pic[0] + 5, pic[1] + 5] == (0, 0, 0) and white > 200 and '[video]' not in log.replace('[video] Media', ''), white)
    tr = track(im, v1)
    check('the bar has a seek track', tr is not None and tr[1] - tr[0] > 80, tr)
    cx, cy = (pic[0] + pic[2]) // 2, (pic[1] + pic[3]) // 2

    # play, pause, look; seek to 85% (paused), look; play to the end
    seek_x = tr[0] + (tr[1] - tr[0]) * 85 // 100
    script = '; '.join([
        'wait 800', 'click %d %d' % (cx, cy), 'wait 1300', 'key 32', 'wait 400', 'video', 'shot ' + os.path.join(work, 'paused.bmp'),
        'click %d %d' % (seek_x, (v1[1] + v1[3]) // 2), 'wait 700', 'video', 'shot ' + os.path.join(work, 'seeked.bmp'),
        'key 32', 'wait 1500', 'video',
        'key 70', 'wait 800', 'shot ' + os.path.join(work, 'full.bmp'), 'key 27', 'wait 800', 'shot ' + os.path.join(work, 'back.bmp'),
        'click %d %d' % (au[0] + 15, (au[1] + au[3]) // 2), 'wait 800', 'video',
        'click %d %d' % ((v2[0] + v2[2]) // 2, v2[1] - 50), 'wait 1000', 'video', 'shot ' + os.path.join(work, 'missing.bmp')])
    log = run(base + 'page.html', script)
    st = states(log)
    if any('no sound device' in s[3] for s in st):  # (CI machines often have none)
        print('SKIP  this computer has no sound device, which Windows needs to play videos')
        sys.exit(0)
    one = [s for s in st if s[0] == min(x[0] for x in st)] if st else []
    check('click: it plays; Space: it pauses', len(one) >= 1 and one[0][1] == 1 and 0.8 < one[0][2] < 2.5, one[:1])
    if one:
        c = colour_of(view('paused.bmp'), pic)
        check('the browser draws the picture where it was (%s at %.2f s)' % (c, one[0][2]), c in expected(one[0][2]), c)
    check('a click on the track seeks there (85%: 3.4 s), still paused', len(one) >= 2 and one[1][1] == 1 and abs(one[1][2] - 3.4) < 0.3,
          one[1:2])
    if len(one) >= 2:
        c = colour_of(view('seeked.bmp'), pic)
        check('... and shows that picture (%s)' % c, c == 'yellow', c)
    check('Space again: it plays to the end', len(one) >= 3 and one[2][1] == 3, one[2:3])
    full, back = Image.open(os.path.join(work, 'full.bmp')), Image.open(os.path.join(work, 'back.bmp'))
    fb = bars(full.convert('RGB'))
    check('F: fullscreen, the video alone with its bar at the bottom; Esc: back',
          '[low-web] fullscreen\n' in log and '[low-web] fullscreen off' in log and full.width > 800 and len(fb) == 1 and
          fb[0][3] >= full.height - 2 and back.size == Image.open(os.path.join(work, 'paused.bmp')).size, (full.size, fb, back.size))
    audio = [s for s in st if s[0] == min(x[0] for x in st) + 1] if st else []
    check('<audio>: its button plays it', any(s[1] == 2 for s in audio), audio)
    missing = [s for s in st if s[0] == min(x[0] for x in st) + 2] if st else []
    check('a missing file: the video says it failed, and why', any(s[1] == 4 and 'not found' in s[3] for s in missing), missing)
    mim = view('missing.bmp')
    msg = sum(mim.getpixel((x, y)) == (255, 255, 255) for x in range(v2[0], v2[0] + 200) for y in range(v2[1] - 180, v2[1] - 150))
    check('... on its picture, in text', msg > 20, msg)

    # over HTTP, slowly, with the pictures 3 MB from the start: they are asked for from there
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    out = os.path.join(work, 'server.txt')
    with open(out, 'w') as so:
        server = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tests', 'slow_server.py'), str(port), work], stdout=so,
                                  stderr=subprocess.STDOUT)
        try:
            time.sleep(0.5)
            log = run('http://127.0.0.1:%d/slow.html' % port, '; '.join([
                'wait 800', 'click %d %d' % (cx, cy), 'wait 1500', 'video',
                'click %d %d' % (seek_x, (v1[1] + v1[3]) // 2), 'wait 3500', 'video']))
        finally:
            server.kill()
    served = open(out).read()
    starts = [int(m) for m in re.findall(r'GET /far\.mp4 from (\d+)', served)]
    check('over HTTP: data far past what has arrived is asked for from there (a range request)',
          len(starts) >= 2 and starts[0] == 0 and max(starts) > 2000000, served)
    st = states(log)
    check('... and played from there', len(st) >= 2 and st[-1][1] in (2, 3) and st[-1][2] > 3.2, st)
finally:
    shutil.rmtree(work, ignore_errors=True)
print('ALL OK' if not fails else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
