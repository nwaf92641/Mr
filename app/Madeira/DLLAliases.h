/* DLL name aliases for the few DirectX module names this bundle cannot ship.
 *
 * This table used to hold the whole d3dx9_24..42 and d3dcompiler_33..46 ladder,
 * pointing every generation at d3dx9_43 / d3dcompiler_43 / d3dcompiler_47 on the
 * theory that "the export names carry no version, so the newest answers for all
 * of them". That theory is false, and measurably so. Wine models each
 * generation's export table in dlls/d3dx9_<n>/d3dx9_<n>.spec -- the names
 * Microsoft's build of that generation exported -- and comparing those against
 * the module that would answer for them gives:
 *
 *   d3dx9_24..30 -- 9 exports d3dx9_43 does NOT have: D3DXCreateFragmentLinker,
 *                   D3DXGatherFragments{,FromFileA,FromFileW,FromResourceA,
 *                   FromResourceW}, D3DXCpuOptimizations, D3DXGetTargetDescBy*
 *   d3dx9_31..35 -- 6: the GatherFragments group and the linker, without
 *                   D3DXCpuOptimizations or D3DXGetTargetDescBy*
 *   d3dx9_36..41 -- 7: as 31..35, plus D3DXCreateFragmentLinkerEx
 *   d3dx10_33..39 -- 4 or 5: D3DX10DisassembleShader, D3DX10DisassembleEffect,
 *                   D3DX10ReflectShader, D3DX10GetDriverLevel, and (37..39)
 *                   D3DX10CreateReduction
 *   d3dcompiler_33..39 -- 6: D3DCompileFromMemory, D3DDisassembleCode,
 *                   D3DDisassembleEffect, D3DGetCodeDebugInfo,
 *                   D3DPreprocessFromMemory, D3DReflectCode
 *
 * The three generations nearest the target -- d3dx9_42, d3dx10_40..42 and
 * d3dcompiler_40..42 -- ARE complete subsets of it, which is why checking a
 * ladder by hand finds agreement: two of the three families are sound for their
 * last two or three rungs and broken for the ones before.
 *
 * A title linked against one of those imports them by name. Aliasing makes the
 * module load and then fails at the import, which is worse than not shipping the
 * alias at all: the failure moves from "cannot find d3dx9_35.dll" to a partial
 * load that no test here would have noticed. Those generations are now built and
 * shipped as real modules instead (tools/pe-module-manifest.txt, the
 * d3dx9-legacy / d3dx10-legacy / d3dcompiler-legacy groups).
 *
 * What is left is the one generation with no binary in existence: Microsoft's
 * d3dcompiler_44 and _45 shipped only inside the SDK and appear in no
 * redistributable, so there is nothing to ship and no Wine module to build.
 * d3dcompiler_47 answers for them, and that is proven rather than assumed --
 * the shipped d3dcompiler_43 (17 exports) and Wine's d3dcompiler_46 model are
 * both strict subsets of the shipped d3dcompiler_47 (29 exports), and 44/45 sit
 * between those generations in Microsoft's numbering. The required sets live in
 * tools/pe-alias-exports.txt and tools/check-dll-aliases.py re-checks them
 * against the real binaries on every gate run.
 *
 * Three rules that gate enforces:
 *   1. an alias target must be a module this bundle actually ships;
 *   2. an alias name must NOT be a module this bundle ships -- a real
 *      implementation always wins over the symlink, so such an entry would be a
 *      comment pretending to be a mechanism;
 *   3. every export of the aliased-away module must exist in the target,
 *      according to the reference set recorded in tools/pe-alias-exports.txt. An
 *      alias with no recorded reference set fails, so an unverified alias cannot
 *      be added back.
 *
 * Keep this file to the table and this comment: the gate parses it, and the
 * runtime loop in WineProcessBridge.m reads nothing else.
 */
#ifndef MADEIRA_DLL_ALIASES_H
#define MADEIRA_DLL_ALIASES_H

typedef struct {
    const char *name;    /* the name a title imports */
    const char *target;  /* a module present in the session's system32 */
} MadeiraDLLAlias;

static const MadeiraDLLAlias kMadeiraDLLAliases[] = {
    /* The two D3DCompiler generations that have no binary anywhere. See above:
     * d3dcompiler_47 is a verified superset of both bracketing generations. */
    { "d3dcompiler_44.dll", "d3dcompiler_47.dll" },
    { "d3dcompiler_45.dll", "d3dcompiler_47.dll" },
};

#define MADEIRA_DLL_ALIAS_COUNT \
    (sizeof(kMadeiraDLLAliases) / sizeof(kMadeiraDLLAliases[0]))

#endif /* MADEIRA_DLL_ALIASES_H */
