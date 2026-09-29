# Generates test images with PIL, decodes them with bin/imgdump.exe, compares pixels.
import os, subprocess, sys, io
from PIL import Image, ImageDraw
import numpy as np

out = 'build/imgtest'; os.makedirs(out, exist_ok=True)
W, H = 173, 121
base = Image.new('RGBA', (W, H))
px = base.load()
for y in range(H):
    for x in range(W):
        px[x, y] = ((x * 255) // W, (y * 255) // H, (x * y) % 256, 255 if (x // 16 + y // 16) % 2 else 90)
d = ImageDraw.Draw(base); d.ellipse((30, 20, 120, 100), fill=(250, 250, 20, 255)); d.text((10, 5), "Low-web", fill=(0, 0, 0, 255))

cases = []
def add(name, img, fmt, **kw):
    p = f'{out}/{name}'; img.save(p, fmt, **kw); cases.append((name, p, fmt))

add('rgba.png', base, 'PNG')
add('rgb.png', base.convert('RGB'), 'PNG')
add('gray.png', base.convert('L'), 'PNG')
add('gray_alpha.png', base.convert('LA'), 'PNG')
add('palette.png', base.convert('RGB').quantize(37), 'PNG')
pt = base.convert('RGB').quantize(16); pt.info['transparency'] = 3; add('palette_trns.png', pt, 'PNG', transparency=3)
add('bw1.png', base.convert('1'), 'PNG')
add('gray16.png', Image.fromarray((np.array(base.convert('L')).astype(np.uint16) * 257)), 'PNG')
add('q4bit.png', base.convert('RGB').quantize(12), 'PNG', bits=4)
add('base420.jpg', base.convert('RGB'), 'JPEG', quality=90)
add('base444.jpg', base.convert('RGB'), 'JPEG', quality=95, subsampling=0)
add('base422.jpg', base.convert('RGB'), 'JPEG', quality=90, subsampling=1)
add('gray.jpg', base.convert('L'), 'JPEG', quality=90)
add('prog420.jpg', base.convert('RGB'), 'JPEG', quality=90, progressive=True)
add('prog444.jpg', base.convert('RGB'), 'JPEG', quality=92, progressive=True, subsampling=0)
add('proggray.jpg', base.convert('L'), 'JPEG', quality=85, progressive=True)
try:
    add('restart.jpg', base.convert('RGB'), 'JPEG', quality=90, restart_marker_blocks=3)
    # the MCU count (11 x 8 = 88) is a multiple of the interval: no RST after the last MCU
    add('restart_exact.jpg', base.convert('RGB'), 'JPEG', quality=90, restart_marker_blocks=4)
    add('restart_prog.jpg', base.convert('RGB'), 'JPEG', quality=90, progressive=True, restart_marker_rows=1)
except Exception as e: print('restart markers not supported by this PIL:', e)
add('rgb24.bmp', base.convert('RGB'), 'BMP')
add('palette.gif', base.convert('RGB').quantize(64), 'GIF')
gt = base.convert('RGB').quantize(32); add('trans.gif', gt, 'GIF', transparency=5)
add('interlaced.gif', base.convert('RGB').quantize(200), 'GIF', interlace=True)
add('pal8.bmp', base.convert('RGB').quantize(50), 'BMP')

# WebP: our decoder must reproduce libwebp (which PIL uses) exactly
photo = Image.new('RGB', (301, 197))
pp = photo.load()
import math, random
random.seed(7)
for y in range(197):
    for x in range(301):
        v = math.sin(x / 17) * 60 + math.cos(y / 11) * 50
        pp[x, y] = (int(128 + v + random.randint(-12, 12)) % 256, (x * 3 + y) % 256, int(100 + v / 2) % 256)
dp = ImageDraw.Draw(photo); dp.ellipse((40, 30, 200, 170), fill=(230, 40, 60)); dp.text((10, 10), 'WebP', fill=(255, 255, 255))
add('lossy_q80.webp', photo, 'WEBP', quality=80)
add('lossy_q10.webp', photo, 'WEBP', quality=10)
add('lossy_q100.webp', photo, 'WEBP', quality=100, method=6)
add('lossy_small.webp', photo.resize((13, 9)), 'WEBP', quality=70)
add('lossy_pattern.webp', base.convert('RGB'), 'WEBP', quality=75)
add('lossy_alpha.webp', base, 'WEBP', quality=80)
add('lossy_alpha_q.webp', base, 'WEBP', quality=60, alpha_quality=50)
add('lossless.webp', photo, 'WEBP', lossless=True)
add('lossless_alpha.webp', base, 'WEBP', lossless=True, exact=True)
add('lossless_m0.webp', base, 'WEBP', lossless=True, method=0, quality=0)
add('lossless_m6.webp', photo, 'WEBP', lossless=True, method=6, quality=100)
add('lossless_pal.webp', base.convert('RGB').quantize(12).convert('RGB'), 'WEBP', lossless=True)
add('lossless_pal2.webp', base.convert('1').convert('RGB'), 'WEBP', lossless=True)
add('lossless_pal4.webp', base.convert('RGB').quantize(4).convert('RGB'), 'WEBP', lossless=True)
frames = [photo, photo.transpose(Image.FLIP_LEFT_RIGHT)]
frames[0].save(f'{out}/anim.webp', 'WEBP', save_all=True, append_images=frames[1:], duration=100, quality=80)
cases.append(('anim.webp', f'{out}/anim.webp', 'WEBP'))

# interlaced PNG via raw writer (PIL can't write Adam7): skip; tested with our own paint encoder elsewhere
# EXIF orientation 6 (rotate 90 CW when displayed)
ex = Image.Exif(); ex[0x0112] = 6
add('exif6.jpg', base.convert('RGB'), 'JPEG', quality=95, exif=ex.tobytes())

# real-world files from the web (tests/samples): WebP must match exactly, JPEG closely
import glob, shutil
for p in sorted(glob.glob('tests/samples/real_*.webp') + glob.glob('tests/samples/real_*.jpg')):
    name = os.path.basename(p)
    shutil.copy(p, f'{out}/{name}')
    cases.append((name, f'{out}/{name}', 'WEBP' if name.endswith('.webp') else 'JPEG'))

bad = 0
for c in cases:
    if c is None: continue
    name, p, fmt = c
    r = subprocess.run([os.path.abspath('bin/imgdump.exe'), p, p + '.rgba'], capture_output=True, text=True)
    if r.returncode:
        print(f'{name:22s} FAIL {r.stdout.strip()}'); bad += 1; continue
    raw = open(p + '.rgba', 'rb').read()
    os.remove(p + '.rgba')  # raw pixels are big; keep build/ small
    hdr, data = raw.split(b'\n', 1)
    w, h = map(int, hdr.split())
    ours = np.frombuffer(data, np.uint8).reshape(h, w, 4).astype(int)
    ref_img = Image.open(p)
    if name == 'exif6.jpg':
        from PIL import ImageOps; ref_img = ImageOps.exif_transpose(ref_img)
    if name == 'gray16.png':  # PIL clips 16-bit gray when converting; the PNG spec scales (high byte)
        g = np.array(ref_img).astype(int) >> 8; ref = np.dstack([g, g, g, np.full_like(g, 255)])
    else:
        ref = np.array(ref_img.convert('RGBA')).astype(int)
    if ref.shape != ours.shape:
        print(f'{name:22s} FAIL size {ours.shape} vs {ref.shape}'); bad += 1; continue
    for a in (ref, ours):  # the colour of a fully transparent pixel does not matter
        a[a[..., 3] == 0] = 0
    diff = np.abs(ref - ours)
    lossy = fmt == 'JPEG'  # WebP must match exactly: both decoders reproduce libwebp
    mean, mx = diff.mean(), diff.max()
    ok = (mean < 1.5 and mx < 40) if lossy else mx == 0
    if not ok: bad += 1
    print(f'{name:22s} {"ok  " if ok else "FAIL"} {w}x{h} mean_diff={mean:.3f} max_diff={mx}')
print('ALL OK' if not bad else f'{bad} FAILED')
