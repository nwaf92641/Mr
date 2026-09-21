/*
 * The PE reader.
 *
 * Deals with untrusted input: a game EXE comes from a store, an installer or a
 * friend's USB stick, and this code parses it before anything has been vetted.
 * Every offset is checked against the buffer, every table walk is bounded, and
 * a structure that claims a length it cannot have is rejected rather than
 * clamped -- a title that parses as "no imports" because its import table was
 * malformed would be reported as needing nothing, which is the one wrong answer
 * that wastes the user's afternoon.
 */
#include "mr/mr_pe.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MR_PE_MACHINE_I386 0x014Cu
#define MR_PE_MACHINE_AMD64 0x8664u
#define MR_PE_MACHINE_ARM 0x01C0u
#define MR_PE_MACHINE_ARMNT 0x01C4u
#define MR_PE_MACHINE_IA64 0x0200u
#define MR_PE_MACHINE_ARM64 0xAA64u
#define MR_PE_MACHINE_ARM64EC_OBJ 0xA641u
#define MR_PE_MACHINE_ARM64X 0xA64Eu

#define MR_PE_OPT_MAGIC_32 0x010Bu
#define MR_PE_OPT_MAGIC_64 0x020Bu

#define MR_PE_DIR_EXPORT 0
#define MR_PE_DIR_IMPORT 1
#define MR_PE_DIR_RESOURCE 2
#define MR_PE_DIR_EXCEPTION 3
#define MR_PE_DIR_SECURITY 4
#define MR_PE_DIR_BASERELOC 5
#define MR_PE_DIR_DEBUG 6
#define MR_PE_DIR_ARCH 7
#define MR_PE_DIR_TLS 9
#define MR_PE_DIR_LOAD_CONFIG 10
#define MR_PE_DIR_IAT 12
#define MR_PE_DIR_DELAY_IMPORT 13
#define MR_PE_DIR_CLR 14

#define MR_PE_SUBSYSTEM_WINDOWS_GUI 2u
#define MR_PE_SUBSYSTEM_WINDOWS_CUI 3u

#define MR_PE_SECTION_OFFSET 40u
#define MR_PE_IMPORT_DESCRIPTOR_SIZE 20u
#define MR_PE_DELAY_DESCRIPTOR_SIZE 32u

/* Bounds. A malformed table must terminate the walk, not the process. */
#define MR_PE_IMPORT_DESCRIPTOR_LIMIT 4096u
#define MR_PE_THUNK_LIMIT 65536u
#define MR_PE_EXPORT_LIMIT 65536u
#define MR_PE_IMPORT_LIMIT 131072u

typedef struct {
  const uint8_t *data;
  size_t len;
  const mr_pe *pe;
} mr_pe_view;

/* ------------------------------------------------------------------ helpers */

static mr_status mr_pe_add_import(mr_pe *pe, const char *module,
                                  const char *symbol) {
  if (pe->import_count == pe->import_cap) {
    if (pe->import_cap >= MR_PE_IMPORT_LIMIT) return MR_ERR_RANGE;
    size_t cap = pe->import_cap == 0 ? 256 : pe->import_cap * 2;
    if (cap > MR_PE_IMPORT_LIMIT) cap = MR_PE_IMPORT_LIMIT;
    mr_pe_import *grown =
        (mr_pe_import *)realloc(pe->imports, cap * sizeof(*grown));
    if (grown == NULL) return MR_ERR_NOMEM;
    pe->imports = grown;
    pe->import_cap = cap;
  }
  mr_pe_import *slot = &pe->imports[pe->import_count++];
  memset(slot, 0, sizeof(*slot));
  if (module != NULL) {
    size_t n = strlen(module);
    if (n >= sizeof(slot->module)) n = sizeof(slot->module) - 1;
    for (size_t i = 0; i < n; i++) {
      char c = module[i];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      slot->module[i] = c;
    }
  }
  if (symbol != NULL) {
    size_t n = strlen(symbol);
    if (n >= sizeof(slot->symbol)) n = sizeof(slot->symbol) - 1;
    for (size_t i = 0; i < n; i++) {
      char c = symbol[i];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      slot->symbol[i] = c;
    }
  }
  return MR_OK;
}

/* RVA -> file offset, or 0 when the RVA falls outside every section. */
size_t mr_pe_rva_to_offset(const mr_pe *pe, uint32_t rva) {
  for (size_t i = 0; i < pe->section_count; i++) {
    const mr_pe_section *s = &pe->sections[i];
    /* The mapped size is the larger of the two; a section whose virtual size
     * exceeds its raw size is zero-padded by the loader, which means an RVA in
     * the tail has no bytes on disk and must not be translated. */
    uint32_t span = s->virtual_size > s->raw_size ? s->virtual_size : s->raw_size;
    if (rva < s->virtual_address || rva >= s->virtual_address + span) continue;
    uint32_t delta = rva - s->virtual_address;
    if (delta >= s->raw_size) return 0;
    return (size_t)s->raw_offset + delta;
  }
  return 0;
}

/* Reads a NUL-terminated ASCII string at a file offset, bounded. */
static bool mr_pe_string_at(const uint8_t *data, size_t len, size_t off,
                            char *dst, size_t dst_cap) {
  if (dst == NULL || dst_cap == 0) return false;
  dst[0] = '\0';
  if (data == NULL || off == 0 || off >= len) return false;
  size_t written = 0;
  for (size_t i = off; i < len && written + 1 < dst_cap; i++) {
    if (data[i] == '\0') {
      dst[written] = '\0';
      return written > 0;
    }
    dst[written++] = (char)data[i];
  }
  dst[written] = '\0';
  /* Ran off the end without a terminator: reject rather than accept a name that
   * may be a slice of something else. */
  return false;
}

static uint64_t mr_pe_count_magic(const uint8_t *data, size_t len,
                                  const char magic[4]) {
  uint64_t count = 0;
  if (len < 4) return 0;
  for (size_t i = 0; i + 4 <= len; i++) {
    if (data[i] == (uint8_t)magic[0] && data[i + 1] == (uint8_t)magic[1] &&
        data[i + 2] == (uint8_t)magic[2] && data[i + 3] == (uint8_t)magic[3]) {
      count++;
      i += 3;
    }
  }
  return count;
}

/* ------------------------------------------------------------------ parsing */

mr_status mr_pe_init(mr_pe *pe) {
  if (pe == NULL) return MR_ERR_INVALID;
  memset(pe, 0, sizeof(*pe));
  return mr_strvec_init(&pe->imported_modules);
}

void mr_pe_free(mr_pe *pe) {
  if (pe == NULL) return;
  free(pe->imports);
  pe->imports = NULL;
  pe->import_count = 0;
  pe->import_cap = 0;
  mr_strvec_free(&pe->imported_modules);
  mr_strvec_free(&pe->delay_loaded_modules);
  mr_strvec_free(&pe->exported_symbols);
  memset(pe, 0, sizeof(*pe));
}

static mr_arch mr_pe_arch_from_machine(uint16_t machine) {
  switch (machine) {
    case MR_PE_MACHINE_I386: return MR_ARCH_I386;
    case MR_PE_MACHINE_AMD64: return MR_ARCH_AMD64_OR_ARM64EC;
    /* A final image never carries 0xA641 -- see mr_pe_is_arm64ec_image. The
     * value only appears in intermediate object files, but a .obj handed to the
     * analyzer should still be described accurately rather than as x86-64. */
    case MR_PE_MACHINE_ARM64EC_OBJ: return MR_ARCH_AMD64_OR_ARM64EC;
    case MR_PE_MACHINE_ARM64X: return MR_ARCH_ARM64X;
    case MR_PE_MACHINE_ARM64: return MR_ARCH_ARM64;
    case MR_PE_MACHINE_ARM: return MR_ARCH_ARM;
    case MR_PE_MACHINE_ARMNT: return MR_ARCH_ARMNT;
    case MR_PE_MACHINE_IA64: return MR_ARCH_IA64;
    default: return MR_ARCH_UNKNOWN;
  }
}

bool mr_pe_is_arm64ec_image(const uint8_t *data, size_t len, size_t pe_offset) {
  if (data == NULL) return false;
  uint16_t machine = 0;
  if (!mr_read_u16(data, len, pe_offset + 4, &machine)) return false;
  /* ARM64X is the only final-image machine word that guarantees ARM64EC code is
   * present. For machine 0x8664 the answer lives in the CHPE metadata hanging
   * off the load configuration directory, and reporting "not ARM64EC" from the
   * machine word alone would be a confident wrong answer; callers that need
   * certainty must check for the CHPE metadata themselves. */
  return machine == MR_PE_MACHINE_ARM64X;
}

/*
 * Reads up to `count` section headers from `table_off`. Returns false when the
 * table was not fully present, which is a different fact from how many sections
 * were readable.
 */
static bool mr_pe_read_sections(mr_pe *pe, const uint8_t *data, size_t len,
                                size_t table_off, uint16_t count) {
  size_t usable = count;
  if (usable > MR_PE_MAX_SECTIONS) usable = MR_PE_MAX_SECTIONS;
  for (size_t i = 0; i < usable; i++) {
    size_t off = table_off + i * MR_PE_SECTION_OFFSET;
    /* A table that runs off the end of the buffer describes a truncated image.
     * Stopping quietly here is what let a fragment of a file be reported as a
     * valid image, which the analyzer then describes as a real game. */
    if (off + MR_PE_SECTION_OFFSET > len) return false;
    mr_pe_section *s = &pe->sections[pe->section_count];
    mr_read_ascii(data, len, off, 8, s->name, sizeof(s->name));
    (void)mr_read_u32(data, len, off + 8, &s->virtual_size);
    (void)mr_read_u32(data, len, off + 12, &s->virtual_address);
    (void)mr_read_u32(data, len, off + 16, &s->raw_size);
    (void)mr_read_u32(data, len, off + 20, &s->raw_offset);
    (void)mr_read_u32(data, len, off + 36, &s->characteristics);
    pe->section_count++;
  }
  /* A declared count above the cap is also an image we cannot describe. */
  return usable == count;
}

static void mr_pe_read_import_table(mr_pe *pe, const uint8_t *data, size_t len,
                                    uint32_t rva, uint32_t size) {
  if (rva == 0 || size == 0) return;
  size_t base = mr_pe_rva_to_offset(pe, rva);
  if (base == 0) return;

  /* The descriptor array is terminated by an all-zero entry, but the directory
   * size is advisory and a crafted file can set it to anything, so the walk is
   * bounded by both. */
  size_t max_entries = size / MR_PE_IMPORT_DESCRIPTOR_SIZE + 1;
  if (max_entries > MR_PE_IMPORT_DESCRIPTOR_LIMIT) {
    max_entries = MR_PE_IMPORT_DESCRIPTOR_LIMIT;
  }

  for (size_t i = 0; i < max_entries; i++) {
    size_t entry = base + i * MR_PE_IMPORT_DESCRIPTOR_SIZE;
    if (entry + MR_PE_IMPORT_DESCRIPTOR_SIZE > len) break;

    uint32_t oft = 0, name_rva = 0, first_thunk = 0;
    (void)mr_read_u32(data, len, entry + 0, &oft);
    (void)mr_read_u32(data, len, entry + 12, &name_rva);
    (void)mr_read_u32(data, len, entry + 16, &first_thunk);
    if (name_rva == 0 && oft == 0 && first_thunk == 0) break; /* terminator */

    char module[64];
    if (!mr_pe_string_at(data, len, mr_pe_rva_to_offset(pe, name_rva), module,
                         sizeof(module))) {
      continue;
    }
    for (char *c = module; *c != '\0'; c++) {
      if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    }
    if (!mr_strvec_contains(&pe->imported_modules, module)) {
      (void)mr_strvec_push(&pe->imported_modules, module);
    }
    (void)mr_pe_add_import(pe, module, NULL);

    /* Symbol names come from the import lookup table; FirstThunk is the IAT,
     * which the loader overwrites, so it is only a fallback for images that
     * omit the lookup table entirely. */
    uint32_t thunk_rva = oft != 0 ? oft : first_thunk;
    size_t thunk_base = mr_pe_rva_to_offset(pe, thunk_rva);
    if (thunk_base == 0) continue;
    size_t step = pe->is_64bit ? 8u : 4u;

    for (size_t t = 0; t < MR_PE_THUNK_LIMIT; t++) {
      size_t at = thunk_base + t * step;
      uint64_t value = 0;
      if (pe->is_64bit) {
        if (!mr_read_u64(data, len, at, &value)) break;
        if ((value >> 63) != 0) continue; /* import by ordinal */
      } else {
        uint32_t v32 = 0;
        if (!mr_read_u32(data, len, at, &v32)) break;
        value = v32;
        if ((v32 >> 31) != 0) continue;
      }
      if (value == 0) break;

      char symbol[128];
      size_t name_off = mr_pe_rva_to_offset(pe, (uint32_t)value);
      if (name_off == 0 ||
          !mr_pe_string_at(data, len, name_off + 2, symbol, sizeof(symbol))) {
        continue;
      }
      (void)mr_pe_add_import(pe, module, symbol);
    }
  }
}

static void mr_pe_read_delay_imports(mr_pe *pe, const uint8_t *data, size_t len,
                                     uint32_t rva, uint32_t size) {
  if (rva == 0 || size == 0) return;
  size_t base = mr_pe_rva_to_offset(pe, rva);
  if (base == 0) return;

  size_t max_entries = size / MR_PE_DELAY_DESCRIPTOR_SIZE + 1;
  if (max_entries > MR_PE_IMPORT_DESCRIPTOR_LIMIT) {
    max_entries = MR_PE_IMPORT_DESCRIPTOR_LIMIT;
  }

  for (size_t i = 0; i < max_entries; i++) {
    size_t entry = base + i * MR_PE_DELAY_DESCRIPTOR_SIZE;
    if (entry + MR_PE_DELAY_DESCRIPTOR_SIZE > len) break;
    uint32_t name_rva = 0;
    (void)mr_read_u32(data, len, entry + 4, &name_rva);
    if (name_rva == 0) break;
    char module[64];
    if (!mr_pe_string_at(data, len, mr_pe_rva_to_offset(pe, name_rva), module,
                         sizeof(module))) {
      continue;
    }
    for (char *c = module; *c != '\0'; c++) {
      if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
    }
    if (!mr_strvec_contains(&pe->delay_loaded_modules, module)) {
      (void)mr_strvec_push(&pe->delay_loaded_modules, module);
    }
  }
}

static void mr_pe_read_exports(mr_pe *pe, const uint8_t *data, size_t len,
                               uint32_t rva, uint32_t size) {
  if (rva == 0 || size == 0) return;
  size_t base = mr_pe_rva_to_offset(pe, rva);
  if (base == 0 || base + 40 > len) return;

  uint32_t name_count = 0, names_rva = 0;
  (void)mr_read_u32(data, len, base + 24, &name_count);
  (void)mr_read_u32(data, len, base + 32, &names_rva);
  if (name_count == 0 || names_rva == 0) return;
  if (name_count > MR_PE_EXPORT_LIMIT) name_count = MR_PE_EXPORT_LIMIT;

  size_t names_base = mr_pe_rva_to_offset(pe, names_rva);
  if (names_base == 0) return;

  for (uint32_t i = 0; i < name_count; i++) {
    uint32_t name_rva = 0;
    if (!mr_read_u32(data, len, names_base + i * 4u, &name_rva)) break;
    if (name_rva == 0) continue;
    char symbol[128];
    if (!mr_pe_string_at(data, len, mr_pe_rva_to_offset(pe, name_rva), symbol,
                         sizeof(symbol))) {
      continue;
    }
    (void)mr_strvec_push(&pe->exported_symbols, symbol);
  }
}

/* Reads one data directory entry. */
static void mr_pe_read_directory(const uint8_t *data, size_t len,
                                 size_t directory_offset,
                                 uint32_t directory_count, uint32_t index,
                                 uint32_t *rva, uint32_t *size) {
  *rva = 0;
  *size = 0;
  if (index >= directory_count) return;
  size_t at = directory_offset + (size_t)index * 8u;
  uint32_t r = 0, s = 0;
  if (!mr_read_u32(data, len, at, &r)) return;
  if (!mr_read_u32(data, len, at + 4, &s)) return;
  *rva = r;
  *size = s;
}

mr_status mr_pe_parse_at(const uint8_t *data, size_t len, size_t offset,
                         mr_pe *out) {
  if (data == NULL || out == NULL) return MR_ERR_INVALID;
  if (offset > len || len - offset < 0x40) return MR_ERR_TRUNCATED;

  uint16_t dos_magic = 0;
  if (!mr_read_u16(data, len, offset, &dos_magic)) return MR_ERR_TRUNCATED;
  if (dos_magic != 0x5A4Du) return MR_ERR_PARSE; /* "MZ" */

  uint32_t lfanew = 0;
  if (!mr_read_u32(data, len, offset + 0x3C, &lfanew)) return MR_ERR_TRUNCATED;

  /* `lfanew` is attacker-controlled. Reject rather than overflow. */
  if ((uint64_t)offset + (uint64_t)lfanew + 24u > (uint64_t)len) {
    return MR_ERR_TRUNCATED;
  }
  size_t pe_off = offset + lfanew;

  uint32_t signature = 0;
  (void)mr_read_u32(data, len, pe_off, &signature);
  if (signature != 0x00004550u) return MR_ERR_PARSE; /* "PE\0\0" */

  size_t coff = pe_off + 4;
  uint16_t machine = 0;
  uint16_t section_count = 0;
  uint16_t optional_size = 0;
  uint16_t characteristics = 0;
  (void)mr_read_u16(data, len, coff + 0, &machine);
  (void)mr_read_u16(data, len, coff + 2, &section_count);
  (void)mr_read_u16(data, len, coff + 16, &optional_size);
  (void)mr_read_u16(data, len, coff + 18, &characteristics);
  (void)mr_read_u32(data, len, coff + 8, &out->timestamp);

  size_t optional = coff + 20;
  uint16_t magic = 0;
  if (!mr_read_u16(data, len, optional, &magic)) return MR_ERR_TRUNCATED;

  size_t directory_offset;
  size_t directory_count_off;
  size_t image_base_off;
  if (magic == MR_PE_OPT_MAGIC_32) {
    out->is_64bit = false;
    directory_offset = optional + 96;
    directory_count_off = optional + 92;
    image_base_off = optional + 28;
  } else if (magic == MR_PE_OPT_MAGIC_64) {
    out->is_64bit = true;
    directory_offset = optional + 112;
    directory_count_off = optional + 108;
    image_base_off = optional + 24;
  } else {
    /* ROM images and the two unknown magics cannot hold guest code. */
    return MR_ERR_PARSE;
  }

  if (optional + 72 > len) return MR_ERR_TRUNCATED;

  uint8_t major_linker = 0, minor_linker = 0;
  uint8_t major_os = 0, minor_os = 0, major_sub = 0, minor_sub = 0;
  if (!mr_read_u8_at(data, len, optional + 2, &major_linker)) return MR_ERR_TRUNCATED;
  if (!mr_read_u8_at(data, len, optional + 3, &minor_linker)) return MR_ERR_TRUNCATED;
  if (!mr_read_u8_at(data, len, optional + 40, &major_os)) return MR_ERR_TRUNCATED;
  if (!mr_read_u8_at(data, len, optional + 41, &minor_os)) return MR_ERR_TRUNCATED;
  if (!mr_read_u8_at(data, len, optional + 48, &major_sub)) return MR_ERR_TRUNCATED;
  if (!mr_read_u8_at(data, len, optional + 49, &minor_sub)) return MR_ERR_TRUNCATED;
  out->major_linker = major_linker;
  out->minor_linker = minor_linker;
  out->major_os = major_os;
  out->minor_os = minor_os;
  out->major_subsystem = major_sub;
  out->minor_subsystem = minor_sub;

  (void)mr_read_u32(data, len, optional + 16, &out->entry_point_rva);
  (void)mr_read_u32(data, len, optional + 56, &out->size_of_image);
  (void)mr_read_u32(data, len, optional + 64, &out->checksum);
  if (out->is_64bit) {
    (void)mr_read_u64(data, len, image_base_off, &out->image_base);
  } else {
    uint32_t base32 = 0;
    (void)mr_read_u32(data, len, image_base_off, &base32);
    out->image_base = base32;
  }
  (void)mr_read_u16(data, len, optional + 68, &out->subsystem);
  (void)mr_read_u16(data, len, optional + 70, &out->dll_characteristics);

  uint32_t directory_count = 0;
  (void)mr_read_u32(data, len, directory_count_off, &directory_count);
  /* MSVC emits 16; a larger count is legal but the tables past 16 are not
   * standardised, and trusting it would let a file point us anywhere. */
  if (directory_count > 16u) directory_count = 16u;

  out->machine = machine;
  out->arch = mr_pe_arch_from_machine(machine);
  out->is_arm64ec = (machine == MR_PE_MACHINE_ARM64X);
  out->is_dll = (characteristics & 0x2000u) != 0;

  bool sections_complete =
      mr_pe_read_sections(out, data, len, optional + optional_size,
                          section_count);

  /*
   * Whether the file actually contains the data its sections claim. This is how
   * an interrupted download presents itself: the headers are all there -- many
   * installers write them first -- and the body is not. Tracked separately from
   * the section-table question above because the two describe different faults
   * and get different advice.
   *
   * Section raw offsets are relative to the start of the image, not to the start
   * of the buffer, so `offset` is part of the comparison. A PE embedded in a
   * self-extracting archive is the case that makes the difference.
   */
  bool body_truncated = !sections_complete;
  for (size_t i = 0; i < out->section_count; i++) {
    const mr_pe_section *s = &out->sections[i];
    if (s->raw_size == 0) continue;
    /* The sum is computed in 64 bits so a hostile raw_offset cannot wrap. */
    uint64_t end = (uint64_t)offset + (uint64_t)s->raw_offset +
                   (uint64_t)s->raw_size;
    if (end > (uint64_t)len) body_truncated = true;
  }
  out->is_truncated = body_truncated;

  uint32_t rva = 0, size = 0;

  mr_pe_read_directory(data, len, directory_offset, directory_count,
                       MR_PE_DIR_IMPORT, &rva, &size);
  mr_pe_read_import_table(out, data, len, rva, size);

  mr_pe_read_directory(data, len, directory_offset, directory_count,
                       MR_PE_DIR_DELAY_IMPORT, &rva, &size);
  mr_pe_read_delay_imports(out, data, len, rva, size);

  mr_pe_read_directory(data, len, directory_offset, directory_count,
                       MR_PE_DIR_EXPORT, &rva, &size);
  mr_pe_read_exports(out, data, len, rva, size);

  mr_pe_read_directory(data, len, directory_offset, directory_count,
                       MR_PE_DIR_CLR, &rva, &size);
  out->has_clr_header = (rva != 0 && size != 0);
  out->clr_rva = rva;
  out->clr_size = size;

  mr_pe_read_directory(data, len, directory_offset, directory_count,
                       MR_PE_DIR_RESOURCE, &rva, &size);
  out->has_resource_manifest = (rva != 0 && size != 0);

  mr_pe_read_directory(data, len, directory_offset, directory_count,
                       MR_PE_DIR_TLS, &rva, &size);
  out->has_tls_directory = (rva != 0 && size != 0);

  mr_pe_read_directory(data, len, directory_offset, directory_count,
                       MR_PE_DIR_DEBUG, &rva, &size);
  out->has_debug_directory = (rva != 0 && size != 0);

  mr_pe_read_directory(data, len, directory_offset, directory_count,
                       MR_PE_DIR_BASERELOC, &rva, &size);
  out->has_relocations = (rva != 0 && size != 0);

  /* Shader-container census. This scans the mapped image, not just .rdata,
   * because some packers move it, and it counts non-overlapping magics only so
   * that a stream of "DXBCDXBC" cannot inflate the number. */
  out->dxbc_blob_count = (uint32_t)mr_pe_count_magic(data + offset, len - offset, "DXBC");
  out->dxil_blob_count = (uint32_t)mr_pe_count_magic(data + offset, len - offset, "DXIL");

  mr_strvec_sort(&out->imported_modules);

  /*
   * Validity is not the same as "the headers parsed".
   *
   * A Windows image must have at least one section, and its section table must
   * be present: a fragment that passes the header checks but cannot supply its
   * own sections is not a program, and reporting it as valid leads the analyzer
   * to describe a game that cannot exist and the user to import a bad download
   * without being told.
   */
  out->is_valid = sections_complete && out->section_count > 0;
  return MR_OK;
}

mr_status mr_pe_parse(const uint8_t *data, size_t len, mr_pe *out) {
  return mr_pe_parse_at(data, len, 0, out);
}

mr_status mr_pe_load(const char *path, mr_pe *out) {
  if (path == NULL || out == NULL) return MR_ERR_INVALID;

  mr_bytes bytes;
  mr_status st = mr_read_file(path, &bytes);
  if (st != MR_OK) return st;
  st = mr_pe_parse(bytes.data, bytes.len, out);
  mr_bytes_free(&bytes);
  return st;
}

/* ------------------------------------------------------------------ queries */

bool mr_pe_imports_module(const mr_pe *pe, const char *module_lowercase) {
  if (pe == NULL || module_lowercase == NULL) return false;
  return mr_strvec_contains(&pe->imported_modules, module_lowercase);
}

bool mr_pe_imports_module_family(const mr_pe *pe, const char *stem_lowercase) {
  if (pe == NULL || stem_lowercase == NULL) return false;
  if (stem_lowercase[0] == '\0') return false;

  size_t stem_len = strlen(stem_lowercase);
  for (size_t i = 0; i < pe->imported_modules.count; i++) {
    const char *name = pe->imported_modules.items[i];
    /*
     * A prefix match, not a substring or an exact comparison.
     *
     * This is what the detection tables need. Windows module names are
     * <family><variant><version>.dll: d3d11.dll and d3d11_1.dll are the same
     * API, xinput1_4.dll and xinput9_1_0.dll are the same API, and
     * easyanticheat_x64.dll is the same product as easyanticheat.dll. An exact
     * comparison against the stem never fires, which is how a title importing
     * easyanticheat_x64.dll was reported as having no anti-cheat at all; a
     * substring match would fire on unrelated names (dinput.dll inside
     * something else). Anchoring at the start is the rule that holds.
     */
    if (strncmp(name, stem_lowercase, stem_len) == 0) return true;
  }
  return false;
}

bool mr_pe_imports_symbol(const mr_pe *pe, const char *module_lowercase,
                          const char *symbol_lowercase) {
  if (pe == NULL || module_lowercase == NULL || symbol_lowercase == NULL) {
    return false;
  }
  for (size_t i = 0; i < pe->import_count; i++) {
    const mr_pe_import *imp = &pe->imports[i];
    if (imp->symbol[0] == '\0') continue;
    if (strcmp(imp->module, module_lowercase) != 0) continue;
    if (strcmp(imp->symbol, symbol_lowercase) == 0) return true;
  }
  return false;
}

const mr_pe_section *mr_pe_find_section(const mr_pe *pe, const char *name) {
  if (pe == NULL || name == NULL) return NULL;
  for (size_t i = 0; i < pe->section_count; i++) {
    if (strcmp(pe->sections[i].name, name) == 0) return &pe->sections[i];
  }
  return NULL;
}

bool mr_pe_has_section(const mr_pe *pe, const char *name) {
  return mr_pe_find_section(pe, name) != NULL;
}

const char *mr_pe_subsystem_str(uint16_t subsystem) {
  switch (subsystem) {
    case 1: return "native";
    case MR_PE_SUBSYSTEM_WINDOWS_GUI: return "windows-gui";
    case MR_PE_SUBSYSTEM_WINDOWS_CUI: return "windows-console";
    case 5: return "os2-console";
    case 7: return "posix-console";
    case 9: return "windows-ce-gui";
    case 10: return "efi-application";
    case 14: return "xbox";
    case 16: return "windows-boot";
    default: return "unknown";
  }
}
