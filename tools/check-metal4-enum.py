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

ROOT = sys.argv[1] if len(sys.argv) > 1 else '.'
strict = '--strict' in sys.argv

header = open(ROOT + '/research/dxmt/src/winemetal/winemetal.h').read()
bridge = open(ROOT + '/graphics/metal4/mr_metal4_winemetal.mm').read()

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
