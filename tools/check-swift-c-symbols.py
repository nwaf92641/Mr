#!/usr/bin/env python3
"""Fail when Swift calls a C function the compiler cannot see.

Swift reaches this project's C only through app/Madeira/Madeira-Bridging-Header.h
and the headers it imports. A call to a C function that no visible header
declares is a build failure, but not one anything in the tree could catch:
check-swift-syntax.sh parses (it is `swiftc -parse`, which does not resolve
names), and there is no compiler for the app off a Mac with the iOS SDK.

ml808 hit it twice on the first CI run that compiled the app: XInputBridge.swift
called madeira_xinput_publish/clear/set_rumble_handler, MadeiraXInput.h was not
imported by the bridging header, and the only signal was an xcodebuild log
twenty minutes into a full FEX + Wine + LLVM + DXMT pipeline.

Scope is deliberately narrow: only identifiers declared in this repository's own
headers under app/Madeira, called from Swift. That is exactly the set the
bridging header has to cover. System symbols (task_info, vm_allocate,
mach_make_memory_entry_64) come from SDK headers the project's headers import,
and C type names used as constructors (vm_size_t) are not functions; both are
left alone, which is why the check reads declarations rather than trying to
resolve what a header pulls in.

Usage: tools/check-swift-c-symbols.py
Exit 0 when every call is declared, 1 with the list when one is not.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app', 'Madeira')
BRIDGE = os.path.join(APP, 'Madeira-Bridging-Header.h')

SWIFT = ('.swift',)
HEADER = ('.h',)


def read(path):
    with open(path, encoding='utf-8', errors='replace') as handle:
        return handle.read()


def files_with(suffixes, directory=APP):
    for name in sorted(os.listdir(directory)):
        if name.endswith(suffixes):
            yield os.path.join(directory, name)


def our_headers():
    """Every header the project owns, including subdirectories like Winios/."""
    for root, _, names in os.walk(APP):
        for name in sorted(names):
            if name.endswith(HEADER):
                yield os.path.join(root, name)


def declared_names():
    """Identifiers this project's headers declare.

    Read as text, not parsed: a declaration is the name followed by '(' or
    preceded by 'typedef'/'extern', and the check only needs to know the name is
    ours. Names without an underscore are left out -- they cannot be told from
    Swift's own (print, fputs), and the project's C API is underscored.
    """
    names = set()
    for path in our_headers():
        for match in re.finditer(r'\b([a-zA-Z_][a-zA-Z0-9_]*)\s*\(', read(path)):
            name = match.group(1)
            if '_' in name:
                names.add(name)
    return names


def swift_c_calls():
    """Swift calls that look like C: an underscored name, not after a dot.

    `frame.winios_post_key(` would be a method, not the C function, so the dot
    is excluded. Snake_case members of our own Swift types do not exist; the
    rest of the app is camelCase.

    `{` is included as a call shape because a C function taking a callback is
    called with a trailing closure -- madeira_xinput_set_rumble_handler { } --
    and that was one of the symbols CI could not resolve.
    """
    calls = {}
    for path in files_with(SWIFT):
        text = read(path)
        pattern = r'(?<![\w.])([a-z][a-zA-Z0-9]*_[a-zA-Z0-9_]*)\s*[({]'
        for match in re.finditer(pattern, text):
            calls.setdefault(match.group(1), set()).add(os.path.basename(path))
    return calls


def visible_names():
    """Identifiers declared in the bridging header and the headers it imports."""
    text = read(BRIDGE)
    visible = set(re.findall(r'[a-zA-Z_][a-zA-Z0-9_]*', text))
    for match in re.finditer(r'^[ \t]*#\s*(?:import|include)\s+"([^"]+)"', text, re.M):
        header = os.path.join(APP, match.group(1))
        if not header.endswith(HEADER):
            header += '.h'
        if os.path.isfile(header):
            visible.update(re.findall(r'[a-zA-Z_][a-zA-Z0-9_]*', read(header)))
    return visible


def main():
    if not os.path.isfile(BRIDGE):
        print('check-swift-c-symbols: no bridging header at %s' % BRIDGE)
        return 1

    ours = declared_names()
    visible = visible_names()
    hidden = {}
    for name, sources in swift_c_calls().items():
        if name in ours and name not in visible:
            hidden[name] = sources

    if hidden:
        print('check-swift-c-symbols: FAIL -- Swift calls %d C symbol(s) no '
              'Swift-visible header declares:' % len(hidden))
        for name in sorted(hidden):
            where = ', '.join(sorted(hidden[name]))
            print('  %s (%s)' % (name, where))
        print('  Add the header to app/Madeira/Madeira-Bridging-Header.h, or '
              'declare the function there.')
        return 1

    print('check-swift-c-symbols: OK -- every project C call in Swift is '
          'declared in the bridging header or a header it imports')
    return 0


if __name__ == '__main__':
    sys.exit(main())
