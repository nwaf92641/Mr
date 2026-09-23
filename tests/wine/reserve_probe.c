/*
 * Can this machine reserve the areas Wine needs, from a running process?
 *
 * Wine's user shared data lives at a fixed 0x7ffe0000, and ntdll maps it there
 * directly: dlls/ntdll/unix/virtual.c, virtual_alloc_first_thread_data calls
 * map_view( user_shared_data, page_size ). On x86_64 macOS that address works
 * because the loader reserves 0x1000..0x200000000 inside its own Mach-O image, and
 * on x86_64 a Mach-O may put its first segment at 0x1000 behind a 4KB __PAGEZERO.
 *
 * configure.ac gives that reservation to x86_64 only:
 *
 *   case $HOST_ARCH in
 *     i386)   wine_use_preloader=yes ;;
 *     x86_64) wine_use_preloader=no
 *             WINELOADER_LDFLAGS="$WINELOADER_LDFLAGS ... -segaddr,WINE_RESERVE,0x1000
 *                                  -segaddr,WINE_TOP_DOWN,0x7ff000000000" ;;
 *     *)      wine_use_preloader=no ;;      # arm64: no reservation of any kind
 *   esac
 *
 * and loader/main.c compiles its zerofill reservation out on arm64 for the same
 * reason, so wine_main_preload_info is NULL and nothing is reserved. The load test
 * reached Wine's own initialisation and stopped here:
 *
 *   err:virtual:map_fixed_area out of memory for 0x7ffe0000-0x7ffe1000
 *   err:virtual:virtual_alloc_first_thread_data wine: failed to map the shared user
 *   data: c0000017
 *
 * c0000017 is STATUS_NO_MEMORY. The fix depends on why, and the difference between
 * "Wine's own bookkeeping refused it" and "this operating system will not map
 * anything below 4GB for any process" decides which fix is right. So this asks the
 * operating system directly rather than inferring it.
 *
 * Built with /usr/bin/clang. The clang first on PATH in this job is llvm-mingw's and
 * targets Windows, which is why an earlier probe here "did not compile" with the
 * reason suppressed -- a failure that cannot be read is worse than no probe.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static void try_reserve(const char *what, void *addr, size_t size) {
  errno = 0;
  void *got = mmap(addr, size, PROT_NONE,
                   MAP_FIXED | MAP_NORESERVE | MAP_PRIVATE | MAP_ANON, -1, 0);
  if (got == MAP_FAILED) {
    printf("RESERVE FAIL %-26s at %p size 0x%zx errno=%d (%s)\n", what, addr,
           size, errno, strerror(errno));
    return;
  }
  printf("RESERVE OK   %-26s at %p size 0x%zx\n", what, got, size);
  munmap(got, size);
}

int main(void) {
  /* The page Wine's shared user data needs, which is below 4GB. */
  try_reserve("user shared data", (void *)0x7ffe0000, 0x1000);
  /* The area Wine reserves on x86_64 to keep the low 8GB to itself. */
  try_reserve("low 8GB", (void *)0x1000, (size_t)0x200000000 - 0x1000);
  /* The top-down area Wine reserves for the heap and thread stacks. */
  try_reserve("top down", (void *)0x7ff000000000, 0x1ff0000);
  /* A control that has to succeed, so a total failure is not read as a finding. */
  try_reserve("control, anywhere", NULL, 0x1000);
  return 0;
}
