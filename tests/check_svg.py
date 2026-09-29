# Renders test SVGs with our decoder (bin/imgdump.exe) and with Microsoft Edge (headless)
# as a reference, then writes side-by-side images to build/svgtest/ and prints how far
# apart they are. Anti-aliasing differs a little between renderers, so compare by eye
# and by the mean difference, not for exact equality.
import os, subprocess, sys
import numpy as np
from PIL import Image

out = 'build/svgtest'
os.makedirs(out, exist_ok=True)
EDGE = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'

tests = {
'shapes': '''<svg xmlns="http://www.w3.org/2000/svg" width="240" height="160">
  <rect x="10" y="10" width="80" height="50" fill="#e33" stroke="#222" stroke-width="3"/>
  <rect x="110" y="10" width="80" height="50" rx="12" fill="rgb(40,120,230)" fill-opacity="0.7"/>
  <circle cx="50" cy="110" r="35" fill="none" stroke="green" stroke-width="6"/>
  <ellipse cx="150" cy="110" rx="50" ry="25" fill="orange"/>
  <line x1="200" y1="10" x2="235" y2="150" stroke="purple" stroke-width="4" stroke-linecap="round"/>
</svg>''',
'paths': '''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 120" width="400" height="240">
  <path d="M10 60 C 30 10, 70 10, 90 60 S 150 110, 170 60" fill="none" stroke="#06c" stroke-width="4"/>
  <path d="m10 100 q20-30 40 0t40 0 40 0" fill="none" stroke="#c60" stroke-width="3"/>
  <path d="M120 20 a20 15 0 1 0 40 0 a20 15 0 1 0-40 0z" fill="#3a3"/>
  <path d="M175 5h20v20h-20zM180 10v10h10v-10z" fill="#333" fill-rule="evenodd"/>
</svg>''',
'transforms': '''<svg xmlns="http://www.w3.org/2000/svg" width="220" height="220">
  <g transform="translate(110 110)">
    <g fill="#f80" opacity="0.8">
      <rect x="-20" y="-80" width="40" height="60" transform="rotate(0)"/>
      <rect x="-20" y="-80" width="40" height="60" transform="rotate(72)"/>
      <rect x="-20" y="-80" width="40" height="60" transform="rotate(144)"/>
      <rect x="-20" y="-80" width="40" height="60" transform="rotate(216)"/>
      <rect x="-20" y="-80" width="40" height="60" transform="rotate(288)"/>
    </g>
    <circle r="18" fill="#036" transform="scale(1.5 1)"/>
    <rect x="-100" y="80" width="60" height="15" fill="#909" transform="skewX(-30)"/>
  </g>
</svg>''',
'gradients': '''<svg xmlns="http://www.w3.org/2000/svg" width="300" height="140">
  <defs>
    <linearGradient id="a" x1="0" x2="1"><stop offset="0" stop-color="#f00"/><stop offset=".5" stop-color="#ff0"/><stop offset="1" stop-color="#00f"/></linearGradient>
    <linearGradient id="b" x1="0%" y1="0%" x2="0%" y2="100%"><stop offset="0%" style="stop-color:#fff;stop-opacity:1"/><stop offset="100%" style="stop-color:#080;stop-opacity:0.3"/></linearGradient>
    <radialGradient id="c" cx="0.5" cy="0.5" r="0.5"><stop offset="0" stop-color="#fff"/><stop offset="1" stop-color="#339"/></radialGradient>
    <linearGradient id="d" gradientUnits="userSpaceOnUse" x1="200" y1="0" x2="290" y2="0" xlink:href="#a" xmlns:xlink="http://www.w3.org/1999/xlink"/>
  </defs>
  <rect x="5" y="5" width="140" height="60" fill="url(#a)"/>
  <rect x="155" y="5" width="60" height="130" fill="url(#b)" stroke="#000"/>
  <circle cx="75" cy="102" r="33" fill="url(#c)"/>
  <rect x="200" y="75" width="90" height="60" fill="url(#d)"/>
</svg>''',
'strokes': '''<svg xmlns="http://www.w3.org/2000/svg" width="300" height="170">
  <g fill="none" stroke="#135" stroke-width="12">
    <polyline points="20,60 60,20 100,60" stroke-linejoin="miter"/>
    <polyline points="120,60 160,20 200,60" stroke-linejoin="round" stroke-linecap="round"/>
    <polyline points="220,60 260,20 290,60" stroke-linejoin="bevel" stroke-linecap="square"/>
  </g>
  <path d="M20 110 H280" stroke="#c00" stroke-width="6" stroke-dasharray="20 10 5 10"/>
  <circle cx="150" cy="140" r="20" fill="#ffd" stroke="#555" stroke-width="3" stroke-dasharray="6 4"/>
  <polygon points="40,125 55,160 20,140 60,140 25,160" fill="#46a" stroke="#000" stroke-width="1" fill-rule="evenodd"/>
  <polygon points="240,125 255,160 220,140 260,140 225,160" fill="#46a"/>
</svg>''',
'styles': '''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="260" height="120" style="color:#a0a">
  <style>.box{fill:#0a6;stroke:#030;stroke-width:2} #special{fill:#f60} rect.small{opacity:.5}</style>
  <defs><symbol id="star" viewBox="0 0 10 10"><path d="M5 0L6.5 3.5 10 4 7.5 6.5 8 10 5 8 2 10 2.5 6.5 0 4 3.5 3.5z"/></symbol>
        <g id="dot"><circle r="8" fill="currentColor"/></g></defs>
  <rect class="box" x="10" y="10" width="60" height="40"/>
  <rect class="box" id="special" x="80" y="10" width="60" height="40"/>
  <rect class="box small" x="150" y="10" width="60" height="40"/>
  <use xlink:href="#star" x="10" y="60" width="50" height="50" fill="gold" stroke="#850" stroke-width="0.3"/>
  <use href="#dot" x="100" y="85"/>
  <use href="#dot" x="130" y="85" style="color:#09c"/>
  <g style="fill:#c33;stroke:none"><rect x="170" y="65" width="40" height="40" style="fill:#39c"/><rect x="215" y="65" width="40" height="40"/></g>
</svg>''',
'aspect': '''<svg xmlns="http://www.w3.org/2000/svg" width="300" height="100" viewBox="0 0 50 50" preserveAspectRatio="xMaxYMid meet">
  <rect width="50" height="50" fill="#eee" stroke="#000"/><circle cx="25" cy="25" r="20" fill="#e60"/>
</svg>''',
}

# a few real-world SVGs (tests/samples)
import shutil
real = []
for f in sorted(os.listdir('tests/samples')):
    if f.startswith('real_') and f.endswith('.svg'):
        shutil.copy(f'tests/samples/{f}', f'{out}/{f}')
        real.append(f)


def ours(path):
    r = subprocess.run([os.path.abspath('bin/imgdump.exe'), path, path + '.rgba'], capture_output=True, text=True)
    if r.returncode:
        return None, r.stdout.strip()
    raw = open(path + '.rgba', 'rb').read()
    os.remove(path + '.rgba')  # keep build/ small: only the side-by-side .cmp.png stays
    hdr, data = raw.split(b'\n', 1)
    w, h = map(int, hdr.split())
    img = Image.frombytes('RGBA', (w, h), data)
    bg = Image.new('RGBA', (w, h), (255, 255, 255, 255))
    return Image.alpha_composite(bg, img).convert('RGB'), None


def edge(path, w, h):
    shot = os.path.abspath(path + '.edge.png')
    html = os.path.abspath(path + '.html')
    uri = 'file:///' + os.path.abspath(path).replace('\\', '/')
    open(html, 'w').write(f'<html><body style="margin:0;background:#fff"><img src="{uri}" style="display:block"></body></html>')
    subprocess.run([EDGE, '--headless', '--disable-gpu', '--hide-scrollbars', f'--screenshot={shot}',
                    f'--window-size={max(w, 100)},{max(h, 100)}', 'file:///' + html.replace('\\', '/')],
                   capture_output=True, timeout=60)
    img = Image.open(shot).convert('RGB').crop((0, 0, w, h))
    os.remove(shot)
    os.remove(html)
    return img


worst = 0
for name in list(tests) + real:
    path = f'{out}/{name}.svg' if name in tests else f'{out}/{name}'
    if name in tests:
        open(path, 'w', encoding='utf-8').write(tests[name])
    a, err = ours(path)
    if a is None:
        print(f'{name:28s} FAIL {err}')
        worst = 999
        continue
    try:
        b = edge(path, a.width, a.height)
    except (subprocess.TimeoutExpired, OSError) as e:
        print(f'{name:28s} (Edge did not answer; reference skipped)')
        continue
    d = np.abs(np.asarray(a, int) - np.asarray(b, int))
    mean = d.mean()
    worst = max(worst, mean)
    side = Image.new('RGB', (a.width * 2 + 10, a.height), (255, 0, 255))
    side.paste(a, (0, 0))
    side.paste(b, (a.width + 10, 0))
    side.save(f'{out}/{name}.cmp.png')
    print(f'{name:28s} {a.width}x{a.height}  mean_diff={mean:.2f}  (left: ours, right: Edge)')
print('worst mean diff', round(worst, 2))
