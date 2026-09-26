#!/usr/bin/env python3
"""Check every WMTRenderCommandType against the Metal 4 bridge's switch.

Claiming D3D11 is complete needs this to come back empty, and it is a check that
has to be mechanical: the Metal 3 walk counts its gaps at run time, which only
helps once something runs, and reading the enum by eye is how "38" got quoted
from a partial listing.

Exits non-zero when something is untranslated, so it can gate CI as soon as the
list is empty and report in the meantime.
"""
import re
import sys

ROOT = '.'
strict = False
for index, arg in enumerate(sys.argv):
    if arg == '--root' and index + 1 < len(sys.argv):
        ROOT = sys.argv[index + 1]
    if arg == '--strict':
        strict = True
# Explicit paths, because the DXMT submodule only exists in CI: a local run can
# point the header at a checkout elsewhere and still check this tree's bridge.
header_path = ROOT + '/research/dxmt/src/winemetal/winemetal.h'
bridge_path = ROOT + '/graphics/metal4/mr_metal4_winemetal.mm'
for index, arg in enumerate(sys.argv):
    if arg == '--header' and index + 1 < len(sys.argv):
        header_path = sys.argv[index + 1]
    if arg == '--bridge' and index + 1 < len(sys.argv):
        bridge_path = sys.argv[index + 1]

header = open(header_path).read()
bridge = open(bridge_path).read()

block = re.search(r'enum WMTRenderCommandType\s*:\s*\w+\s*\{(.*?)\}', header, re.S)
if block is None:
    sys.exit('WMTRenderCommandType not found in winemetal.h')

# Values are implicit and sequential, so the position is the id the bridge sees.
names = []
for entry in block.group(1).split(','):
    name = entry.split('=')[0].strip()
    if name and not name.startswith('//'):
        names.append(name)

# Unused<N> are ids DXMT reserves rather than opcodes it sends, so they are not
# missing translations and must not keep the gate red.
commands = [n for n in names if not re.fullmatch(r'Unused\d+', n)]
missing = [n for n in commands if ('case ' + n + ':') not in bridge]
translated = len(commands) - len(missing)
reserved = len(names) - len(commands)

print('check-metal4-enum: %d of %d render command types translated (%d reserved ids)'
      % (translated, len(commands), reserved))
for name in missing:
    print('check-metal4-enum:   not translated: %s (id %d)' % (name, names.index(name)))

def scoped(enum_name, signature, label):
    block = re.search(r'enum ' + enum_name + r'\s*:\s*\w+\s*\{(.*?)\}', header, re.S)
    if block is None:
        sys.exit(enum_name + ' not found in winemetal.h')
    names = [e.split('=')[0].strip() for e in block.group(1).split(',')]
    names = [n for n in names if n and not n.startswith('//')]
    start = bridge.find(signature)
    if start == -1:
        sys.exit('walker not found for ' + label + ': ' + signature)
    body = bridge[start:start + 20000]
    gone = [n for n in names if ('case ' + n + ':') not in body]
    print('check-metal4-enum: %s: %d of %d translated' % (label, len(names) - len(gone), len(names)))
    for name in gone:
        print('check-metal4-enum:   %s not translated: %s' % (label, name))
    return gone

blit_missing = scoped('WMTBlitCommandType', 'bool blit_one(', 'blit')
compute_missing = scoped('WMTComputeCommandType', 'uint32_t mr_mtl4_wmt_encode_compute(', 'compute')
missing = missing + blit_missing + compute_missing

if missing and strict:
    sys.exit('check-metal4-enum: %d untranslated across render, blit and compute' % len(missing))
