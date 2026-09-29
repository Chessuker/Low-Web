# Times the HTML viewer on a big real page (Wikipedia "United States", ~3 MB) without a
# window, with and without fused instructions, and checks that both draw exactly the same
# final frame (a test of the interpreter's instruction fusion on a real program).
#   python tests/bench_viewer.py [URL]
# The page and its stylesheets are downloaded once into build/bench/.
import html, os, re, subprocess, sys, urllib.parse, urllib.request

url = sys.argv[1] if len(sys.argv) > 1 else 'https://en.wikipedia.org/wiki/United_States'
work = os.path.abspath('build/bench')
os.makedirs(work, exist_ok=True)
page = os.path.join(work, 'page.html')
fetchmap = os.path.join(work, 'fetchmap.txt')
stamp = os.path.join(work, 'url.txt')
if not os.path.exists(page) or not os.path.exists(stamp) or open(stamp).read() != url:
    ua = {'User-Agent': 'Low-web/0.1'}
    body = urllib.request.urlopen(urllib.request.Request(url, headers=ua)).read()
    open(page, 'wb').write(body)
    lines = []
    for tag in re.findall(rb'<link[^>]*rel="stylesheet"[^>]*>', body):
        m = re.search(rb'href="([^"]*)"', tag)
        if not m:
            continue
        href = html.unescape(m.group(1).decode())
        css = urllib.request.urlopen(urllib.request.Request(urllib.parse.urljoin(url, href), headers=ua)).read()
        name = os.path.join(work, f'css{len(lines)}.css')
        open(name, 'wb').write(css)
        lines.append(f'{href}\t{name}\n')
    open(fetchmap, 'w').write(''.join(lines))
    open(stamp, 'w').write(url)

viewer = os.path.abspath('build/viewer.wasm')
results = {}
for label, flags in (('fused', []), ('not fused', ['--no-fuse'])):
    best, frame = None, None
    for _ in range(3):
        out = subprocess.run([os.path.abspath('bin/viewerbench.exe'), *flags, viewer, page, url, fetchmap],
                             capture_output=True, text=True, timeout=300).stdout
        t = re.search(r'interpreter (\d+) ms', out)
        f = re.search(r'final frame fnv (\w+)', out)
        if not t or not f:
            print(out)
            sys.exit(1)
        best = min(best or 10**9, int(t.group(1)))
        frame = f.group(1)
    results[label] = (best, frame)
    print(f'{label:10s} interpreter {best} ms (best of 3), final frame {frame}')
same = results['fused'][1] == results['not fused'][1]
print(('ok   ' if same else 'FAIL ') + 'both draw the same final frame')
print(f'fusion: {results["not fused"][0] / max(results["fused"][0], 1):.2f}x faster')
sys.exit(0 if same else 1)
