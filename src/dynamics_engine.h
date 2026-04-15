// dynamics_engine.h — Peak analysis and envelope smoothing for SneakPeak
// Translated from saxmand's Lua amplitude analysis code.
// Pure computation — no REAPER API calls, no GDI.
#pragma once

#include <vector>
#include <cmath>

struct DynamicsParams {
  double attackMs = 5.0;      // envelope attack time
  double releaseMs = 100.0;   // envelope release time
  double lookaheadMs = 0.0;   // lookahead for peak detection
  double minDb = -60.0;       // visual floor (normalization)
  double maxDb = 6.0;         // visual ceiling (normalization)
};

struct DynamicsPoint {
  double time;       // seconds from item start
  double peakLinear; // raw peak amplitude (0..1+)
  double db;         // smoothed amplitude in dB (includes item vol)
  double norm;       // normalized 0..1 within minDb..maxDb range
};

class DynamicsEngine {
public:
  // Analyze audio buffer: collect peaks then build smoothed envelope.
  // audioData: interleaved samples [frame * numChannels + ch]
  // itemVolDb: item volume in dB (added to smoothed dB values)
  void Analyze(const double* audioData, int numFrames, int numChannels,
               int sampleRate, double itemVolDb, const DynamicsParams& params);

  const std::vector<DynamicsPoint>& GetResults() const { return m_results; }
  double GetAveragePeakDb() const { return m_avgPeakDb; }
  bool HasResults() const { return !m_results.empty(); }
  void Clear() { m_results.clear(); }
  const DynamicsParams& GetParams() const { return m_params; }

private:
  // Stage 1: collect peak per 1ms window from audio buffer
  void CollectPeaks(const double* audioData, int numFrames, int numChannels,
                    int sampleRate);

  // Stage 2: asymmetric attack/release follower with lookahead
  void BuildEnvelope(double itemVolDb, const DynamicsParams& params);

  struct RawPeak {
    double time;
    double peak; // max(abs(L), abs(R)) in window
  };

  std::vector<RawPeak> m_rawPeaks;
  std::vector<DynamicsPoint> m_results;
  double m_avgPeakDb = -60.0;
  DynamicsParams m_params;
};
