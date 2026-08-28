// dyn_trace.h — the Dynamics detector as a chunked, resumable pass (v2.5 8f)
// See .harness/design_dynamics_stream.md. DynamicsEngine used to scan the whole
// sample buffer (CollectPeaks + DeEssBandTrace) on every analysis; on long items
// that buffer is downsampled (or, after 8g, absent). The detector now lives here:
// fed sequentially with interleaved frames of ANY chunk size - the whole buffer
// in one call, or 64k-frame reads from an AudioStream on a worker - it emits the
// same 1 ms lanes bit for bit (window max is order-free, the RMS sum is redone
// per step in the legacy order from a history of the last W frames, biquad state
// carries per sample). Pure computation: no REAPER API, no GDI.
#pragma once

#include "deess_engine.h"
#include <cstdint>
#include <memory>
#include <vector>

// What a trace was built for. Detector params only: everything else in
// DynamicsParams (thresh/ratio/attack/...) works on the finished trace.
struct DynTraceKey {
  int sampleRate = 0, numChannels = 0;
  int64_t numFrames = 0;
  bool rmsMode = false;
  double rmsWindowMs = 0.0;
  bool dsEnable = false;
  int dsMode = 0;
  double dsFreqHz = 0.0, dsQ = 0.0;
  unsigned long long contentHash = 0;   // buffer path only (sparse FNV), 0 for streams
  bool operator==(const DynTraceKey& o) const
  {
    return sampleRate == o.sampleRate && numChannels == o.numChannels &&
           numFrames == o.numFrames && rmsMode == o.rmsMode &&
           rmsWindowMs == o.rmsWindowMs && dsEnable == o.dsEnable && dsMode == o.dsMode &&
           dsFreqHz == o.dsFreqHz && dsQ == o.dsQ && contentHash == o.contentHash;
  }
};

// Immutable once built; shared by every engine that analyses the same audio.
struct DynTrace {
  DynTraceKey key;
  int samplesPerStep = 1;
  std::vector<double> peak;   // per step: max|x| over channels, or RMS in rms mode
  std::vector<double> band;   // per step: de-ess band max|y|; empty when ds is off
  size_t Count() const { return peak.size(); }
  // Same expression as the legacy CollectPeaks: (double)frame / (double)rate.
  double TimeAt(size_t i) const
  {
    return (double)((int64_t)i * samplesPerStep) / (double)key.sampleRate;
  }
};

class DynTraceBuilder {
public:
  void Begin(const DynTraceKey& key);
  // Sequential frames (interleaved, key.numChannels wide), any n >= 1.
  void Feed(const double* frames, int n);
  // Flushes the partial last window. Fewer frames than key.numFrames = the
  // trace covers what was fed. The builder is empty afterwards.
  std::shared_ptr<const DynTrace> Finish();
  int64_t FramesFed() const { return m_fed; }

private:
  void EmitRmsSteps(int64_t numFrames);

  std::shared_ptr<DynTrace> m_trace;
  int m_nch = 1, m_step = 1;
  int64_t m_fed = 0;
  // peak + band lanes: one window, its fill and running maxima
  int m_winFill = 0;
  double m_winMax = 0.0, m_bandMax = 0.0;
  // rms lane: legacy centred window [max(0, f - W/2), min(N, start + W))
  int m_rmsWindow = 0;
  std::vector<double> m_hist;   // interleaved frames from m_histStart to m_fed
  int64_t m_histStart = 0;
  int64_t m_nextRmsStep = 0;
  // band lane: per-channel filter chain (BP = 1 section, HP = Butterworth pair)
  int m_nStages = 0;
  std::vector<DeEssBiquad> m_filt;
};
