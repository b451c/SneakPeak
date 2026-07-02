// deess_engine.h — De-esser sidechain detector (v2.3.0 INC-3)
// Pure computation — no REAPER API calls, no GDI (dynamics_engine contract).
//
// Wideband de-esser topology: a band-filtered sidechain (RBJ biquad band-pass
// or 24 dB/oct Butterworth high-pass cascade) measures the sibilance band on
// the same 1 ms grid as DynamicsEngine::CollectPeaks; the band trace drives a
// third gain-reduction pass inside ComputeCompression. The whole signal ducks
// (take volume envelope) — the classic broadband de-esser design, NOT a
// dynamic EQ (honest-docs rule).
//
// Filter math: RBJ "Audio EQ Cookbook" (W3C edition) — formulas reimplemented
// from the specification. Double-precision Direct Form 1.
#pragma once

#include <vector>

// One RBJ biquad section, Direct Form 1, double precision.
struct DeEssBiquad {
  double b0 = 1.0, b1 = 0.0, b2 = 0.0; // normalized by a0
  double a1 = 0.0, a2 = 0.0;
  double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

  // Constant-0dB-peak-gain band-pass (RBJ cookbook).
  void SetBandpass(double fs, double f0, double q);
  // High-pass with explicit section Q (RBJ cookbook).
  void SetHighpass(double fs, double f0, double q);
  void Reset() { x1 = x2 = y1 = y2 = 0.0; }

  inline double Process(double x)
  {
    double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    return y;
  }
};

enum { DEESS_MODE_BANDPASS = 0, DEESS_MODE_HIGHPASS = 1 };

// Band-level trace: max |filtered| per channel per stepSec window, window loop
// IDENTICAL to DynamicsEngine::CollectPeaks so index i of the result aligns
// 1:1 with m_rawPeaks[i] / m_results[i].
//
// mode = DEESS_MODE_BANDPASS: single RBJ band-pass at (f0, q).
// mode = DEESS_MODE_HIGHPASS: 4th-order Butterworth high-pass at f0 — a
// cascade of two RBJ sections with Q = 0.54119610 and 1.30656296 (the exact
// maximally-flat pair 1/(2cos(3pi/8)), 1/(2cos(pi/8))), q ignored.
// f0 is clamped to [200 Hz, 0.45 * sampleRate]. Filters start at zero state
// (first ~1-2 ms read low — attenuation-safe).
void DeEssBandTrace(const double* audioData, int numFrames, int numChannels,
                    int sampleRate, double stepSec, int mode, double f0,
                    double q, std::vector<double>& outBandPeaks);
