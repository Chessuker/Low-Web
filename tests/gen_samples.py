# Makes the sample files in tests/samples that stand in for files from the web (gen_*): made
# here from pictures drawn here, so they belong to this project, but written by the same real
# encoders (libjpeg, libwebp) with the features real files have: camera-style JPEGs (4:2:0,
# restart markers, EXIF, XMP, ICC, sizes that aren't whole blocks), lossy, lossless and
# lossy-with-alpha WebP, and SVGs written the ways drawing tools write them.
#   python tests/gen_samples.py      (needs Pillow and numpy; writes tests/samples/gen_*)
import math, os, random
import numpy as np
from PIL import Image, ImageCms, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'samples')
rnd = random.Random(7)


def photo(w, h, seed):
    """Something like a photograph: sky, hills, a sun, leaves, and sensor noise."""
    r = np.random.default_rng(seed)
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    img = np.zeros((h, w, 3), np.float32)
    sky = y / h
    img[..., 0] = 90 + 120 * sky
    img[..., 1] = 140 + 80 * sky
    img[..., 2] = 230 - 40 * sky
    for k in range(3):  # hills, the nearer the darker and greener
        top = h * (0.45 + 0.12 * k) + h * 0.06 * np.sin(x / w * (3 + 2 * k) * math.pi + seed + k)
        land = y > top
        shade = 1 - 0.25 * k
        img[land] = np.stack([60 * shade + 0 * x, 120 * shade + 30 * np.sin(x / 37 + y / 23), 50 * shade + 0 * x], -1)[land]
    sun = (x - w * 0.72) ** 2 + (y - h * 0.22) ** 2 < (min(w, h) * 0.08) ** 2
    img[sun] = (255, 236, 160)
    im = Image.fromarray(np.clip(img + r.normal(0, 6, img.shape), 0, 255).astype(np.uint8))
    d = ImageDraw.Draw(im)
    for _ in range(140):  # leaves in front
        cx, cy, s = r.uniform(0, w), r.uniform(h * 0.6, h), r.uniform(4, 22)
        g = int(r.uniform(70, 170))
        d.ellipse((cx - s, cy - s / 2, cx + s, cy + s / 2), fill=(30, g, 40))
    return im


def icc():
    return ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes()


def exif(model, dpi):
    e = Image.Exif()
    e[0x010F] = 'Low-web'          # Make
    e[0x0110] = model              # Model
    e[0x0131] = 'tests/gen_samples.py'  # Software
    e[0x0132] = '2026:01:01 12:00:00'
    e[0x011A] = e[0x011B] = dpi    # X/YResolution
    return e.tobytes()


# camera-style JPEGs: 4:2:0, restart markers, EXIF (+ XMP), ICC; heights not a multiple of 16
photo(800, 801, 1).save(os.path.join(OUT, 'gen_photo1.jpg'), 'JPEG', quality=88, subsampling=2, restart_marker_blocks=50,
                        exif=exif('Camera One', 72), icc_profile=icc(),
                        xmp=b'<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">'
                            b'<rdf:Description/></rdf:RDF></x:xmpmeta>')
photo(1395, 1613, 2).save(os.path.join(OUT, 'gen_photo2.jpg'), 'JPEG', quality=82, subsampling=2, restart_marker_blocks=88,
                          exif=exif('Camera Two', 144), icc_profile=icc(), dpi=(144, 144))

# WebP: lossy (two settings), lossless with alpha, lossy with alpha (VP8X + ALPH)
photo(550, 368, 3).save(os.path.join(OUT, 'gen_1.webp'), 'WEBP', quality=75, method=4)
photo(800, 533, 4).save(os.path.join(OUT, 'gen_3.webp'), 'WEBP', quality=60, method=6)


def cutout(w, h, seed):
    """A photo in a rounded shape with soft edges, on transparency (as product shots are)."""
    im = photo(w, h, seed).convert('RGBA')
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    dist = np.sqrt(((x - w / 2) / (w * 0.45)) ** 4 + ((y - h / 2) / (h * 0.45)) ** 4) ** 0.5
    alpha = np.clip((1.05 - dist) * 8, 0, 1) * 255
    im.putalpha(Image.fromarray(alpha.astype(np.uint8)))
    return im


cutout(400, 301, 5).save(os.path.join(OUT, 'gen_1_ll.webp'), 'WEBP', lossless=True, quality=80)
cutout(386, 395, 6).save(os.path.join(OUT, 'gen_2_alpha.webp'), 'WEBP', quality=70, alpha_quality=100)

# SVGs, written as tools write them
with open(os.path.join(OUT, 'gen_logo1.svg'), 'w', encoding='utf-8', newline='\n') as f:  # one line, offset viewBox, relative h/v/l
    f.write('<svg height="18" viewBox="4 4 188 188" width="18" xmlns="http://www.w3.org/2000/svg"><path d="m4 4h188v188h-188z" '
            'fill="#2a7"/><path d="m60.5 50.25h75.125v19.875h-52.5v16.25h44.375v19.75h-44.375v39.625h-22.625z" fill="#fff"/></svg>')
with open(os.path.join(OUT, 'gen_logo2.svg'), 'w', encoding='utf-8', newline='\n') as f:  # relative c with packed numbers (-.5, 1.2.3)
    f.write('<?xml version="1.0" encoding="utf-8"?>\n<svg xmlns="http://www.w3.org/2000/svg" xml:space="preserve" width="97" height="97">\n'
            '  <path fill="#3B6FD8" d="M48.5 4.3c2.3 0 4.4.9 5.9 2.4l36.3 36.3c3.2 3.2 3.2 8.5 0 11.8L54.4 91.1c-3.2 3.2-8.5 3.2-11.8 0'
            'L6.3 54.8c-3.2-3.3-3.2-8.6 0-11.8L42.6 6.7c1.5-1.5 3.6-2.4 5.9-2.4zm0 20.4c-13.2 0-23.8 10.6-23.8 23.8s10.6 23.8 23.8 23.8'
            ' 23.8-10.6 23.8-23.8-10.6-23.8-23.8-23.8zm0 9.5c7.9 0 14.3 6.4 14.3 14.3S56.4 62.8 48.5 62.8 34.2 56.4 34.2 48.5s6.4-14.3'
            ' 14.3-14.3z"/>\n</svg>\n')


def num(v):
    s = ('%.3f' % v).rstrip('0').rstrip('.')
    return s.replace('0.', '.', 1) if s.startswith('0.') else s.replace('-0.', '-.', 1)


# A big drawing like the classic test illustrations: a group with a matrix, stroked and
# filled paths that inherit from it, relative curves (c, s), lines (l, v), moves (m) and z.
paths = []
for i in range(220):
    a = i * 0.29
    rad = 20 + i * 1.6
    x, y = 450 + rad * math.cos(a), 450 + rad * math.sin(a)
    d = 'M%s %s' % (num(x), num(y))
    for k in range(3):
        d += 'c%s %s %s %s %s %s' % (num(rnd.uniform(-30, 30)), num(rnd.uniform(-30, 30)), num(rnd.uniform(-40, 40)),
                                     num(rnd.uniform(-40, 40)), num(rnd.uniform(-25, 25)), num(rnd.uniform(-25, 25)))
    d += 's%s %s %s %s' % (num(rnd.uniform(-20, 20)), num(rnd.uniform(-20, 20)), num(rnd.uniform(-15, 15)), num(rnd.uniform(-15, 15)))
    d += 'l%s %s' % (num(rnd.uniform(-10, 10)), num(rnd.uniform(-10, 10)))
    if i % 3 == 0:
        d += 'v%s' % num(rnd.uniform(-12, 12))
    if i % 5 == 0:
        d += 'm%s %s l%s %s' % (num(rnd.uniform(-5, 5)), num(rnd.uniform(-5, 5)), num(rnd.uniform(-8, 8)), num(rnd.uniform(-8, 8)))
    d += 'z'
    hue = i * 7 % 360
    col = '#%02x%02x%02x' % tuple(int(127 + 120 * math.cos(math.radians(hue + o))) for o in (0, 120, 240))
    attrs = ' fill="%s"' % col if i % 4 else ' stroke="#000" stroke-width="%s"' % num(0.3 + i % 7 * 0.2)
    paths.append('   <path id="p%d" d="%s"%s/>' % (i, d, attrs))
with open(os.path.join(OUT, 'gen_drawing.svg'), 'w', encoding='utf-8', newline='\n') as f:
    f.write('<?xml version="1.0" encoding="UTF-8" standalone="no"?>\n'
            '<svg id="svg2" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 900 900" version="1.1">\n'
            ' <g id="g4" fill="none" transform="matrix(0.9,0.12,-0.12,0.9,90,-20)">\n'
            '  <g id="g6" stroke-width="0.172" stroke="#000" fill="#FFF">\n' + '\n'.join(paths) + '\n  </g>\n </g>\n</svg>\n')

for f in sorted(os.listdir(OUT)):
    if f.startswith('gen_'):
        print('%-20s %7d bytes' % (f, os.path.getsize(os.path.join(OUT, f))))
