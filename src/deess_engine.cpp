// deess_engine.cpp — De-esser sidechain filter (v2.3.0 INC-3)
// RBJ biquad filters (W3C Audio EQ Cookbook, reimplemented from the spec).
#include "deess_engine.h"
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// RBJ cookbook, band-pass (constant 0 dB peak gain):
//   b0 = alpha, b1 = 0, b2 = -alpha; a0 = 1+alpha, a1 = -2cos(w0), a2 = 1-alpha
void DeEssBiquad::SetBandpass(double fs, double f0, double q)
{
  double w0 = 2.0 * M_PI * f0 / fs;
  double alpha = std::sin(w0) / (2.0 * q);
  double cw = std::cos(w0);
  double a0 = 1.0 + alpha;
  b0 = alpha / a0;
  b1 = 0.0;
  b2 = -alpha / a0;
  a1 = -2.0 * cw / a0;
  a2 = (1.0 - alpha) / a0;
}

// RBJ cookbook, high-pass:
//   b0 = (1+cos w0)/2, b1 = -(1+cos w0), b2 = (1+cos w0)/2
//   a0 = 1+alpha, a1 = -2cos(w0), a2 = 1-alpha
void DeEssBiquad::SetHighpass(double fs, double f0, double q)
{
  double w0 = 2.0 * M_PI * f0 / fs;
  double alpha = std::sin(w0) / (2.0 * q);
  double cw = std::cos(w0);
  double a0 = 1.0 + alpha;
  b0 = (1.0 + cw) / (2.0 * a0);
  b1 = -(1.0 + cw) / a0;
  b2 = (1.0 + cw) / (2.0 * a0);
  a1 = -2.0 * cw / a0;
  a2 = (1.0 - alpha) / a0;
}

int DeEssSetupChain(std::vector<DeEssBiquad>& filt, int nch, double fs, int mode,
                    double f0, double q)
{
  nch = std::max(1, nch);
  f0 = std::max(200.0, std::min(0.45 * fs, f0));
  q = std::max(0.1, std::min(16.0, q));
  const int nStages = (mode == DEESS_MODE_HIGHPASS) ? 2 : 1;
  filt.assign((size_t)nch * (size_t)nStages, DeEssBiquad());
  for (int ch = 0; ch < nch; ch++) {
    if (mode == DEESS_MODE_HIGHPASS) {
      filt[(size_t)ch * 2 + 0].SetHighpass(fs, f0, DEESS_BUTTERWORTH4_Q1);
      filt[(size_t)ch * 2 + 1].SetHighpass(fs, f0, DEESS_BUTTERWORTH4_Q2);
    } else {
      filt[(size_t)ch].SetBandpass(fs, f0, q);
    }
  }
  return nStages;
}

void DeEssApplySplit(const double* in, double* out, int frames, int nch, double fs,
                     int mode, double f0, double q, const std::vector<double>& gWide,
                     const std::vector<double>& gBand, int stepFrames)
{
  if (frames <= 0 || nch <= 0 || gWide.empty() || gWide.size() != gBand.size()) return;
  const size_t n = (size_t)frames, ch = (size_t)nch;
  std::vector<DeEssBiquad> filt;
  const size_t nStages = (size_t)DeEssSetupChain(filt, nch, fs, mode, f0, q);
  auto run = [&](size_t c, double v) {
    for (size_t s = 0; s < nStages; s++) v = filt[c * nStages + s].Process(v);
    return v;
  };
  // Band signal into `out`: forward pass, then backward over the result with
  // fresh states (zero phase, |B|^2). Both start at rest, so the first and the
  // last millisecond read low - attenuation-safe, like the detector lane.
  for (size_t i = 0; i < n; i++)
    for (size_t c = 0; c < ch; c++) out[i * ch + c] = run(c, in[i * ch + c]);
  for (DeEssBiquad& f : filt) f.Reset();
  for (size_t i = n; i-- > 0;)
    for (size_t c = 0; c < ch; c++) out[i * ch + c] = run(c, out[i * ch + c]);

  const size_t last = gWide.size() - 1;
  const size_t step = (size_t)std::max(1, stepFrames);
  for (size_t i = 0; i < n; i++) {
    size_t k = i / step;
    if (k > last) k = last;
    // Same lerp as the wideband apply: a = (t - t_k) / (t_k+1 - t_k) on the grid.
    double gW = gWide[k], gB = gBand[k];
    if (k < last) {
      const double a = (double)(i - k * step) / (double)step;
      gW += (gWide[k + 1] - gW) * a;
      gB += (gBand[k + 1] - gB) * a;
    }
    const double cut = 1.0 - gB;
    for (size_t c = 0; c < ch; c++)
      out[i * ch + c] = (in[i * ch + c] - cut * out[i * ch + c]) * gW;
  }
}
