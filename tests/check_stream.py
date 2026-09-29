# Streaming decompression (deflate::Stream, used to show pages while they download):
# compresses test data every way servers do (gzip, zlib, raw deflate, stored blocks, big
# single blocks) and has bin/inflatetest.exe decode it from random-sized pieces.
import gzip, os, random, subprocess, sys, zlib

out = 'build/streamtest'
os.makedirs(out, exist_ok=True)
rnd = random.Random(7)
words = b'lorem ipsum dolor sit amet <div class="x">&amp; \xe0\xb8\x81\xe0\xb8\xa3\xe0\xb8\xb8\xe0\xb8\x87 </div>'.split(b' ')
text = b' '.join(rnd.choice(words) for _ in range(300000))
noise = bytes(rnd.getrandbits(8) for _ in range(200000))
data = {'text': text, 'noise': noise, 'mixed': text[:300000] + noise[:100000] + text[:400000], 'tiny': b'hi', 'empty': b''}

cases = []
for name, d in data.items():
    open(f'{out}/{name}.orig', 'wb').write(d)
    def w(suffix, blob, kind):
        path = f'{out}/{name}.{suffix}'
        open(path, 'wb').write(blob)
        cases.append((path, f'{out}/{name}.orig', kind))
    w('gz', gzip.compress(d, 6), 0)
    w('stored.gz', gzip.compress(d, 0), 0)
    w('zlib', zlib.compress(d, 9), 1)
    c = zlib.compressobj(1, zlib.DEFLATED, -15)
    w('raw', c.compress(d) + c.flush(), 1)
    c = zlib.compressobj(9, zlib.DEFLATED, 31, 9)  # gzip with large blocks, like some servers
    w('bigblock.gz', c.compress(d) + c.flush(), 0)

failed = 0
for path, orig, kind in cases:
    for seed in (1, 2, 3):
        r = subprocess.run([os.path.abspath('bin/inflatetest.exe'), path, orig, str(kind), str(seed)], capture_output=True, text=True)
        if r.returncode:
            failed += 1
            print(r.stdout.strip() or r.stderr.strip())
        elif seed == 1:
            print(r.stdout.strip())
print('ALL OK' if not failed else f'{failed} FAILED')
if not failed:  # the test data is made again each run
    import shutil
    shutil.rmtree(out)
sys.exit(1 if failed else 0)
