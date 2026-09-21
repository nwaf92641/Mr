/*
 * A bounds-checked PE/COFF reader.
 *
 * Scope is deliberately narrow: it answers the questions the Game Analyzer
 * asks, and nothing else. It never maps sections, never runs code and never
 * writes to the image -- the input is an untrusted file downloaded from a store
 * or copied off a PC, so every read is range-checked and a malformed image
 * produces MR_ERR_PARSE rather than reading past the buffer.
 *
 * The reader is also intentionally dependency-free. The analyzer has to run
 * inside the iPadOS app, where there is no Python and no package manager, so it
 * cannot lean on pefile or LIEF.
 */
#ifndef MR_PE_H
#define MR_PE_H

#include "mr/mr_types.h"
#include "mr/mr_util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MR_PE_MAX_SECTIONS 96
#define MR_PE_SECTION_NAME_MAX 9

typedef struct {
  char name[MR_PE_SECTION_NAME_MAX];
  uint32_t virtual_size;
  uint32_t virtual_address;
  uint32_t raw_size;
  uint32_t raw_offset;
  uint32_t characteristics;
} mr_pe_section;

/* One import, flattened. Keeping the module it came from lets callers ask
 * "does anything import D3D11CreateDevice?" without re-walking the tables. */
typedef struct {
  char module[64]; /* lowercased DLL name, without any path */
  char symbol[128];
} mr_pe_import;

typedef struct {
  bool is_valid;
  /*
   * True when the file stops before some section's raw data does, or before the
   * section table does. This is the incomplete-download case, and it is worth its
   * own flag: the image describes itself perfectly well in its headers, so
   * is_valid can be true while the file is only part of the program. Reporting
   * that as runnable sends the user into a crash inside the loader instead of
   * telling them to download it again.
   */
  bool is_truncated;
  bool is_64bit;
  uint16_t machine;
  mr_arch arch;
  /*
   * True when the image is an ARM64EC hybrid. Such an image reports machine
   * 0x8664 and only the ARM64EC metadata distinguishes it from a genuine
   * x86-64 binary, so this must be parsed rather than inferred.
   */
  bool is_arm64ec;
  bool is_dll;
  uint16_t subsystem;
  uint16_t dll_characteristics;
  uint32_t timestamp;
  uint64_t image_base;
  uint32_t entry_point_rva;
  uint32_t size_of_image;
  uint32_t checksum;
  uint8_t major_linker;
  uint8_t minor_linker;
  uint8_t major_os;
  uint8_t minor_os;
  uint8_t major_subsystem;
  uint8_t minor_subsystem;

  mr_pe_section sections[MR_PE_MAX_SECTIONS];
  size_t section_count;

  mr_pe_import *imports;
  size_t import_count;
  size_t import_cap;

  mr_strvec imported_modules; /* unique, lowercased, sorted */
  mr_strvec delay_loaded_modules;
  mr_strvec exported_symbols;

  bool has_clr_header;      /* .NET */
  /* Kept alongside the flag because the CLR header also leads to the metadata
   * version string, which is how the required .NET runtime is identified
   * instead of guessed at. */
  uint32_t clr_rva;
  uint32_t clr_size;
  bool has_resource_manifest;
  bool has_tls_directory;
  bool has_debug_directory;
  bool has_relocations;

  /*
   * Frequency of the DXBC shader container magic in the raw file. Direct3D
   * bytecode embedded in the image is direct evidence that the title ships
   * pre-compiled D3D shaders, which is stronger than an import alone and is
   * how we tell a D3D11 title from one that merely links d3d11 by accident.
   */
  uint32_t dxbc_blob_count;
  uint32_t dxil_blob_count; /* DXIL: shader model 6, i.e. D3D12 territory */
} mr_pe;

/* Parses `data` into `out`. `out` must already be zeroed via mr_pe_init(). */
mr_status mr_pe_parse(const uint8_t *data, size_t len, mr_pe *out);
mr_status mr_pe_init(mr_pe *pe);
void mr_pe_free(mr_pe *pe);

mr_status mr_pe_load(const char *path, mr_pe *out);

/* Reads a PE from a raw stream embedded in another container (an installer, or
 * a self-extracting launcher), starting at `offset`. */
mr_status mr_pe_parse_at(const uint8_t *data, size_t len, size_t offset,
                         mr_pe *out);

/* Exact match against one imported module name. Both sides lowercase. */
bool mr_pe_imports_module(const mr_pe *pe, const char *module_lowercase);

/*
 * True when any imported module name *begins with* `stem_lowercase`.
 *
 * This is the predicate the detection tables want. Windows names a module
 * <family><variant><version>.dll, so the table entry is the family and the
 * import is the full name: "xinput" must match xinput1_4.dll, and "d3d11" must
 * match d3d11_1.dll. An exact comparison silently never fires on those, which
 * makes a title look like it uses no anti-cheat and no controller.
 */
bool mr_pe_imports_module_family(const mr_pe *pe, const char *stem_lowercase);

bool mr_pe_imports_symbol(const mr_pe *pe, const char *module_lowercase,
                          const char *symbol_lowercase);
bool mr_pe_has_section(const mr_pe *pe, const char *name);
const mr_pe_section *mr_pe_find_section(const mr_pe *pe, const char *name);

/*
 * Translates a relative virtual address to a file offset, or returns 0 when the
 * RVA is outside every section. Exposed because callers that follow a data
 * directory (the CLR metadata root, for instance) need to walk the image
 * themselves, and duplicating the section arithmetic there is how the two
 * copies drift apart.
 */
size_t mr_pe_rva_to_offset(const mr_pe *pe, uint32_t rva);

/* True when the image carries an ARM64EC metadata directory entry. */
bool mr_pe_is_arm64ec_image(const uint8_t *data, size_t len, size_t pe_offset);

const char *mr_pe_subsystem_str(uint16_t subsystem);

#ifdef __cplusplus
}
#endif

#endif /* MR_PE_H */
