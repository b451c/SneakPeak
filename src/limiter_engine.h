// limiter_engine.h — True-Peak Hard Limiter DSP core (v2.4.0 INC-L0)
// Pure computation — no REAPER API calls, no GDI (same contract as
// dynamics_engine / spectral_repair).
//
// Design: Signalsmith limiter chain (Geraint Luff, 2022) — sliding minimum of
// the needed gain over the attack window, cascaded-box (BoxStackFilter)
// smoothing, hold + cascaded exponential release, dry signal aligned by the
// chain latency. Sample-domain overshoot is impossible by construction: the
// smoothed envelope at chain index i is a convex average of sliding minima
// whose windows all contain need[i - D], so env never exceeds the gain the
// delayed sample requires. In the INTERPOLATED domain construction alone is
// not enough (an envelope step de-weights cancelling kernel terms and can
// grow new inter-sample peaks), so with true peak enabled the engine closes
// the loop offline: re-measure the output with the detector, tighten, re-run,
// until the detector reads at/below the ceiling.
// True-peak detection is the BS.1770-family windowed-sinc
// polyphase interpolator, upsized from the standard 4x/49-tap to 8x/97-tap:
// the 4x minimum under-reads broadband inter-sample peaks by up to ~0.3 dB
// (fine for a meter, not for a hard ceiling), while the 8x grid strictly
// dominates any BS.1770-4 4x compliance meter. Offline, the cost is free.
//
// Licensing: math ported natively from MIT sources — Signalsmith DSP library
// (github.com/Signalsmith-Audio/dsp: PeakHold, BoxStackFilter ratios) and
// libebur128 (github.com/jiixyj/libebur128: interpolator coefficients). No
// code vendored; behavior references: signalsmith-audio.co.uk/writing/2022/
// limiter/, ITU-R BS.1770-4/-5.
#pragma once

#include <vector>

struct LimiterParams {
  double gainDb = 0.0;      // input gain (push into the ceiling), 0..+24
  double ceilingDb = -1.0;  // dBTP when truePeak, dBFS otherwise; -12..0
  double attackMs = 5.0;    // lookahead = smoothing window, 0.1..30
  double holdMs = 10.0;     // flat-gain hold before release, 0..50
  double releaseMs = 60.0;  // cascaded exponential release, 10..1000
  bool truePeak = true;     // 4x/2x polyphase inter-sample peak detection
  bool link = true;         // stereo max-link vs per-channel envelopes
};

struct LimiterResult {
  bool ok = false;             // false: bad args (null/empty buffer)
  double inputPeakDb = -999.0; // post-gain peak, detector domain (dBTP/dBFS)
  double outputPeakDb = -999.0;// same detector on the processed output
  double maxGainReductionDb = 0.0; // deepest envelope dip, positive dB
  int latencySamples = 0;      // chain alignment D (informational; already
                               // compensated — output is time-aligned)
};

// Harness-only taps: raw chain stages at chain index i (NOT delay-aligned).
// need = min(1, ceiling/peak); e1 = sliding min; e2 = box stack; e3 = final
// (post hold+release). With link=false the vectors interleave one value per
// chain: [i * numChains + chain].
struct LimiterDebugTaps {
  std::vector<double> need, e1, e2, e3;
  int latencySamples = 0;
};

// Compute the per-frame gain envelope WITHOUT touching the audio buffer.
// audio = interleaved doubles [frame * numChannels + channel]. envOut is
// resized to numFrames (link=true or mono) or numFrames * numChannels
// interleaved (link=false). Envelope values are in (0, 1]; the caller applies
// out = in * gainLin * env. edgeRampFrames > 0 forces the envelope to 1.0 at
// both buffer edges with a linear blend over that many frames (selection-apply
// handoff — the ramp region may exceed the ceiling by design).
// detectorPeaksOut (optional): receives the PRE-GAIN per-frame detector peaks,
// one value per frame per chain ([frame * numChains + chain]) — a cache for
// LimiterEnvelopeFromPeaks. Valid for this buffer + truePeak/link combo;
// independent of gain/ceiling/attack/hold/release. Kept double so the draft
// path is bit-exact against this function's own need computation.
LimiterResult LimiterComputeEnvelope(const double* audio, int numFrames,
                                     int numChannels, int sampleRate,
                                     const LimiterParams& params,
                                     std::vector<double>& envOut,
                                     int edgeRampFrames = 0,
                                     LimiterDebugTaps* debugTaps = nullptr,
                                     std::vector<double>* detectorPeaksOut = nullptr);

// DRAFT envelope from cached detector peaks: need -> chain -> edge ramp, but
// NO true-peak refinement loop and no output measure (both need the audio).
// The knob-drag live preview path: visually identical GR trace (refinement
// tightens by fractions of a dB), orders of magnitude cheaper than detection.
// numChains must match the cache ((link || mono) ? 1 : channels); outputPeakDb
// stays unset. Apply and the settled preview keep the refined guarantee.
LimiterResult LimiterEnvelopeFromPeaks(const double* peaks, int numFrames,
                                       int numChains, int sampleRate,
                                       const LimiterParams& params,
                                       std::vector<double>& envOut,
                                       int edgeRampFrames = 0);

// Apply the limiter in place: gain -> envelope -> multiply. Two-pass offline
// (exact, no streaming state). Returns the same stats as ComputeEnvelope plus
// the measured output peak.
LimiterResult LimiterProcess(double* audio, int numFrames, int numChannels,
                             int sampleRate, const LimiterParams& params,
                             int edgeRampFrames = 0);

// Peak of a buffer in the detector domain, linear amplitude: BS.1770-4
// true peak (truePeak) or plain sample peak. Exposed for panel readouts and
// the offline harness.
double LimiterMeasurePeak(const double* audio, int numFrames, int numChannels,
                          int sampleRate, bool truePeak);
