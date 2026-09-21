/*
 * The smallest D3D11 program that proves something about DXMT's Metal path.
 *
 * It is an ordinary macOS program. It links DXMT's native build -- the
 * d3d11.dylib and dxgi.dylib that `meson compile` produces with DXMT's
 * `nativemetal` mode -- and calls the D3D11 entry points directly. No Wine, no
 * FEX, no Windows. That is the point: if this fails, Wine is not a suspect.
 *
 * What it does, in order, reporting each step separately so a failure names the
 * layer it came from:
 *
 *   device      D3D11CreateDevice
 *   adapter     DXGI adapter name, so the GPU in the log is the GPU that ran
 *   output      texture + render target view
 *   clear       ClearRenderTargetView with a known colour
 *   staging     a CPU-readable copy target
 *   copy        CopyResource, which is what forces the clear to be executed
 *   map         Map with D3D11_MAP_READ, which blocks until the GPU is done
 *   pixels      compare the bytes that came back against the bytes requested
 *
 * Why clear-and-read-back rather than a triangle: a triangle needs a shader, and
 * D3D11 takes DXBC, which needs fxc or Wine's d3dcompiler_47 to produce. Neither
 * exists in this path yet, and hand-writing DXBC bytecode to avoid admitting that
 * would be inventing evidence. Clear and read-back needs no shader and still
 * exercises resource creation, a render encoder, command submission, GPU
 * synchronisation and a real pixel comparison, so it proves the parts of the path
 * a triangle would exercise minus the pipeline. The draw steps are named in the
 * output as skipped, with the reason, rather than quietly omitted.
 *
 * Exit codes, because the difference matters:
 *   0   the GPU path ran and the pixels were correct
 *   1   a step failed; the failing step is named
 *   77  no usable Metal device, so nothing here could be verified. The caller is
 *       expected to report REQUIRES REAL APPLE GPU VALIDATION rather than either
 *       a pass or a failure.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

namespace {

int g_passed = 0;
int g_failed = 0;
int g_skipped = 0;

enum Status { PASS, FAIL, SKIP };

void report(int n, const char *what, Status status, const char *detail) {
  const char *tag = status == PASS ? "PASS" : (status == FAIL ? "FAIL" : "SKIP");
  if (status == PASS) g_passed++;
  if (status == FAIL) g_failed++;
  if (status == SKIP) g_skipped++;
  std::printf("[%2d] %-46s %s  %s\n", n, what, tag, detail == nullptr ? "" : detail);
  std::fflush(stdout);
}

const char *feature_level_name(D3D_FEATURE_LEVEL level) {
  switch (level) {
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    default: return "not a D3D11 feature level";
  }
}

const uint32_t kWidth = 64;
const uint32_t kHeight = 64;

/* Distinct per channel, so a channel swap is visible rather than plausible. */
const float kClear[4] = {0.25f, 0.50f, 0.75f, 1.0f};
const uint8_t kExpected[4] = {64, 128, 191, 255};

bool near(uint8_t got, uint8_t want) {
  int d = (int)got - (int)want;
  if (d < 0) d = -d;
  return d <= 2;
}

}  // namespace

int main() {
  std::printf("Mr D3D11 native probe, against DXMT's native macOS build\n\n");

  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  D3D_FEATURE_LEVEL level = (D3D_FEATURE_LEVEL)0;
  const D3D_FEATURE_LEVEL wanted[2] = {D3D_FEATURE_LEVEL_11_1,
                                       D3D_FEATURE_LEVEL_11_0};

  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 wanted, 2, D3D11_SDK_VERSION, &device, &level,
                                 &ctx);

  char detail[512];
  std::snprintf(detail, sizeof(detail), "HRESULT 0x%08x, feature level %s",
                (unsigned)hr, SUCCEEDED(hr) ? feature_level_name(level) : "-");
  report(1, "D3D11CreateDevice", SUCCEEDED(hr) && device != nullptr ? PASS : FAIL,
         detail);

  if (FAILED(hr) || device == nullptr || ctx == nullptr) {
    /*
     * Distinguish "the machine has no GPU for this" from "the code is wrong".
     * DXMT returns a failure here on a device it cannot use, and a paravirtual
     * GPU reporting no Metal families is exactly that case.
     */
    std::printf(
        "\nNo usable D3D11 device, so nothing below could be verified. On a\n"
        "machine whose Metal device is paravirtual this is expected and is not a\n"
        "result about this code.\n\n"
        "REQUIRES REAL APPLE GPU VALIDATION\n");
    return 77;
  }

  /* The adapter name is evidence: it says which GPU actually ran this. */
  {
    IDXGIDevice *dxgi_device = nullptr;
    char name[256] = "unavailable";
    IDXGIAdapter *adapter = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),
                                         reinterpret_cast<void **>(&dxgi_device))) &&
        dxgi_device != nullptr) {
      if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter != nullptr) {
        DXGI_ADAPTER_DESC desc;
        std::memset(&desc, 0, sizeof(desc));
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
          std::snprintf(name, sizeof(name), "%ls", desc.Description);
        }
      }
    }
    std::snprintf(detail, sizeof(detail), "adapter \"%s\"", name);
    report(2, "DXGI adapter named", adapter != nullptr ? PASS : SKIP, detail);
    if (adapter != nullptr) adapter->Release();
    if (dxgi_device != nullptr) dxgi_device->Release();
  }

  /* A render target, and a CPU-readable copy of it. */
  D3D11_TEXTURE2D_DESC desc;
  std::memset(&desc, 0, sizeof(desc));
  desc.Width = kWidth;
  desc.Height = kHeight;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  ID3D11Texture2D *target = nullptr;
  hr = device->CreateTexture2D(&desc, nullptr, &target);
  std::snprintf(detail, sizeof(detail), "HRESULT 0x%08x, %ux%u R8G8B8A8_UNORM",
                (unsigned)hr, kWidth, kHeight);
  report(3, "render target texture", SUCCEEDED(hr) && target ? PASS : FAIL, detail);
  if (FAILED(hr) || target == nullptr) return 1;

  ID3D11RenderTargetView *rtv = nullptr;
  hr = device->CreateRenderTargetView(target, nullptr, &rtv);
  std::snprintf(detail, sizeof(detail), "HRESULT 0x%08x", (unsigned)hr);
  report(4, "render target view", SUCCEEDED(hr) && rtv ? PASS : FAIL, detail);
  if (FAILED(hr) || rtv == nullptr) return 1;

  ctx->ClearRenderTargetView(rtv, kClear);
  /* The clear is deferred, so its success is only observable after the copy. */
  report(5, "ClearRenderTargetView issued", PASS, "rgba(0.25, 0.50, 0.75, 1.0)");

  D3D11_TEXTURE2D_DESC sdesc = desc;
  sdesc.Usage = D3D11_USAGE_STAGING;
  sdesc.BindFlags = 0;
  sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  ID3D11Texture2D *staging = nullptr;
  hr = device->CreateTexture2D(&sdesc, nullptr, &staging);
  std::snprintf(detail, sizeof(detail), "HRESULT 0x%08x", (unsigned)hr);
  report(6, "staging texture", SUCCEEDED(hr) && staging ? PASS : FAIL, detail);
  if (FAILED(hr) || staging == nullptr) return 1;

  /* This is what makes the GPU execute the clear: a copy out of the target. */
  ctx->CopyResource(staging, target);
  report(7, "CopyResource target -> staging", PASS, "forces the frame to execute");

  D3D11_MAPPED_SUBRESOURCE mapped;
  std::memset(&mapped, 0, sizeof(mapped));
  hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
  std::snprintf(detail, sizeof(detail), "HRESULT 0x%08x, row pitch %u",
                (unsigned)hr, (unsigned)mapped.RowPitch);
  report(8, "Map with D3D11_MAP_READ", SUCCEEDED(hr) && mapped.pData ? PASS : FAIL,
         detail);
  if (FAILED(hr) || mapped.pData == nullptr) return 1;

  /* Three places, not one: a clear should be uniform, and a single sample could
   * be right by accident. */
  const uint32_t xs[3] = {0, kWidth / 2, kWidth - 1};
  const uint32_t ys[3] = {0, kHeight / 2, kHeight - 1};
  bool pixels_ok = true;
  char found[256];
  int used = 0;
  for (int i = 0; i < 3; i++) {
    const uint8_t *p =
        reinterpret_cast<const uint8_t *>(mapped.pData) +
        (size_t)ys[i] * mapped.RowPitch + (size_t)xs[i] * 4u;
    used += std::snprintf(found + used, sizeof(found) - (size_t)used,
                          "%s(%u,%u)=%u,%u,%u,%u", i == 0 ? "" : " ", xs[i], ys[i],
                          p[0], p[1], p[2], p[3]);
    for (int c = 0; c < 4; c++) {
      if (!near(p[c], kExpected[c])) pixels_ok = false;
    }
  }
  ctx->Unmap(staging, 0);

  std::snprintf(detail, sizeof(detail), "wanted %u,%u,%u,%u; read %s",
                kExpected[0], kExpected[1], kExpected[2], kExpected[3], found);
  report(9, "pixels read back match the clear colour", pixels_ok ? PASS : FAIL,
         detail);

  /* Named rather than omitted, so the gap is visible in the output. */
  report(10, "vertex shader, pipeline and draw", SKIP,
         "D3D11 needs DXBC, which needs fxc or Wine's d3dcompiler_47; neither "
         "exists on this path yet");

  rtv->Release();
  target->Release();
  staging->Release();
  ctx->Release();
  device->Release();

  std::printf("\n%d passed, %d failed, %d skipped\n", g_passed, g_failed,
              g_skipped);
  if (g_failed != 0) return 1;
  std::printf("\nD3D11 device, clear and read-back verified through DXMT on a "
              "real Metal device.\n");
  return 0;
}
