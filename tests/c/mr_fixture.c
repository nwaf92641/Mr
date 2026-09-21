#include "mr_fixture.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ writing */

static void mr_put_u8(uint8_t *dst, size_t off, uint8_t v) { dst[off] = v; }

static void mr_put_u16(uint8_t *dst, size_t off, uint16_t v) {
  dst[off] = (uint8_t)(v & 0xFFu);
  dst[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void mr_put_u32(uint8_t *dst, size_t off, uint32_t v) {
  dst[off] = (uint8_t)(v & 0xFFu);
  dst[off + 1] = (uint8_t)((v >> 8) & 0xFFu);
  dst[off + 2] = (uint8_t)((v >> 16) & 0xFFu);
  dst[off + 3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void mr_put_u64(uint8_t *dst, size_t off, uint64_t v) {
  mr_put_u32(dst, off, (uint32_t)(v & 0xFFFFFFFFu));
  mr_put_u32(dst, off + 4, (uint32_t)(v >> 32));
}

static void mr_put_ascii(uint8_t *dst, size_t off, const char *s, size_t cap) {
  size_t n = strlen(s);
  if (n >= cap) n = cap - 1;
  memcpy(dst + off, s, n);
  dst[off + n] = '\0';
}

/* RVA inside .rdata to a file offset. */
static size_t mr_rdata_off(uint32_t rva) {
  return MR_FIXTURE_RDATA_OFF + (rva - MR_FIXTURE_RDATA_RVA);
}

/* -------------------------------------------------------------------- init */

void mr_fixture_init(mr_fixture *f) {
  memset(f, 0, sizeof(*f));
  f->machine = 0x8664u;
  f->subsystem = 2;
  f->characteristics = 0x0022u; /* executable image, large address aware */
  f->clr_version = "v4.0.30319";

  /* The DOS stub. The message sits at 0x10, below the header fields at 0x3C, so
   * writing it cannot clobber e_lfanew -- a version of this that wrote it after
   * the header made every fixture unparseable. */
  mr_put_u16(f->bytes, 0, 0x5A4Du); /* "MZ" */
  mr_put_ascii(f->bytes, 0x10,
               "This program cannot be run in DOS mode.\r\r\n$", 40);
  mr_put_u32(f->bytes, 0x3C, 0x40u); /* e_lfanew */
}

/* ------------------------------------------------------------------ imports */

bool mr_fixture_add_import(mr_fixture *f, const char *module,
                           const char *const *symbols, size_t symbol_count) {
  if (f->import_count >= MR_FIXTURE_MAX_IMPORTS) return false;
  if (symbol_count > MR_FIXTURE_MAX_SYMBOLS) return false;

  mr_fixture_import *imp = &f->imports[f->import_count++];
  imp->module = module;
  imp->symbol_count = symbol_count;
  for (size_t i = 0; i < symbol_count; i++) imp->symbols[i] = symbols[i];
  return true;
}

void mr_fixture_add_blob(mr_fixture *f, const char *magic, size_t payload_len) {
  size_t need = 4 + payload_len;
  if (f->rdata_tail_len + need > sizeof(f->rdata_tail)) return;

  memcpy(f->rdata_tail + f->rdata_tail_len, magic, 4);
  f->rdata_tail_len += 4;

  /* A real container has a length field and a checksum; the analyzer counts
   * magic words, so filler is enough here and keeps the fixture honest about
   * what it is: a marker, not a valid container. */
  memset(f->rdata_tail + f->rdata_tail_len, 0xA5, payload_len);
  f->rdata_tail_len += payload_len;
}

bool mr_fixture_add_section(mr_fixture *f, const char *name) {
  if (f->extra_section_count >= 4) return false;
  f->extra_sections[f->extra_section_count++] = name;
  return true;
}

void mr_fixture_set_clr(mr_fixture *f, const char *version) {
  f->with_clr_header = true;
  f->clr_version = version;
}

/* ------------------------------------------------------------------- build */

/*
 * Lays out .rdata in a fixed order: import descriptors, then the module name and
 * symbol strings they point at, then the CLR header and its metadata root, then
 * whatever blobs were requested. The order matters only in that the addresses
 * must be assigned before the descriptors that reference them are written, which
 * is why this is a single pass with a moving cursor rather than several passes.
 */
static bool mr_build_rdata(mr_fixture *f, uint32_t *import_dir_rva,
                          uint32_t *import_dir_size, uint32_t *clr_rva) {
  uint32_t cursor = MR_FIXTURE_RDATA_RVA;

  /* Reserve the descriptor array: one per import plus the terminator. */
  uint32_t descriptors_rva = cursor;
  size_t descriptors_bytes = (f->import_count + 1) * 20;
  cursor += (uint32_t)descriptors_bytes;
  cursor = (cursor + 7u) & ~7u; /* keep thunks 8-byte aligned */

  /* Per import: the module name, then the ILT and a matching IAT. */
  uint32_t module_rva[MR_FIXTURE_MAX_IMPORTS];
  uint32_t thunk_rva[MR_FIXTURE_MAX_IMPORTS];
  uint32_t hint_rva[MR_FIXTURE_MAX_IMPORTS][MR_FIXTURE_MAX_SYMBOLS];

  for (size_t i = 0; i < f->import_count; i++) {
    module_rva[i] = cursor;
    cursor += (uint32_t)strlen(f->imports[i].module) + 1;
    cursor = (cursor + 7u) & ~7u;

    thunk_rva[i] = cursor;
    /* ILT plus terminator, pointer-sized. */
    cursor += (uint32_t)((f->imports[i].symbol_count + 1) * 8);
    cursor += (uint32_t)((f->imports[i].symbol_count + 1) * 8); /* the IAT */
    cursor = (cursor + 7u) & ~7u;

    for (size_t s = 0; s < f->imports[i].symbol_count; s++) {
      hint_rva[i][s] = cursor;
      /* 2-byte hint, then the name. */
      cursor += 2 + (uint32_t)strlen(f->imports[i].symbols[s]) + 1;
      cursor = (cursor + 1u) & ~1u;
    }
  }

  /* CLR header and metadata root, when requested. */
  uint32_t clr_header_rva = 0;
  uint32_t metadata_rva = 0;
  if (f->with_clr_header) {
    clr_header_rva = cursor;
    cursor += 72; /* the CLR header is fixed size */

    metadata_rva = cursor;
    size_t version_len = strlen(f->clr_version) + 1;
    /* Signature (4) + major (2) + minor (2) + reserved (4) + length (4) + the
     * version string padded to a 4-byte boundary. */
    size_t root_bytes = 16 + ((version_len + 3u) & ~3u);
    cursor += (uint32_t)root_bytes;
    cursor = (cursor + 7u) & ~7u;
  }

  /* Blobs last, so their addresses stay stable as imports are added. */
  uint32_t tail_rva = cursor;
  cursor += (uint32_t)f->rdata_tail_len;

  if (cursor - MR_FIXTURE_RDATA_RVA > 0x400u) return false; /* .rdata is 1 KB */

  uint8_t *b = f->bytes;

  /* Module names, thunks and hint/name entries. */
  for (size_t i = 0; i < f->import_count; i++) {
    mr_put_ascii(b, mr_rdata_off(module_rva[i]), f->imports[i].module, 64);

    size_t ilt_off = mr_rdata_off(thunk_rva[i]);
    size_t iat_off = ilt_off + (f->imports[i].symbol_count + 1) * 8;

    for (size_t s = 0; s < f->imports[i].symbol_count; s++) {
      mr_put_u64(b, ilt_off + s * 8, hint_rva[i][s]);
      mr_put_u64(b, iat_off + s * 8, hint_rva[i][s]);
      mr_put_u16(b, mr_rdata_off(hint_rva[i][s]), 0);
      mr_put_ascii(b, mr_rdata_off(hint_rva[i][s]) + 2, f->imports[i].symbols[s],
                   64);
    }
    /* Terminators stay zero, which the memset already guaranteed. */
  }

  /* Import descriptors. */
  size_t desc_off = mr_rdata_off(descriptors_rva);
  for (size_t i = 0; i < f->import_count; i++) {
    size_t entry = desc_off + i * 20;
    mr_put_u32(b, entry + 0, thunk_rva[i]);      /* OriginalFirstThunk */
    mr_put_u32(b, entry + 4, 0);                 /* TimeDateStamp */
    mr_put_u32(b, entry + 8, 0);                 /* ForwarderChain */
    mr_put_u32(b, entry + 12, module_rva[i]);    /* Name */
    mr_put_u32(b, entry + 16, thunk_rva[i] +
                                   (uint32_t)(f->imports[i].symbol_count + 1) *
                                       8);       /* FirstThunk */
  }
  /* The terminator descriptor is already zero. */

  *import_dir_rva = descriptors_rva;
  *import_dir_size = (uint32_t)descriptors_bytes;

  /* CLR header. Offset 8 holds the metadata directory as an RVA/size pair. */
  if (f->with_clr_header) {
    size_t off = mr_rdata_off(clr_header_rva);
    mr_put_u32(b, off + 0, 72);           /* cb */
    mr_put_u16(b, off + 4, 2);            /* MajorRuntimeVersion */
    mr_put_u16(b, off + 6, 5);            /* MinorRuntimeVersion */
    mr_put_u32(b, off + 8, metadata_rva); /* MetaData RVA */
    mr_put_u32(b, off + 12, 16u +
                   (uint32_t)((strlen(f->clr_version) + 4u) & ~3u));
    mr_put_u32(b, off + 16, 1);           /* Flags: ILONLY */
    *clr_rva = clr_header_rva;

    size_t root = mr_rdata_off(metadata_rva);
    mr_put_u32(b, root + 0, 0x424A5342u); /* "BSJB" */
    mr_put_u16(b, root + 4, 1);
    mr_put_u16(b, root + 6, 1);
    mr_put_u32(b, root + 8, 0);
    uint32_t version_len = (uint32_t)strlen(f->clr_version) + 1;
    mr_put_u32(b, root + 12, version_len);
    mr_put_ascii(b, root + 16, f->clr_version, 64);
  }

  memcpy(b + mr_rdata_off(tail_rva), f->rdata_tail, f->rdata_tail_len);
  return true;
}

bool mr_fixture_build(mr_fixture *f) {
  /* Section list: the three standard ones, plus any the test added. */
  const char *names[8];
  uint32_t rvas[8];
  uint32_t offsets[8];
  size_t count = 3;

  names[0] = ".rdata";
  rvas[0] = MR_FIXTURE_RDATA_RVA;
  offsets[0] = MR_FIXTURE_RDATA_OFF;
  names[1] = ".text";
  rvas[1] = MR_FIXTURE_TEXT_RVA;
  offsets[1] = MR_FIXTURE_TEXT_OFF;
  names[2] = ".data";
  rvas[2] = MR_FIXTURE_DATA_RVA;
  offsets[2] = MR_FIXTURE_DATA_OFF;

  for (size_t i = 0; i < f->extra_section_count; i++) {
    names[count] = f->extra_sections[i];
    rvas[count] = MR_FIXTURE_DATA_RVA + 0x1000u * (uint32_t)(count - 2);
    offsets[count] = MR_FIXTURE_DATA_OFF + 0x200u * (uint32_t)(count - 2);
    count++;
  }
  for (size_t i = count; i < 8; i++) {
    names[i] = NULL;
    rvas[i] = 0;
    offsets[i] = 0;
  }

  uint32_t import_rva = 0;
  uint32_t import_size = 0;
  uint32_t clr_rva = 0;
  if (!mr_build_rdata(f, &import_rva, &import_size, &clr_rva)) return false;

  uint8_t *b = f->bytes;
  const size_t pe_off = 0x40;
  const size_t coff = pe_off + 4;
  const size_t optional = coff + 20;
  const size_t optional_size = 240; /* PE32+ with 16 data directories */
  const size_t section_table = optional + optional_size;

  if (section_table + count * 40 > MR_FIXTURE_RDATA_OFF) return false;

  memcpy(b + pe_off, "PE\0\0", 4);

  mr_put_u16(b, coff + 0, f->machine);
  mr_put_u16(b, coff + 2, (uint16_t)count);
  mr_put_u32(b, coff + 4, 0x5F000000u); /* a fixed timestamp keeps ids stable */
  mr_put_u32(b, coff + 8, 0x5F000000u);
  mr_put_u32(b, coff + 12, 0);
  mr_put_u32(b, coff + 16, 0);
  mr_put_u16(b, coff + 16, (uint16_t)optional_size);
  mr_put_u16(b, coff + 18, f->characteristics);

  mr_put_u16(b, optional + 0, 0x020Bu); /* PE32+ */
  mr_put_u8(b, optional + 2, 14);       /* linker 14.0 */
  mr_put_u8(b, optional + 3, 0);
  mr_put_u32(b, optional + 4, 0x1000u);  /* SizeOfCode */
  mr_put_u32(b, optional + 8, 0x1000u);  /* SizeOfInitializedData */
  mr_put_u32(b, optional + 12, 0);       /* SizeOfUninitializedData */
  mr_put_u32(b, optional + 16, MR_FIXTURE_TEXT_RVA); /* AddressOfEntryPoint */
  mr_put_u32(b, optional + 20, MR_FIXTURE_TEXT_RVA); /* BaseOfCode */
  mr_put_u64(b, optional + 24, 0x140000000ull);      /* ImageBase */
  mr_put_u32(b, optional + 32, 0x1000u); /* SectionAlignment */
  mr_put_u32(b, optional + 36, 0x200u);  /* FileAlignment */
  mr_put_u8(b, optional + 40, 6);        /* MajorOperatingSystemVersion */
  mr_put_u8(b, optional + 48, 6);        /* MajorSubsystemVersion */
  mr_put_u32(b, optional + 56, 0x4000u); /* SizeOfImage */
  mr_put_u32(b, optional + 60, 0x200u);  /* SizeOfHeaders */
  mr_put_u16(b, optional + 68, f->subsystem);
  mr_put_u32(b, optional + 72, 0x100000u); /* SizeOfStackReserve */
  mr_put_u32(b, optional + 76, 0x1000u);   /* SizeOfStackCommit */
  mr_put_u32(b, optional + 80, 0x100000u); /* SizeOfHeapReserve */
  mr_put_u32(b, optional + 84, 0x1000u);   /* SizeOfHeapCommit */
  mr_put_u32(b, optional + 108, 16u);      /* NumberOfRvaAndSizes */

  /* Data directory 1 is the import table; 14 is the CLR header. */
  mr_put_u32(b, optional + 112 + 1 * 8, import_rva);
  mr_put_u32(b, optional + 112 + 1 * 8 + 4, import_size);
  if (clr_rva != 0) {
    mr_put_u32(b, optional + 112 + 14 * 8, clr_rva);
    mr_put_u32(b, optional + 112 + 14 * 8 + 4, 72u);
  }

  for (size_t i = 0; i < count; i++) {
    size_t s = section_table + i * 40;
    mr_put_ascii(b, s, names[i], 8);
    /* .rdata needs a full 1 KB because the fixture's tail may fill it; the
     * others only carry their headers. */
    uint32_t raw_size = (i == 0) ? 0x400u : 0x200u;
    mr_put_u32(b, s + 8, raw_size);
    mr_put_u32(b, s + 12, rvas[i]);
    mr_put_u32(b, s + 16, raw_size);
    mr_put_u32(b, s + 20, offsets[i]);
    mr_put_u32(b, s + 36, (i == 0) ? 0x40000040u : 0x60000020u);
  }

  f->len = MR_FIXTURE_DATA_OFF + 0x200u + (count > 3 ? (count - 3) * 0x200u : 0);
  if (f->len > MR_FIXTURE_CAPACITY) return false;
  return true;
}

bool mr_fixture_write(const mr_fixture *f, const char *path) {
  FILE *fp = fopen(path, "wb");
  if (fp == NULL) return false;
  size_t written = fwrite(f->bytes, 1, f->len, fp);
  fclose(fp);
  return written == f->len;
}

/* --------------------------------------------------------------- presets */

void mr_fixture_d3d11(mr_fixture *f) {
  mr_fixture_init(f);
  static const char *const d3d11_symbols[] = {
      "D3D11CreateDeviceAndSwapChain", "D3D11CreateDevice",
      "D3D11CreateDeferredContext"};
  static const char *const dxgi_symbols[] = {"CreateDXGIFactory1"};
  static const char *const user32_symbols[] = {"GetAsyncKeyState",
                                               "RegisterRawInputDevices"};
  static const char *const xinput_symbols[] = {"XInputGetState"};

  (void)mr_fixture_add_import(f, "d3d11.dll", d3d11_symbols, 3);
  (void)mr_fixture_add_import(f, "DXGI.DLL", dxgi_symbols, 1);
  (void)mr_fixture_add_import(f, "USER32.dll", user32_symbols, 2);
  (void)mr_fixture_add_import(f, "xinput1_4.dll", xinput_symbols, 1);
  (void)mr_fixture_add_import(f, "VCRUNTIME140.dll", NULL, 0);

  mr_fixture_add_blob(f, "DXBC", 64);
  mr_fixture_add_blob(f, "DXBC", 32);
}

void mr_fixture_i386(mr_fixture *f) {
  mr_fixture_init(f);
  f->machine = 0x014Cu;
  f->characteristics = 0x0102u; /* 32-bit executable */
  static const char *const symbols[] = {"D3D11CreateDevice"};
  (void)mr_fixture_add_import(f, "d3d11.dll", symbols, 1);
}

void mr_fixture_anticheat(mr_fixture *f) {
  mr_fixture_d3d11(f);
  (void)mr_fixture_add_import(f, "EasyAntiCheat_x64.dll", NULL, 0);
}
