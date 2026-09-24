#!/usr/bin/env python3
"""Minimal PE/COFF reader: machine word, exports and imports.

The checks in this directory need to ask questions about PE modules that a
header field cannot answer -- "does this module export the symbol an aliased
generation was linked against?", "what does this module import that the bundle
does not ship?". pefile answers both, but it is a third-party dependency and the
release gates run on a runner that only has the Xcode toolchain and python3, so
this module reads the few structures involved directly.

Only what the gates use is implemented: the optional header's machine word and
data directories, the export name table, and the import descriptor names. Parsing
is bounds-checked and raises PEFormatError rather than reading past the end --
a malformed or truncated DLL must fail a gate loudly, not silently report zero
exports, which would make an alias look verified when it is not.
"""

from __future__ import annotations

import struct
from pathlib import Path

IMAGE_DIRECTORY_ENTRY_EXPORT = 0
IMAGE_DIRECTORY_ENTRY_IMPORT = 1

MACHINE_NAMES = {
    0x014C: "i386",
    0x8664: "amd64/arm64ec",
    0xAA64: "arm64",
    0x01C0: "arm",
    0x01C4: "armnt",
}


class PEFormatError(Exception):
    """The file is not a PE image this reader understands."""


def _u16(data: bytes, offset: int) -> int:
    if offset + 2 > len(data):
        raise PEFormatError(f"truncated at {offset} reading uint16")
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    if offset + 4 > len(data):
        raise PEFormatError(f"truncated at {offset} reading uint32")
    return struct.unpack_from("<I", data, offset)[0]


class PE:
    """One parsed PE image, enough for the gates' questions."""

    def __init__(self, data: bytes):
        self.data = data
        if len(data) < 0x40 or data[:2] != b"MZ":
            raise PEFormatError("no DOS header")
        pe_offset = _u32(data, 0x3C)
        if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise PEFormatError("no PE signature")
        coff = pe_offset + 4
        self.machine = _u16(data, coff)
        self.section_count = _u16(data, coff + 2)
        optional_size = _u16(data, coff + 16)
        optional = coff + 20
        self.optional = optional
        magic = _u16(data, optional)
        if magic == 0x10B:
            self.is_64bit = False
            directory_offset = optional + 96
        elif magic == 0x20B:
            self.is_64bit = True
            directory_offset = optional + 112
        else:
            raise PEFormatError(f"unknown optional header magic {magic:#x}")
        self.directory_offset = directory_offset
        self.sections = self._read_sections(optional + optional_size)

    def _read_sections(self, offset: int) -> list[tuple[int, int, int, int]]:
        sections = []
        for i in range(self.section_count):
            base = offset + i * 40
            virtual_size = _u32(self.data, base + 8)
            virtual_address = _u32(self.data, base + 12)
            raw_size = _u32(self.data, base + 16)
            raw_offset = _u32(self.data, base + 20)
            sections.append((virtual_address, max(virtual_size, raw_size), raw_offset, raw_size))
        return sections

    def rva_to_offset(self, rva: int) -> int:
        for virtual_address, span, raw_offset, _ in self.sections:
            if virtual_address <= rva < virtual_address + span:
                return raw_offset + (rva - virtual_address)
        raise PEFormatError(f"rva {rva:#x} is not in any section")

    def directory(self, index: int) -> tuple[int, int]:
        """(RVA, size) of a data directory, (0, 0) when absent."""
        offset = self.directory_offset + index * 8
        try:
            return _u32(self.data, offset), _u32(self.data, offset + 4)
        except PEFormatError:
            return 0, 0

    def exports(self) -> set[str]:
        """Every named export, sorted out of the name pointer table."""
        rva, size = self.directory(IMAGE_DIRECTORY_ENTRY_EXPORT)
        if not rva or not size:
            return set()
        base = self.rva_to_offset(rva)
        number_of_names = _u32(self.data, base + 24)
        names_rva = _u32(self.data, base + 32)
        if not number_of_names or not names_rva:
            return set()
        names_base = self.rva_to_offset(names_rva)
        names = set()
        for i in range(number_of_names):
            name_rva = _u32(self.data, names_base + i * 4)
            start = self.rva_to_offset(name_rva)
            end = self.data.find(b"\0", start)
            if end < 0:
                raise PEFormatError(f"unterminated export name at {start:#x}")
            names.add(self.data[start:end].decode("ascii", "replace"))
        return names

    def imports(self) -> set[str]:
        """The DLL names in the import descriptor table, lowercased."""
        rva, size = self.directory(IMAGE_DIRECTORY_ENTRY_IMPORT)
        if not rva or not size:
            return set()
        base = self.rva_to_offset(rva)
        names = set()
        # The table is terminated by an all-zero descriptor.
        for i in range(size // 20 + 1):
            entry = base + i * 20
            try:
                name_rva = _u32(self.data, entry + 12)
            except PEFormatError:
                break
            if name_rva == 0:
                break
            start = self.rva_to_offset(name_rva)
            end = self.data.find(b"\0", start)
            if end < 0:
                raise PEFormatError(f"unterminated import name at {start:#x}")
            names.add(self.data[start:end].decode("ascii", "replace").lower())
        return names


def load(path: Path) -> PE:
    return PE(path.read_bytes())
