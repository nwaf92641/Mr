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

missing = [n for n in names if ('case ' + n + ':') not in bridge]
translated = len(names) - len(missing)

print('check-metal4-enum: %d of %d render command types translated' % (translated, len(names)))
for name in missing:
    print('check-metal4-enum:   not translated: %s (id %d)' % (name, names.index(name)))

if missing and strict:
    sys.exit('check-metal4-enum: %d untranslated' % len(missing))
