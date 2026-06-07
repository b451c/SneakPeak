// ============================================================================
// ui_render.cpp — Blend2D offscreen rendering + cross-platform blit
//
// The offscreen surface management mirrors spectral_view.cpp exactly:
//   Win32 : CreateDIBSection (top-down 32-bit BI_RGB) -> bm.bmBits
//   SWELL : SWELL_CreateMemContext + SWELL_GetCtxFrameBuffer
// Blend2D renders into a BLImage (PRGB32); we copy its scanlines into that
// framebuffer (forcing opaque alpha), then BitBlt into the destination HDC.
// ============================================================================

#include "ui_render.h"
#include "ui_theme.h"

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
#include <cmath>

// --- helpers ---------------------------------------------------------------

static inline BLRgba32 col(uint32_t argb) { return BLRgba32(argb); }
static inline BLRgba32 colA(uint32_t argb, int a) {
  return BLRgba32((uint32_t(a) << 24) | (argb & 0x00FFFFFFu));
}

// Compression gain reduction (dB, <= 0) for an input level, matching
// dynamics_engine.cpp ComputeCompression(): soft-knee, slope = 1/R - 1.
static double CompressionGrDb(double inDb, double thrDb, double ratio, double kneeDb) {
  if (ratio <= 1.0) return 0.0;
  const double slope = 1.0 / ratio - 1.0;   // negative
  const double halfKnee = kneeDb * 0.5;
  const double overshoot = inDb - thrDb;
  if (overshoot <= -halfKnee) return 0.0;
  if (kneeDb > 0.0 && overshoot < halfKnee) {
    const double t = overshoot + halfKnee;
    return slope * t * t / (2.0 * kneeDb);
  }
  return slope * overshoot;
}

// Stacked-stroke additive glow for a path (Blend2D has no blur primitive).
static void GlowStroke(BLContext& ctx, const BLPath& path, uint32_t baseArgb) {
  ctx.set_comp_op(BL_COMP_OP_PLUS);
  struct Pass { double w; int a; };
  const Pass passes[3] = { {7.0, 26}, {4.0, 48}, {2.0, 90} };
  for (const Pass& p : passes) {
    ctx.set_stroke_width(p.w);
    ctx.stroke_path(path, colA(baseArgb, p.a));
  }
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
}

// --- lifecycle -------------------------------------------------------------

UiCanvas::~UiCanvas()
{
#ifdef _WIN32
  if (m_memBmp) { DeleteObject(m_memBmp); m_memBmp = nullptr; }
  if (m_memDC)  { DeleteDC(m_memDC); m_memDC = nullptr; }
#else
  if (m_memDC)  { SWELL_DeleteGfxContext(m_memDC); m_memDC = nullptr; }
#endif
}

bool UiCanvas::ensure(HDC hdc, int devW, int devH)
{
  if (devW < 1 || devH < 1) return false;
  if (m_memDC && m_w == devW && m_h == devH) return true;

#ifdef _WIN32
  if (m_memBmp) { DeleteObject(m_memBmp); m_memBmp = nullptr; }
  if (m_memDC)  { DeleteDC(m_memDC); m_memDC = nullptr; }
  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = devW;
  bmi.bmiHeader.biHeight = -devH; // top-down
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
  m_memDC = SWELL_CreateMemContext(hdc, devW, devH);
  if (!m_memDC) return false;
#endif
  m_w = devW;
  m_h = devH;
  return true;
}

// --- the hero render -------------------------------------------------------

void UiCanvas::RenderTransferCurve(HDC hdc, int x, int y, int w, int h, double dpr,
                                   const DynCurveParams& p)
{
  if (!hdc || w < 8 || h < 8) return;
  if (dpr < 1.0) dpr = 1.0;
  const int devW = (int)std::lround(w * dpr);
  const int devH = (int)std::lround(h * dpr);
  if (!ensure(hdc, devW, devH)) return;

  unsigned int* fbuf = nullptr;
#ifdef _WIN32
  BITMAP bm;
  if (GetObject(m_memBmp, sizeof(bm), &bm)) fbuf = (unsigned int*)bm.bmBits;
#else
  fbuf = (unsigned int*)SWELL_GetCtxFrameBuffer(m_memDC);
#endif
  if (!fbuf) return;

  BLImage img(devW, devH, BL_FORMAT_PRGB32);
  if (!img) return;
  {
    BLContext ctx;
    if (ctx.begin(img) != BL_SUCCESS) return;
    ctx.scale(dpr);                 // draw everything in LOGICAL coordinates
    ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

    const double W = w, H = h;

    // --- panel slab: vertical gradient + rounded body + top hairline highlight
    {
      BLGradient bg(BLLinearGradientValues(0, 0, 0, H));
      bg.add_stop(0.0, col(dynui::kSurface1));
      bg.add_stop(1.0, col(dynui::kSurface0));
      ctx.fill_round_rect(BLRoundRect(0, 0, W, H, dynui::kRadiusPanel), bg);
      ctx.set_stroke_width(1.0);
      ctx.stroke_round_rect(BLRoundRect(0.5, 0.5, W - 1, H - 1, dynui::kRadiusPanel),
                            col(dynui::kHairline));
    }

    // --- plot well (square, inset) ---
    const double pad = dynui::kPanelPad;
    const double plotSide = std::min(W - 2 * pad, H - 2 * pad);
    const double px0 = pad;
    const double py0 = (H - plotSide) * 0.5;
    const double px1 = px0 + plotSide;
    const double py1 = py0 + plotSide;
    ctx.fill_round_rect(BLRoundRect(px0, py0, plotSide, plotSide, dynui::kRadiusCtrl),
                        col(dynui::kSurface1));

    const double inMin = p.inMinDb, inMax = p.inMaxDb;
    const double span = (inMax - inMin) != 0 ? (inMax - inMin) : 1.0;
    auto dbToX = [&](double db) { return px0 + (db - inMin) / span * plotSide; };
    auto dbToY = [&](double db) { return py1 - (db - inMin) / span * plotSide; };
    auto outAt = [&](double in) {
      return in + CompressionGrDb(in, p.thresholdDb, p.ratio, p.kneeDb);
    };

    // clip to the plot well so curves never spill over the rounded corners
    ctx.save();
    ctx.clip_to_rect(BLRect(px0, py0, plotSide, plotSide));

    // unity 1:1 diagonal (faint reference)
    ctx.set_stroke_width(1.0);
    ctx.stroke_line(BLLine(px0, py1, px1, py0), col(dynui::kCurveUnity));

    const int N = 96;
    auto sampleDb = [&](int i) { return inMin + span * (double)i / (double)N; };

    // reduction wedge: area between unity diagonal and the curve, red->transparent
    {
      BLPath wedge;
      wedge.move_to(dbToX(inMin), dbToY(inMin));
      wedge.line_to(dbToX(inMax), dbToY(inMax));        // along the diagonal
      for (int i = N; i >= 0; --i) {                    // back along the curve
        const double in = sampleDb(i);
        wedge.line_to(dbToX(in), dbToY(outAt(in)));
      }
      wedge.close();
      BLGradient g(BLLinearGradientValues(0, py0, 0, py1));
      g.add_stop(0.0, colA(dynui::kGrRed, 70));
      g.add_stop(1.0, colA(dynui::kGrRed, 0));
      ctx.fill_path(wedge, g);
    }

    // dotted threshold line (vertical), persists regardless of overlays
    {
      const double tx = dbToX(p.thresholdDb);
      ctx.set_stroke_width(1.0);
      for (double yy = py0; yy < py1; yy += 5.0)
        ctx.stroke_line(BLLine(tx, yy, tx, std::min(yy + 2.5, py1)),
                        colA(dynui::kInkMuted, 200));
    }

    // gate cliff (lower-left): violet, output drops by gateRange below gate thr
    if (p.showGate && p.gateRangeDb > 0.0) {
      BLPath gate;
      bool started = false;
      for (int i = 0; i <= N; ++i) {
        const double in = sampleDb(i);
        if (in > p.gateThreshDb) break;
        const double out = std::max(inMin, in - p.gateRangeDb);
        if (!started) { gate.move_to(dbToX(in), dbToY(out)); started = true; }
        else          gate.line_to(dbToX(in), dbToY(out));
      }
      if (started) {
        gate.line_to(dbToX(p.gateThreshDb), dbToY(outAt(p.gateThreshDb))); // riser
        ctx.set_stroke_width(2.0);
        ctx.stroke_path(gate, col(dynui::kGateViolet));
      }
    }

    // static transfer curve (neutral)
    BLPath curve;
    for (int i = 0; i <= N; ++i) {
      const double in = sampleDb(i);
      const double cx = dbToX(in), cy = dbToY(outAt(in));
      if (i == 0) curve.move_to(cx, cy); else curve.line_to(cx, cy);
    }
    ctx.set_stroke_width(2.0);
    ctx.stroke_path(curve, col(dynui::kCurveStatic));

    // live segment up to the operating point (avgPeak), amber + glow
    const bool hasOp = p.avgPeakDb > inMin && p.avgPeakDb <= inMax;
    if (hasOp) {
      BLPath lit;
      bool started = false;
      for (int i = 0; i <= N; ++i) {
        const double in = sampleDb(i);
        if (in > p.avgPeakDb) break;
        const double cx = dbToX(in), cy = dbToY(outAt(in));
        if (!started) { lit.move_to(cx, cy); started = true; } else lit.line_to(cx, cy);
      }
      lit.line_to(dbToX(p.avgPeakDb), dbToY(outAt(p.avgPeakDb)));
      GlowStroke(ctx, lit, dynui::kAmberGlow);
      ctx.set_stroke_width(2.0);
      ctx.stroke_path(lit, col(dynui::kAmber));

      // operating-point dot + glow
      const double dx = dbToX(p.avgPeakDb), dy = dbToY(outAt(p.avgPeakDb));
      ctx.set_comp_op(BL_COMP_OP_PLUS);
      ctx.fill_circle(BLCircle(dx, dy, 7.0), colA(dynui::kAmberGlow, 60));
      ctx.fill_circle(BLCircle(dx, dy, 4.5), colA(dynui::kAmberGlow, 110));
      ctx.set_comp_op(BL_COMP_OP_SRC_OVER);
      ctx.fill_circle(BLCircle(dx, dy, 3.0), col(dynui::kAmber));
    }

    ctx.restore();   // remove plot clip
    ctx.end();
  }

  // --- copy Blend2D scanlines into the platform framebuffer (force opaque) ---
  BLImageData dat;
  if (img.get_data(&dat) != BL_SUCCESS) return;
  if (!dat.pixel_data) return;
  if (dat.stride <= 0 || (dat.stride % 4) != 0 || dat.stride < (intptr_t)devW * 4) return;
  const uint8_t* srcBase = (const uint8_t*)dat.pixel_data;
  for (int row = 0; row < devH; ++row) {
    const unsigned int* s = (const unsigned int*)(srcBase + (intptr_t)row * dat.stride);
    unsigned int* d = fbuf + (size_t)row * (size_t)devW;
    for (int col2 = 0; col2 < devW; ++col2)
      d[col2] = s[col2] | 0xFF000000u;
  }

  BitBlt(hdc, x, y, devW, devH, m_memDC, 0, 0, SRCCOPY);
}
