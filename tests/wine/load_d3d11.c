/*
 * Ask Wine to load the three graphics modules and hand back the entry points.
 *
 * The PE workflow already read the exports out of these DLLs with llvm-readobj.
 * That establishes the symbols are in the files. This establishes something the
 * files cannot establish about themselves: that Wine's loader will map them, bind
 * their imports, and let a process call into them. A module that links and cannot
 * be loaded is the failure this is looking for, and export inspection is blind to
 * it by construction.
 *
 * It is built as a native arm64 Windows executable and run through the loader. The
 * modules are ARM64X, so the arm64 half of each is the half this process binds to,
 * which is the half a native arm64 process is entitled to.
 *
 * It deliberately does not call D3D11CreateDevice. Whether a device can be created
 * needs a GPU and a driver and is a separate question asked by a separate test, so
 * that the answer here stays about loading.
 *
 * Prints one result per line and only says LOAD TEST OK when every module loaded
 * and every entry point resolved. The caller greps for that exact string.
 */

#include <stdio.h>
#include <windows.h>

typedef struct {
  const char *module;
  const char *symbol;
} probe;

static int probe_one(const probe *p) {
  HMODULE module = LoadLibraryA(p->module);
  if (module == NULL) {
    printf("FAIL %s could not be loaded (GetLastError=%lu)\n", p->module,
           (unsigned long)GetLastError());
    return 0;
  }
  FARPROC symbol = GetProcAddress(module, p->symbol);
  if (symbol == NULL) {
    printf("FAIL %s loaded but %s was not found (GetLastError=%lu)\n", p->module,
           p->symbol, (unsigned long)GetLastError());
    return 0;
  }
  printf("OK   %s loaded, %s at %p\n", p->module, p->symbol, (void *)symbol);
  FreeLibrary(module);
  return 1;
}

int main(void) {
  char exe_path[MAX_PATH];
  HMODULE self = GetModuleHandleA(NULL);
  DWORD n = GetModuleFileNameA(self, exe_path, sizeof(exe_path));

  printf("load test\n");
  printf("  executable ....... %s\n", n > 0 ? exe_path : "(unknown)");

  static const probe probes[] = {
      {"d3d11.dll", "D3D11CreateDevice"},
      {"dxgi.dll", "CreateDXGIFactory"},
      {"d3dcompiler_47.dll", "D3DCompile"},
  };

  int loaded = 0;
  size_t total = sizeof(probes) / sizeof(probes[0]);
  for (size_t i = 0; i < total; i++) {
    if (probe_one(&probes[i])) loaded++;
  }

  if ((size_t)loaded == total) {
    printf("LOAD TEST OK: %d of %d modules loaded and resolved\n", loaded,
           (int)total);
    return 0;
  }
  printf("LOAD TEST FAILED: %d of %d modules loaded and resolved\n", loaded,
         (int)total);
  return 1;
}
