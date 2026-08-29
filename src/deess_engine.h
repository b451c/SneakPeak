// deess_engine.h — De-esser sidechain filter (v2.3.0 INC-3)
// Pure computation — no REAPER API calls, no GDI (dynamics_engine contract).
//
// Wideband de-esser topology: a band-filtered sidechain (RBJ biquad band-pass
// or 24 dB/oct Butterworth high-pass cascade) measures the sibilance band on
// the same 1 ms grid as the peak detector (DynTraceBuilder, dyn_trace.h, runs
// both lanes); the band trace drives a third gain-reduction pass inside
// ComputeCompression. The whole signal ducks (take volume envelope) — the
// classic broadband de-esser design, NOT a dynamic EQ (honest-docs rule).
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
    // A silence never brings the state to zero by itself (it settles on the
    // smallest subnormal); flush it below 1e-30 (-600 dB) so the tail of a
    // long silence is not a denormal grind on x86 (audit A7.3).
    if (y > -1e-30 && y < 1e-30) y = 0.0;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    return y;
  }
};

enum { DEESS_MODE_BANDPASS = 0, DEESS_MODE_HIGHPASS = 1 };

// Exact 4th-order Butterworth section Qs: 1/(2cos(3pi/8)), 1/(2cos(pi/8)).
// A naive same-Q double application would droop the knee by 6 dB; this pair is
// maximally flat in the passband at exactly 24 dB/oct. Shared with dyn_trace.
constexpr double DEESS_BUTTERWORTH4_Q1 = 0.54119610014619698;
constexpr double DEESS_BUTTERWORTH4_Q2 = 1.30656296487637653;
// Band lane (DynTraceBuilder): DEESS_MODE_BANDPASS = one RBJ band-pass at
// (f0, q); DEESS_MODE_HIGHPASS = 4th-order Butterworth high-pass at f0, the
// cascade of two sections with the Qs above, q ignored. f0 is clamped to
// [200 Hz, 0.45 * sampleRate]. Filters start at zero state (first ~1-2 ms read
// low — attenuation-safe).

// The detector chain for nch channels, filt[ch * nStages + s]: BP = one RBJ
// band-pass at (f0, q); HP = the Butterworth pair at f0 (q ignored). f0 is
// clamped to [200 Hz, 0.45 * fs], q to [0.1, 16]. Returns nStages. Shared by
// DynTraceBuilder (the band lane) and DeEssApplySplit, so the band that is
// measured is exactly the band that is cut.
int DeEssSetupChain(std::vector<DeEssBiquad>& filt, int nch, double fs, int mode,
                    double f0, double q);

// Split-band apply (v2.5.0 row 15, Standalone Apply): per sample
//   y = (x - (1 - gBand) * b) * gWide,   b = the band signal
// b is the INPUT run through the detector chain forward AND backward (zero
// phase, magnitude |B|^2 - the buffer is whole, so this offline luxury is
// free). Zero phase is load-bearing: subtracting a causal band signal boosts
// wherever its phase nears 180 deg (the Butterworth HP pair sits at 180 deg
// exactly at f0: x - c * HP4(x) came out +3.4 dB there), while with a real,
// non-negative |B|^2 the output magnitude is 1 - c * |B|^2 in [gBand, 1] -
// a dynamic EQ cut in the detector's band that can never boost. Pre-ringing
// is a fraction of a millisecond at de-ess frequencies. gBand / gWide are
// linear gains on the analysis grid (step k covers frames [k * stepFrames,
// (k + 1) * stepFrames)), interpolated linearly between steps exactly like
// the wideband Standalone apply. gBand == 1 -> x untouched (0 * b == 0), so
// wherever the de-esser is idle the output equals the wideband path. `out`
// doubles as the band buffer (no third buffer on a 1 GB Standalone file).
void DeEssApplySplit(const double* in, double* out, int frames, int nch, double fs,
                     int mode, double f0, double q, const std::vector<double>& gWide,
                     const std::vector<double>& gBand, int stepFrames);
