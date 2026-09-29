# HPACK (HTTP/2 header compression): checks the browser's decoder against every example in
# RFC 7541 Appendix C (requests and responses, with and without Huffman coding, with the
# dynamic table filling up and evicting), the static table against Appendix A, and that
# what the browser encodes decodes back to the same headers.
#   python tests/check_hpack.py [rfc7541.txt]   (downloads the RFC if no file is given)
import os, re, subprocess, sys, urllib.request

if len(sys.argv) > 1:
    rfc = open(sys.argv[1], encoding='utf-8').read()
else:
    rfc = urllib.request.urlopen('https://www.rfc-editor.org/rfc/rfc7541.txt').read().decode()
# drop the page breaks (footer, form feed, header) so that sections read continuously
rfc = re.sub(r'\n\n+Peon & Ruellan[^\n]*\n\f?\n?RFC 7541[^\n]*\n\n', '\n', rfc)

cmds, expect, names = [], [], []

# Appendix A: indexed field 0x80|i on a fresh decoder gives static entry i
table = re.findall(r'\|\s*(\d+)\s*\|\s*(\S+)\s*\|\s*(.*?)\s*\|', rfc[rfc.index('Appendix A.  Static Table Definition'):rfc.index('Appendix B.  Huffman Code')])
assert len(table) == 61, len(table)
for i, name, value in table:
    cmds += ['table 4096', 'hex %02x' % (0x80 | int(i))]
    expect.append([f'{name}: {value}'])
    names.append(f'static table entry {i}')

# Appendix C.3 - C.6: consecutive header blocks on one connection
for sec, size, nxt in (('C.3', 4096, '\nC.4.  '), ('C.4', 4096, '\nC.5.  '), ('C.5', 256, '\nC.6.  '), ('C.6', 256, '\nAcknowledgments')):
    start = rfc.index(f'\n{sec}.  ')
    end = rfc.index(nxt, start)
    body = rfc[start:end]
    cmds.append(f'table {size}')
    for m in re.finditer(r'\n(C\.\d\.\d)\.  [^\n]*\n(.*?)(?=\nC\.\d\.\d\.  |\Z)', body, re.S):
        part = m.group(2)
        dump = part[part.index('Hex dump of encoded data:'):part.index('Decoding process:')]
        hexs = ''.join(re.findall(r'^\s{3}([0-9a-f ]+?)\s*\|', dump, re.M)).replace(' ', '')
        decoded = part[part.index('Decoded header list:') + len('Decoded header list:'):]
        lines = [l.strip() for l in decoded.strip('\n').split('\n')]
        lines = lines[:lines.index('')] if '' in lines else lines
        cmds.append('hex ' + hexs)
        expect.append(lines)
        names.append(f'RFC 7541 {m.group(1)}')

# round trip through our encoder
trip = [':method: GET', ':scheme: https', ':authority: th.wikipedia.org', ':path: /wiki/%E0%B8%81?a=1&b=2',
        'user-agent: Low-web/0.1', 'cookie: a=1; b=' + 'x' * 300, 'if-none-match: "W/abc"']
cmds += ['encode'] + trip + ['']
expect.append(trip)
names.append('encode, then decode')

r = subprocess.run([os.path.abspath('bin/hpacktest.exe')], input='\n'.join(cmds) + '\n', capture_output=True, text=True)
blocks, cur = [], []
for l in r.stdout.split('\n'):
    if l == '--' or l == 'ERROR':
        blocks.append(cur if l == '--' else ['ERROR'])
        cur = []
    elif l:
        cur.append(l)
failed = 0
for name, want, got in zip(names, expect, blocks + [None] * (len(names) - len(blocks))):
    ok = got == want
    failed += not ok
    if not ok or not name.startswith('static'):
        print(f'{"ok  " if ok else "FAIL"} {name}' + ('' if ok else f'\n   want {want}\n   got  {got}'))
print(f'ok   static table: {sum(1 for n in names if n.startswith("static"))} entries checked' if not failed else '')
print('ALL OK' if not failed else f'{failed} FAILED')
sys.exit(1 if failed else 0)
