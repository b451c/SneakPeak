// deess_engine.cpp — De-esser sidechain detector (v2.3.0 INC-3)
// RBJ biquad filters (W3C Audio EQ Cookbook, reimplemented from the spec) +
// band-level trace on the DynamicsEngine 1 ms analysis grid.
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

// Exact 4th-order Butterworth section Qs: 1/(2cos(3pi/8)), 1/(2cos(pi/8)).
// A naive same-Q double application would droop the knee by 6 dB; this pair is
// maximally flat in the passband at exactly 24 dB/oct.
static constexpr double BUTTERWORTH4_Q1 = 0.54119610014619698;
static constexpr double BUTTERWORTH4_Q2 = 1.30656296487637653;

void DeEssBandTrace(const double* audioData, int numFrames, int numChannels,
                    int sampleRate, double stepSec, int mode, double f0,
                    double q, std::vector<double>& outBandPeaks)
{
  outBandPeaks.clear();
  if (!audioData || numFrames <= 0 || numChannels <= 0 || sampleRate <= 0) return;

  const int nch = std::max(1, numChannels);
  const double fs = (double)sampleRate;
  f0 = std::max(200.0, std::min(0.45 * fs, f0));
  q = std::max(0.1, std::min(16.0, q));

  // Per-channel filter chains: BP = one section, HP = Butterworth pair.
  const int nStages = (mode == DEESS_MODE_HIGHPASS) ? 2 : 1;
  std::vector<DeEssBiquad> filt((size_t)nch * (size_t)nStages);
  for (int ch = 0; ch < nch; ch++) {
    if (mode == DEESS_MODE_HIGHPASS) {
      filt[(size_t)ch * 2 + 0].SetHighpass(fs, f0, BUTTERWORTH4_Q1);
      filt[(size_t)ch * 2 + 1].SetHighpass(fs, f0, BUTTERWORTH4_Q2);
    } else {
      filt[(size_t)ch].SetBandpass(fs, f0, q);
    }
  }

  // Window loop mirrors DynamicsEngine::CollectPeaks exactly, so the trace
  // index aligns 1:1 with the raw-peak / results grid.
  const int samplesPerStep = std::max(1, (int)(stepSec * sampleRate));
  outBandPeaks.reserve((size_t)(numFrames / samplesPerStep + 1));

  for (int frame = 0; frame < numFrames; frame += samplesPerStep) {
    int windowEnd = std::min(numFrames, frame + samplesPerStep);
    double value = 0.0;
    for (int s = frame; s < windowEnd; s++) {
      for (int ch = 0; ch < nch; ch++) {
        double y = audioData[(size_t)s * nch + ch];
        DeEssBiquad* f = &filt[(size_t)ch * (size_t)nStages];
        for (int st = 0; st < nStages; st++) y = f[st].Process(y);
        double a = std::fabs(y);
        if (a > value) value = a;
      }
    }
    outBandPeaks.push_back(value);
  }
}
