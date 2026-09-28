#!/usr/bin/env python3
# Recovers the retail Tribes 2 class hierarchy from the stripped Linux
# binary's RTTI. egcs 1.1.2 builds each typeinfo node at run time in
# __tf<Class>(): push base node; push "<len><Name>"; push node (or a register
# loaded with mov $node, %reg); call __rtti_si. Writes {class: parent} JSON.
# Usage: retail_rtti_classes.py out.json [Class ...]   (prints each chain)
import struct, re, sys, json
data = open(__import__('os').environ.get('T2_BINARY', '/home/methodown/t2-linux/tribes2'),'rb').read()
e_phoff, = struct.unpack_from('<I', data, 0x1c)
e_phentsize, e_phnum = struct.unpack_from('<HH', data, 0x2a)
loads = []
for i in range(e_phnum):
    p = struct.unpack_from('<8I', data, e_phoff + i*e_phentsize)
    if p[0] == 1: loads.append((p[2], p[1], p[4]))
def off2va(o):
    for va, off, sz in loads:
        if off <= o < off + sz: return va + (o - off)
names = {}
for m in re.finditer(rb'\x00([0-9]{1,3})([A-Za-z_][A-Za-z0-9_]*)\x00', data):
    ident = m.group(2)
    if len(ident) != int(m.group(1)): continue
    names[off2va(m.start(1))] = ident.decode()
node_name = {}   # typeinfo node va -> class name
node_base = {}   # node va -> base node va (single inheritance)
for nva, name in names.items():
    pat = b'\x68' + struct.pack('<I', nva)
    for m in re.finditer(re.escape(pat), data):
        o = m.start()
        # __rtti_si(node, name, base): push base; push name; push node.
        node = None
        start = data.rfind(b'\x55\x89\xe5', max(o - 400, 0), o)
        nxt = data[o + 5]
        if nxt == 0x68:
            node = struct.unpack_from('<I', data, o + 6)[0]
        elif 0x50 <= nxt <= 0x57:
            # push %reg: the node was loaded with mov $node, %reg
            op = 0xb8 + (nxt - 0x50)
            for k in range(o - 1, max(start, 0), -1):
                if data[k] == op:
                    node = struct.unpack_from('<I', data, k + 1)[0]; break
        base = None
        if o >= 5 and data[o - 5] == 0x68:
            base = struct.unpack_from('<I', data, o - 4)[0]
        if node is not None:
            node_name[node] = name
            if base is not None: node_base[node] = base
parent = {}
for node, name in node_name.items():
    b = node_base.get(node)
    if b in node_name and node_name[b] != name: parent[name] = node_name[b]
json.dump(parent, open(sys.argv[1], 'w'), indent=0, sort_keys=True)
print(len(node_name), 'nodes', len(parent), 'parents')
for k in sys.argv[2:]:
    chain = [k]
    while chain[-1] in parent and len(chain) < 25: chain.append(parent[chain[-1]])
    print(' -> '.join(chain))
