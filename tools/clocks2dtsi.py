#!/usr/bin/env python3
"""Turn the vendor DT's clocks{} node (decompiled from the VT10101's dt.img)
into kernel/sc7731g-clocks.dtsi: labels instead of phandle
numbers, our own compatibles (sprd,sc7731g-*), MM-domain clocks left out."""
import re, sys
src, first, last = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
lines = open(src).read().split('\n')[first-1:last]
nodes, cur = [], None
for l in lines[4:]:
    m = re.match(r'^\t\t(\w+) \{$', l)
    if m: cur = {'name': m.group(1), 'props': []}; continue
    if re.match(r'^\t\t\};$', l): nodes.append(cur); cur = None; continue
    if cur is not None: cur['props'].append(l.strip())
ph = {}
for n in nodes:
    for p in n['props']:
        m = re.match(r'^phandle = <(0x[0-9a-f]+)>;', p)
        if m: ph[int(m.group(1), 16)] = n['name']
skip = {n['name'] for n in nodes if 'mm-domain;' in n['props']}
# a clock whose registers live in a block that can be powered down: reading
# it at registration with the block off can hang the bus
skip |= {n['name'] for n in nodes if any(re.search(r'reg = <0x6[0-9a-f]{7}', p) for p in n['props'])}
out = []
for n in nodes:
    if n['name'] in skip: continue
    lab = n['name'].replace('-', '_')
    out.append(f"\t\t{lab}: {n['name'].replace('_', '-')} {{")
    for p in n['props']:
        if p.startswith(('linux,phandle', 'phandle')): continue
        p = re.sub(r'compatible = "sprd,([\w-]+)"', r'compatible = "sprd,sc7731g-\1"', p)
        m = re.match(r'^clocks = <(.*)>;$', p)
        if m:
            refs = [ph[int(x, 16)] for x in m.group(1).split()]
            bad = [r for r in refs if r in skip]
            # a mux's parent index is its position in this list: dropping
            # one would shift the rest and the driver would read and set
            # the wrong parent
            if bad and len(refs) > 1:
                sys.exit(f"{n['name']}: mux parents left out ({' '.join(bad)}); keep them or leave the mux out")
            p = 'clocks = <' + ' '.join('&' + r for r in refs if r not in skip) + '>;'
        out.append('\t\t\t' + p)
    out.append('\t\t};\n')
print('\n'.join(out))
print('/* left out: ' + ' '.join(sorted(skip)) + ' */', file=sys.stderr)
