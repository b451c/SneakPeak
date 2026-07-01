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

  // Gate extension (v2.3.0) - APPENDED AT END on purpose: g_dynamicsPresets
  // uses positional aggregate init (15 legacy values per row); trailing NSDMI
  // fields keep those rows compiling with legacy-equivalent behavior. Never
  // reorder or insert fields above this line.
  double gateRatio = 2.0;       // downward-expander ratio Rg (output slope Rg:1 below G.Thr; 2.0 = legacy)
  double gateHystDb = 0.0;      // close threshold relative to G.Thr (<= 0; 0 = legacy single threshold)
  double gateAttackMs = 2.0;    // gate open speed (legacy hard-coded constant)
  double gateReleaseMs = 100.0; // gate close speed (legacy hard-coded constant)

  // Upward compression (v2.3.0 INC-2) - same append-at-end rule as above.
  // 0 = Down (classic, legacy), 1 = Up (boost below threshold), 2 = Both
  // (leveler: Down above + Up below in one pass; the summed knee quadratics
  // collapse to an exactly linear S*(x-T) through the threshold).
  // Serialized as "up=" (old strings: absent -> 0; 0/1 keep their meaning).
  int compMode = 0;
  double maxBoostDb = 8.0;      // boost cap in Up/Both (mandatory - uncapped boosts the noise floor)
};

enum { COMP_MODE_DOWN = 0, COMP_MODE_UP = 1, COMP_MODE_BOTH = 2 };

// Static-curve slope factor S = 1/R - 1, extended ratio encoding (v2.3.0):
//   r >= 1  -> classic compression, S in (-1, 0] (bit-identical to the legacy
//              1.0 / max(1.0, r) - 1.0 for the legacy range)
//   r == 0  -> Inf:1 sentinel (true limiting, S = -1)
//   r <  0  -> over-compression (saxmand): reduction EXCEEDS the overshoot,
//              output slope goes negative; r = -2 -> S = -1.5, r = -1 -> S = -2
// Old binaries reading a new P_EXT string degrade gracefully: max(1.0, r)
// clamps 0/negative to 1 -> compression off, never garbage.
inline double DynSlopeFromRatio(double r)
{
  if (r == 0.0) return -1.0;
  if (r < 0.0) return 1.0 / r - 1.0;
  return 1.0 / (r < 1.0 ? 1.0 : r) - 1.0;
}

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
  PRESET_UPWARD,     // v2.3.0: upward leveling (Up mode showcase)
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
