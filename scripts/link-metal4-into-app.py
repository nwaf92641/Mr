#!/usr/bin/env python3
"""Wire graphics/metal4 into the app target, or leave the project untouched.

The app does not link the Metal 4 layer: no mr_metal4 reference exists in
app/Madeira and libmr_metal4.a is not in project.pbxproj. Until that changes, an
IPA on an iPad exercises DXMT's own present path (winemetal_unix.c) and never
touches the layer, so a device run would report nothing about it.

This performs the wiring and refuses to leave a broken project behind: every
anchor must be found exactly once, and the result is checked by
tools/check-xcodeproj.py before it is kept. If the check fails, the project is
restored from git and the script exits non-zero -- a half-wired project is worse
than an unwired one.

Idempotent: running it twice changes nothing.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ROOT = os.path.normpath(ROOT)
PBXPROJ = os.path.join(ROOT, "app", "Madeira.xcodeproj", "project.pbxproj")

# Ids in the project's own scheme: A1... are build files, A2... are file refs.
SOURCE_BUILD = "A10000B0"
SOURCE_REF = "A20000B0"
LIB_BUILD = "A10000B1"
LIB_REF = "A20000B1"

SOURCE_NAME = "Metal4SelfTest.m"
LIB_NAME = "libmr_metal4.a"
# Staged beside libdxmt_unix.a, which the app already links from app/, so the
# IPA pipeline has one place to put built libraries. The reference is relative to
# the group holding the app sources (app/Madeira), so one level up.
LIB_PATH = "../libmr_metal4.a"
HEADER_PATH = "$(SRCROOT)/../graphics/metal4"


def fail(message):
    print("link-metal4-into-app: " + message)
    sys.exit(1)


def restore():
    subprocess.run(["git", "-C", ROOT, "checkout", "--", PBXPROJ], check=False)


def insert_once(text, anchor, addition, what):
    count = text.count(anchor)
    if count != 1:
        fail("expected exactly one %s anchor, found %d" % (what, count))
    return text.replace(anchor, anchor + addition, 1)


with open(PBXPROJ) as handle:
    original = handle.read()

if SOURCE_BUILD in original:
    print("link-metal4-into-app: already wired, nothing to do")
    sys.exit(0)

text = original

# 1. Build-file entries, next to the other entries in that section.
text = insert_once(
    text,
    "/* Begin PBXBuildFile section */\n",
    "            %s /* %s in Sources */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };\n"
    "            %s /* %s in Frameworks */ = {isa = PBXBuildFile; fileRef = %s /* %s */; };\n"
    % (SOURCE_BUILD, SOURCE_NAME, SOURCE_REF, SOURCE_NAME,
       LIB_BUILD, LIB_NAME, LIB_REF, LIB_NAME),
    "PBXBuildFile",
)

# 2. File references, next to the app's own sources and libraries.
refs = ("            %s /* %s */ = {isa = PBXFileReference; lastKnownFileType = sourcecode.c.objc; "
        "path = %s; sourceTree = \"<group>\"; };\n"
        "            %s /* %s */ = {isa = PBXFileReference; lastKnownFileType = archive.ar; "
        "name = %s; path = \"../../%s\"; sourceTree = \"<group>\"; };\n"
        % (SOURCE_REF, SOURCE_NAME, SOURCE_NAME, LIB_REF, LIB_NAME, LIB_NAME, LIB_PATH))
text = insert_once(text, "/* Begin PBXFileReference section */\n", refs, "PBXFileReference")

# 3. The source into the compile phase, the library into the link phase. Both
#    phases end their file list the same way, so each is located by its own isa.
def add_to_phase(text, isa, entry, what):
    match = re.search(r"isa = %s;.*?files = \((.*?)\);" % isa, text, re.S)
    if match is None:
        fail("could not find the %s files list" % what)
    body = match.group(1)
    end = match.end(1)
    return text[:end] + entry + text[end:]


text = add_to_phase(text, "PBXSourcesBuildPhase", "\n                    %s /* %s in Sources */,"
                     % (SOURCE_BUILD, SOURCE_NAME), "sources")
text = add_to_phase(text, "PBXFrameworksBuildPhase", "\n                    %s /* %s in Frameworks */,"
                     % (LIB_BUILD, LIB_NAME), "frameworks")

# 4. Both new files into the group the app's other sources live in, found by
#    looking for the group that already holds MadeiraApp.swift.
group_match = re.search(r"([0-9A-F]{8}) /\* ([A-Za-z]+) \*/ = \{\s*isa = PBXGroup;\s*children = \((.*?)\);",
                        text, re.S)
target_group = None
for candidate in re.finditer(
        r"([0-9A-F]{8}) /\* ([A-Za-z]+) \*/ = \{\s*isa = PBXGroup;\s*children = \((.*?)\);", text, re.S):
    if "MadeiraApp.swift" in candidate.group(3):
        target_group = candidate
        break
if target_group is None:
    fail("could not find the group holding the app sources")
entry = "\n                    %s /* %s */,\n                    %s /* %s */," % (
    SOURCE_REF, SOURCE_NAME, LIB_REF, LIB_NAME)
text = text[:target_group.end(3)] + entry + text[target_group.end(3):]

# 5. Search paths, in every build configuration, so the header and the archive
#    are found. Anchored on the setting name rather than on an indentation, which
#    is what the first attempt got wrong: the anchor did not match and the script
#    stopped before writing anything.
def add_search_path(text, setting, entry):
    pattern = r"(%s = \(\n)" % setting
    result, count = re.subn(pattern, lambda m: m.group(1) + '                                    "%s",\n' % entry, text)
    if count == 0:
        fail("no %s found in any configuration" % setting)
    print("link-metal4-into-app: %s += %s (%d configuration(s))" % (setting, entry, count))
    return result


text = add_search_path(text, "HEADER_SEARCH_PATHS", HEADER_PATH)
text = add_search_path(text, "LIBRARY_SEARCH_PATHS", "$(SRCROOT)")

with open(PBXPROJ, "w") as handle:
    handle.write(text)

check = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "check-xcodeproj.py")],
                       capture_output=True, text=True)
if check.returncode != 0:
    print(check.stdout)
    print(check.stderr)
    restore()
    fail("check-xcodeproj.py rejected the result; project restored from git")

print("link-metal4-into-app: wired and check-xcodeproj.py accepts it")
print("link-metal4-into-app: %s -> Sources, %s -> Frameworks" % (SOURCE_NAME, LIB_NAME))
print("link-metal4-into-app: header search path %s" % HEADER_PATH)
print("link-metal4-into-app: the IPA pipeline must stage %s into app/ before Xcode runs" % LIB_NAME)
