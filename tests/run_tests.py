# Runs every test that needs nothing but this repository (no internet, no Edge), after
# build.cmd. Used by CI (.github/workflows/ci.yml) and handy before a commit:
#   python tests/run_tests.py [NAME...]     (names: see TESTS below; default all)
# Left out on purpose: tests/bench_viewer.py (downloads Wikipedia) and tests/check_svg.py
# (renders with Edge and is judged by eye).
import os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
BIN = os.path.join(ROOT, 'bin')
PY = sys.executable


def run(cmd, timeout=600):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r.returncode, (r.stdout + r.stderr).replace('\r', '')


def ops():
    # the interpreter against V8 (node): the same results, with and without fused instructions
    calls = ['int32()', 'int64()', 'floats()', 'control()', 'memory_ops()', 'trap_div(1,0)',
             'trap_div(-2147483648,-1)', 'trap_oob()', 'add3(1,2,0.5)']
    code, v8 = run(['node', 'tests/check_ops.mjs', 'build/ops.wasm'])
    if code:
        return False, v8
    want = v8.strip().splitlines()
    out = ''
    ok = True
    for flags in ([], ['--no-fuse']):
        _, got = run([os.path.join(BIN, 'wasmrun.exe')] + flags + ['build/ops.wasm'] + calls)
        got = got.strip().splitlines()
        for w, g in zip(want, got):
            # trap messages are worded differently by V8; that it trapped is what counts
            same = w == g or ('-> trap:' in w and '-> trap:' in g and w.split('->')[0] == g.split('->')[0])
            ok &= same
            out += ('ok    ' if same else 'FAIL  ') + ' '.join(flags) + ' ' + g + ('' if same else '   (V8: ' + w + ')') + '\n'
        ok &= len(got) == len(want)
    return ok, out


def viewer():
    # the HTML viewer on generated pages (UTF-8, and Thai in windows-874, which is converted
    # first): no trap, and the same last frame without fused instructions, and when the page
    # comes in small pieces as it downloads (the parsed part is thrown away meanwhile)
    os.makedirs('build/runtests', exist_ok=True)
    rows = ''.join('<tr><td>%d</td><td>row <b>%d</b></td><td><a href="#r%d">link</a></td></tr>' % (i, i * i, i) for i in range(3000))
    items = ''.join('<li>item %d with <i>some</i> <code>text</code></li>' % i for i in range(2000))
    para = ' '.join('word%d' % i for i in range(4000))
    html = ('<!doctype html><html><head><meta charset="%s"><title>t</title><style>h1{color:#c00} .x td{padding:4px} '
            'p.lead{font-size:20px} ul li:nth-child(odd){color:#036}</style></head><body>'
            '<h1>Heading</h1><p class="lead">%s</p><ul>%s</ul><table class="x">%s</table>'
            '<h2 id="end">ภาษาไทย ทดสอบ</h2><pre>  pre\n  text</pre></body></html>')
    out = ''
    ok = True
    for name, cs, py_cs in (('page.html', 'utf-8', 'utf-8'), ('page-874.html', 'windows-874', 'cp874')):
        page = 'build/runtests/' + name
        with open(page, 'w', encoding=py_cs) as f:
            f.write(html % (cs, para + ' ภาษาไทย' * 200, items, rows))
        frames = []
        for flags in ([], ['--no-fuse'], ['--stream', '3000']):
            code, o = run([os.path.join(BIN, 'viewerbench.exe')] + flags + ['build/viewer.wasm', page])
            out += '%s %s:\n%s' % (name, ' '.join(flags), o)
            fnv = [l for l in o.splitlines() if l.startswith('final frame fnv')]
            if code or not fnv or 'trap' in o:
                return False, out
            frames.append(fnv[0])
        same = len(set(frames)) == 1
        ok &= same
        out += '%s: %s\n' % (name, 'same final frame' if same else 'DIFFERENT FRAMES')
    return ok, out


def script(*args):
    return lambda: (lambda r: (r[0] == 0, r[1]))(run([PY] + list(args)))


TESTS = [
    ('ops', ops),
    ('viewer', viewer),
    ('images', script('tests/check_images.py')),
    ('stream', script('tests/check_stream.py')),
    ('cache', script('tests/check_cache.py')),
    ('cookies', script('tests/check_cookies.py')),
    ('hpack', script('tests/check_hpack.py', 'tests/samples/rfc7541.txt')),
    ('access', script('tests/check_access.py')),
    ('sleep', script('tests/check_sleep.py')),
    ('select', script('tests/check_select.py')),
]

only = sys.argv[1:]
failed = []
for name, fn in TESTS:
    if only and name not in only:
        continue
    t0 = time.time()
    try:
        ok, out = fn()
    except Exception as e:  # a missing tool, a timeout ...
        ok, out = False, repr(e)
    print('%-8s %s  (%.1f s)' % (name, 'ok' if ok else 'FAILED', time.time() - t0), flush=True)
    if not ok:
        failed.append(name)
        print('    ' + out.strip().replace('\n', '\n    '), flush=True)
print('ALL OK' if not failed else 'FAILED: ' + ', '.join(failed))
sys.exit(1 if failed else 0)
