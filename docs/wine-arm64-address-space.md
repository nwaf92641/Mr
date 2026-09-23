# The arm64 address space Wine expects, and what macOS gives it

## The fact the load test ran into

Wine's user shared data is `KUSER_SHARED_DATA`, and Windows puts it at
`0x7ffe0000`, below the 4GB line. ntdll maps it at exactly that address:
`dlls/ntdll/unix/virtual.c`, `virtual_alloc_first_thread_data()` calls
`map_view( user_shared_data, page_size )`, and `virtual_map_user_shared_data()`
maps the section over the same address with `MAP_FIXED`.

On x86_64 macOS that works because Wine reserves the range for itself. The
reservation lives in the loader's own Mach-O image, through zerofill sections and
`-segaddr,WINE_RESERVE,0x1000 -segaddr,WINE_TOP_DOWN,0x7ff000000000`, and
`configure.ac` gives it to x86_64 only:

```
case $HOST_ARCH in
  i386)   wine_use_preloader=yes ;;
  x86_64) wine_use_preloader=no; WINELOADER_LDFLAGS="$... -segaddr,WINE_RESERVE,0x1000
                                              -segaddr,WINE_TOP_DOWN,0x7ff000000000" ;;
  *)      wine_use_preloader=no ;;      # arm64: no reservation of any kind
esac
```

`loader/main.c` compiles its zerofill reservation out on arm64 for the same
reason, so `wine_main_preload_info` is NULL and nothing is reserved. The 64-bit
branch of `mmap_init()` then reserves three areas, two of which lie below 4GB.

macOS on Apple Silicon has no address space below 4GB to give. Measured with
`tests/wine/reserve_probe.c` on the CI machine:

| request | result |
| --- | --- |
| `0x7ffe0000`, one page | `FAIL errno=12` (ENOMEM) |
| `0x1000`, the low 8GB | `FAIL errno=22` (EINVAL) |
| `0x7ff000000000`, the top-down area | `OK` |

The link flags cannot change that. A Mach-O whose `__TEXT` sits below 4GB is not
loadable on this platform at all, which is why the loader itself had to be relinked
at a normal PIE base before it would run.

## What the patch changes

`patches/wine/0001-arm64-shared-user-data.patch`, against the pinned revision:

1. `user_shared_data` becomes a variable on `__APPLE__ && __aarch64__` instead of
   the constant `0x7ffe0000`.
2. `virtual_init()` reserves the page at Wine's own top-down base,
   `0x7ff000000000` (`configure.ac`, `-segaddr,WINE_TOP_DOWN`), which the probe
   measured this platform granting. It is done in `virtual_init()` rather than in
   `mmap_init()` because the branch of `mmap_init()` that handles the 64-bit layout
   is not the one every build reaches: the first version of this patch placed the
   code there and the source carried it while Wine behaved as if it had no address
   at all. `virtual_init()` is compiled by every build.

   Letting the kernel choose the address was tried first and does not work. Wine
   places `host_addr_space_limit` at `0x7ffffe000000` (`get_host_addr_space_limit`,
   `MACH_VM_MAX_ADDRESS_RAW`), and this platform hands out unaddressed mappings at
   or above it, so `map_view` refuses with `STATUS_CONFLICTING_ADDRESSES`,
   `c0000018`. That is what the second run of the patched Wine reported, and it is
   why the address is one Wine already uses rather than one the kernel hands over.
3. The two reservations below 4GB are not attempted on arm64, because they cannot
   succeed. The top-down reservation above the line is kept.

## What this preserves, and what it costs

Preserved: the page is still a single page shared between processes through the
`\KernelObjects\__wine_user_shared_data` section, still mapped read-only in the
process after initialisation, and still reached by Wine's own code through the
same global. Wine's code reads it through the pointer in every place measured:
`LargePageMinimum`, `ProcessorFeatures` and the section mapping.

Cost: a Windows binary that hardcodes `0x7ffe0000`, and an x86-64 game under FEX
is such a binary, will not find it there on Apple Silicon. That address does not
exist on this platform, so the cost is the platform's and not the patch's; what the
eventual answer is for those binaries, whether a shim, an emulation layer or a
different placement rule, is a question for the FEX stage and is not answered here.

## What is still open

The 32-bit and low-64-bit regions Wine reserves for WoW64 and for the low
allocation area do not exist on this platform either. Nothing in the D3D11 stage
needs them, and the failure that blocked the load test is gone once the shared user
data has somewhere to live. WoW64 is not part of the current stage.
