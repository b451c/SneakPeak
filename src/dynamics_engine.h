// dynamics_engine.h — Professional dynamics processor for SneakPeak
// Standard compressor: threshold, ratio, knee, attack/release, makeup gain.
// Pure computation — no REAPER API calls, no GDI.
#pragma once

#include <vector>
#include <cmath>

struct DynamicsParams {
  // Compressor controls (standard)
  double threshold = -100.0;  // dB (-100 sentinel = use average peak)
  double ratio = 4.0;         // compression ratio (1.0 = off, 20.0 = limiter)
  double kneeDb = 6.0;        // soft knee width in dB (0 = hard knee)
  double makeupDb = 0.0;      // output makeup gain in dB
  bool autoMakeup = true;     // auto-compute makeup from average GR

  // Envelope follower
  double attackMs = 5.0;
  double releaseMs = 100.0;
  double lookaheadMs = 0.0;

  // Detection mode
  bool rmsMode = false;       // false = peak, true = RMS
  double rmsWindowMs = 5.0;   // RMS averaging window

  // Gate (applied after compression + makeup to catch boosted breaths)
  double gateThreshDb = -100.0;  // dB (-100 = gate off, below any signal)
  double gateRangeDb = -20.0;    // max reduction in dB (not full mute)
  double gateHoldMs = 50.0;      // hold time before gate closes (ms)

  // Visual range (normalization for curve display)
  double minDb = -60.0;
  double maxDb = 6.0;
};

struct DynamicsPoint {
  double time;       // seconds from item start
  double peakLinear; // raw peak amplitude (0..1+)
  double db;         // smoothed amplitude in dB (includes item vol) - for orange curve
  double norm;       // normalized 0..1 within minDb..maxDb range
  double smoothedGR; // gain-smoothed GR in dB (negative, 0=no compression) - set by ComputeCompression
};

// Built-in presets (researched from professional sources)
struct DynamicsPreset {
  const char* name;
  DynamicsParams params;
};

// Preset index constants
enum {
  PRESET_DEFAULT = 0,
  PRESET_GENTLE,
  PRESET_VOICE,
  PRESET_BROADCAST,
  PRESET_DEBREATH,
  PRESET_MUSIC_BUS,
  PRESET_COUNT
};

extern const DynamicsPreset g_dynamicsPresets[PRESET_COUNT];

// P_EXT serialization key
static constexpr const char* PEXT_DYNAMICS_KEY = "P_EXT:SneakPeak_Dynamics";

// Serialize/deserialize DynamicsParams to/from compact string
void DynamicsParamsToString(const DynamicsParams& p, char* buf, int bufSize);
bool DynamicsParamsFromString(const char* str, DynamicsParams& out);

class DynamicsEngine {
public:
  void Analyze(const double* audioData, int numFrames, int numChannels,
               int sampleRate, double itemVolDb, const DynamicsParams& params);

  const std::vector<DynamicsPoint>& GetResults() const { return m_results; }
  double GetAveragePeakDb() const { return m_avgPeakDb; }
  double GetThreshold() const;
  bool HasResults() const { return !m_results.empty(); }
  void Clear() { m_results.clear(); }
  const DynamicsParams& GetParams() const { return m_params; }
  void SetParams(const DynamicsParams& p) { m_params = p; }

  // Compute gain reduction per point (gain-smoothing compressor model).
  // Attack/release smooth the GR signal, not the input level.
  // Also populates smoothedGR on each DynamicsPoint for rendering.
  // Returns {time, dbAdjustment} pairs. Includes makeup gain.
  struct CompressPoint {
    double time;
    double dbAdjust; // total dB change (GR + makeup)
  };
  std::vector<CompressPoint> ComputeCompression();

  // Average gain reduction from last ComputeCompression (for GR meter, auto-makeup)
  double GetAvgGainReduction() const { return m_avgGR; }

  // Ramer-Douglas-Peucker curve simplification
  static std::vector<CompressPoint> SimplifyCurve(const std::vector<CompressPoint>& pts,
                                                  double epsilonDb);

private:
  void CollectPeaks(const double* audioData, int numFrames, int numChannels,
                    int sampleRate, bool rmsMode, double rmsWindowMs);
  void BuildEnvelope(double itemVolDb, const DynamicsParams& params);

  struct RawPeak {
    double time;
    double peak;
  };

  std::vector<RawPeak> m_rawPeaks;
  std::vector<DynamicsPoint> m_results;
  double m_avgPeakDb = -60.0;
  double m_avgGR = 0.0;
  double m_itemVolDb = 0.0; // cached for ComputeCompression (gain smoothing needs raw dB)
  DynamicsParams m_params;
};
