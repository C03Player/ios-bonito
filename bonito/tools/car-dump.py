#!/usr/bin/env python3
"""Dump the facet names and rendition keys of an Apple asset catalog (.car, BOM format), read-only.

  car-dump.py Assets.car [NAME-SUBSTRING]
Prints, per matching facet, the facet attributes and every rendition key (attribute=value) that carries its identifier.
"""
import struct, sys
ATTR = {1:'element',2:'part',3:'size',4:'direction',5:'placement',6:'value',7:'appearance',8:'dimension1',9:'dimension2',
        10:'state',11:'layer',12:'scale',13:'localization',14:'presentationState',15:'idiom',16:'subtype',17:'identifier',
        18:'previousValue',19:'previousState',20:'sizeClassH',21:'sizeClassV',22:'memoryClass',23:'graphicsClass',
        24:'displayGamut',25:'deploymentTarget'}
d = open(sys.argv[1], 'rb').read()
want = sys.argv[2] if len(sys.argv) > 2 else ''
assert d[:8] == b'BOMStore'
nblocks, idx_off, idx_len, vars_off, vars_len = struct.unpack('>5I', d[12:32])
idx = struct.unpack_from('>I', d, idx_off)[0]
blocks = [struct.unpack_from('>II', d, idx_off + 4 + 8 * i) for i in range(idx)]
def blk(i):
    o, l = blocks[i]; return d[o:o + l]
count = struct.unpack_from('>I', d, vars_off)[0]; p = vars_off + 4; vs = {}
for _ in range(count):
    bi, nl = struct.unpack_from('>IB', d, p); vs[d[p + 5:p + 5 + nl].decode()] = bi; p += 5 + nl
def tree(name):
    t = blk(vs[name]); assert t[:4] == b'tree'
    child = struct.unpack('>I', t[8:12])[0]
    out = []
    def walk(bi):
        n = blk(bi); leaf, cnt, fwd, bwd = struct.unpack('>HHII', n[:12])
        ents = [struct.unpack('>II', n[12 + 8 * i:20 + 8 * i]) for i in range(cnt)]
        if leaf:
            for vi, ki in ents: out.append((blk(ki), blk(vi)))
        else:
            for vi, ki in ents: walk(vi)
    walk(child); return out
kf = blk(vs['KEYFORMAT']); assert kf[:4] == b'tmfk'
ntok = struct.unpack('<I', kf[8:12])[0]; toks = list(struct.unpack('<%dI' % ntok, kf[12:12 + 4 * ntok]))
print('key format tokens:', [ATTR.get(t, t) for t in toks])
facets = {}
for k, v in tree('FACETKEYS'):
    name = k.decode('latin1').rstrip('\0')
    # value: hot(u16) count(u16) then (attr u16, value u16) pairs
    cnt = struct.unpack('<HH', v[:4])[1] if len(v) >= 4 else 0
    pairs = {ATTR.get(a, a): val for a, val in (struct.unpack('<HH', v[4 + 4 * i:8 + 4 * i]) for i in range(cnt))}
    facets[name] = pairs
rend = []
for k, v in tree('RENDITIONS'):
    vals = struct.unpack('<%dH' % ntok, k[:2 * ntok])
    rend.append({ATTR.get(t, t): x for t, x in zip(toks, vals)})
print('facets', len(facets), 'renditions', len(rend))
for name, pairs in sorted(facets.items()):
    if want not in name: continue
    ident = pairs.get('identifier')
    rs = [r for r in rend if r.get('identifier') == ident]
    print(f'\n{name}  facet={pairs}  renditions={len(rs)}')
    for r in rs: print('    ', {a: x for a, x in r.items() if x})
