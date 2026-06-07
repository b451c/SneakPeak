// ============================================================================
// ui_render.cpp — Blend2D offscreen rendering + cross-platform blit
//
// PHASE 0 spike implementation. See ui_render.h for rationale.
// The offscreen surface management mirrors spectral_view.cpp exactly:
//   Win32 : CreateDIBSection (top-down 32-bit BI_RGB) -> bm.bmBits
//   SWELL : SWELL_CreateMemContext + SWELL_GetCtxFrameBuffer
// Blend2D renders into a BLImage (PRGB32); we copy its scanlines into that
// framebuffer (forcing opaque alpha), then BitBlt into the destination HDC.
// ============================================================================

#include "ui_render.h"

// Blend2D public header. Wrapped to keep the project's strict warning flags
// (-Wconversion/-Wshadow/-Wdouble-promotion) from firing inside library headers.
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wconversion"
#  pragma GCC diagnostic ignored "-Wshadow"
#  pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#include <blend2d/blend2d.h>
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

#include <algorithm>

UiCanvas::~UiCanvas()
{
#ifdef _WIN32
  if (m_memBmp) { DeleteObject(m_memBmp); m_memBmp = nullptr; }
  if (m_memDC)  { DeleteDC(m_memDC); m_memDC = nullptr; }
#else
  if (m_memDC)  { SWELL_DeleteGfxContext(m_memDC); m_memDC = nullptr; }
#endif
}

bool UiCanvas::ensure(HDC hdc, int w, int h)
{
  if (w < 1 || h < 1) return false;
  if (m_memDC && m_w == w && m_h == h) return true;

#ifdef _WIN32
  if (m_memBmp) { DeleteObject(m_memBmp); m_memBmp = nullptr; }
  if (m_memDC)  { DeleteDC(m_memDC); m_memDC = nullptr; }
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h; // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  m_memBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!m_memBmp) return false;
  m_memDC = CreateCompatibleDC(hdc);
  if (!m_memDC) { DeleteObject(m_memBmp); m_memBmp = nullptr; return false; }
  SelectObject(m_memDC, m_memBmp);
#else
  if (m_memDC) { SWELL_DeleteGfxContext(m_memDC); m_memDC = nullptr; }
  m_memDC = SWELL_CreateMemContext(hdc, w, h);
  if (!m_memDC) return false;
#endif
  m_w = w;
  m_h = h;
  return true;
}

void UiCanvas::RenderSpikeDemo(HDC hdc, int x, int y, int w, int h)
{
  if (!hdc || w < 2 || h < 2) return;
  if (!ensure(hdc, w, h)) return;

  // Locate the destination framebuffer (contiguous, w pixels per row).
  unsigned int* fbuf = nullptr;
#ifdef _WIN32
  BITMAP bm;
  if (GetObject(m_memBmp, sizeof(bm), &bm)) fbuf = (unsigned int*)bm.bmBits;
#else
  fbuf = (unsigned int*)SWELL_GetCtxFrameBuffer(m_memDC);
#endif
  if (!fbuf) return;

  // --- Blend2D vector render into an offscreen image ---
  BLImage img(w, h, BL_FORMAT_PRGB32);
  if (!img) return;   // image allocation failed
  {
    BLContext ctx;
    if (ctx.begin(img) != BL_SUCCESS) return;   // context / JIT init failed

    // Dark vertical gradient background (panel body).
    BLGradient grad(BLLinearGradientValues(0.0, 0.0, 0.0, (double)h));
    grad.add_stop(0.0, BLRgba32(0xFF202830u));
    grad.add_stop(1.0, BLRgba32(0xFF12161Bu));
    ctx.fill_all(grad);

    // Rounded-rect body with a subtle fill.
    const double pad = 6.0;
    const double rw = w - pad * 2.0;
    const double rh = h - pad * 2.0;
    const double rr = 10.0;
    ctx.fill_round_rect(BLRoundRect(pad, pad, rw, rh, rr), BLRgba32(0xFF2A323Cu));

    // Cyan accent border (proves AA strokes).
    ctx.set_stroke_width(2.0);
    ctx.stroke_round_rect(BLRoundRect(pad, pad, rw, rh, rr), BLRgba32(0xFF3CC8DCu));

    // Smooth orange transfer-style curve (proves AA + curve rendering + JIT).
    BLPath path;
    const double cx0 = pad + 8.0;
    const double cy0 = h - pad - 8.0;
    const double cx1 = w - pad - 8.0;
    const double cy1 = pad + 8.0;
    path.move_to(cx0, cy0);
    path.cubic_to((cx0 + cx1) * 0.5, cy0, (cx0 + cx1) * 0.5, cy1, cx1, cy1);
    ctx.set_stroke_width(2.5);
    ctx.stroke_path(path, BLRgba32(0xFFFFA000u));

    if (ctx.end() != BL_SUCCESS) return;
  }

  // --- Copy Blend2D scanlines into the platform framebuffer (force opaque) ---
  BLImageData dat;
  if (img.get_data(&dat) != BL_SUCCESS) return;
  if (!dat.pixel_data) return;
  if (dat.stride <= 0 || (dat.stride % 4) != 0 || dat.stride < (intptr_t)w * 4) return;
  const uint8_t* srcBase = (const uint8_t*)dat.pixel_data;
  for (int row = 0; row < h; ++row) {
    const unsigned int* s = (const unsigned int*)(srcBase + (intptr_t)row * dat.stride);
    unsigned int* d = fbuf + (size_t)row * (size_t)w;
    for (int col = 0; col < w; ++col)
      d[col] = s[col] | 0xFF000000u;
  }

  BitBlt(hdc, x, y, w, h, m_memDC, 0, 0, SRCCOPY);
}
