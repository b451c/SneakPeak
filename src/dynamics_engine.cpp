// dynamics_engine.cpp — Professional dynamics processor
// Standard compressor: threshold + ratio + soft knee + attack/release + makeup
#include "dynamics_engine.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>

static constexpr double STEP_SIZE = 0.001; // 1ms analysis windows
static constexpr double EPSILON = 1e-12;

// --- Built-in presets (researched from iZotope, Waves, EBU R128, BBC guidelines) ---

const DynamicsPreset g_dynamicsPresets[PRESET_COUNT] = {
  { "Default", { 0.0, 1.0, 0.0, 0.0, false, 10.0, 100.0, 0.0, false, 5.0, -100.0, -20.0, 50.0, -60.0, 6.0 } },
  { "Gentle Leveling", { -18.0, 2.0, 12.0, 0.0, true, 20.0, 150.0, 3.0, true, 5.0, -100.0, -40.0, 50.0, -60.0, 6.0 } },
  { "Voice / Podcast", { -24.0, 4.0, 6.0, 0.0, true, 5.0, 80.0, 5.0, true, 5.0, -45.0, -18.0, 80.0, -60.0, 6.0 } },
  { "Broadcast", { -20.0, 6.0, 3.0, 0.0, true, 1.0, 50.0, 5.0, true, 5.0, -40.0, -24.0, 60.0, -60.0, 6.0 } },
  { "De-breath", { -18.0, 2.0, 8.0, 0.0, true, 10.0, 100.0, 5.0, false, 5.0, -35.0, -12.0, 30.0, -60.0, 6.0 } },
  { "Music Bus", { -16.0, 2.0, 9.0, 0.0, true, 30.0, 200.0, 0.0, true, 5.0, -100.0, -40.0, 50.0, -60.0, 6.0 } },
};

// --- P_EXT serialization ---

void DynamicsParamsToString(const DynamicsParams& p, char* buf, int bufSize)
{
  snprintf(buf, bufSize,
    "t=%.1f r=%.1f k=%.1f m=%.1f am=%d a=%.1f re=%.1f la=%.1f rms=%d "
    "gt=%.1f gr=%.1f gh=%.1f",
    p.threshold, p.ratio, p.kneeDb, p.makeupDb, p.autoMakeup ? 1 : 0,
    p.attackMs, p.releaseMs, p.lookaheadMs, p.rmsMode ? 1 : 0,
    p.gateThreshDb, p.gateRangeDb, p.gateHoldMs);
}

bool DynamicsParamsFromString(const char* str, DynamicsParams& out)
{
  if (!str || !str[0]) return false;
  out = DynamicsParams{}; // start with defaults
  auto readKey = [&](const char* key, double& val) {
    char search[16];
    snprintf(search, sizeof(search), "%s=", key);
    const char* p = strstr(str, search);
    if (p) val = atof(p + strlen(search));
  };
  double am = 1.0, rms = 0.0;
  readKey("t", out.threshold);
  readKey("r", out.ratio);
  readKey("k", out.kneeDb);
  readKey("m", out.makeupDb);
  readKey("am", am); out.autoMakeup = (am > 0.5);
  readKey("a", out.attackMs);
  readKey("re", out.releaseMs);
  readKey("la", out.lookaheadMs);
  readKey("rms", rms); out.rmsMode = (rms > 0.5);
  readKey("gt", out.gateThreshDb);
  readKey("gr", out.gateRangeDb);
  readKey("gh", out.gateHoldMs);
  return true;
}

void DynamicsEngine::Analyze(const double* audioData, int numFrames, int numChannels,
                             int sampleRate, double itemVolDb, const DynamicsParams& params)
{
  m_params = params;
  m_results.clear();
  m_rawPeaks.clear();
  m_avgGR = 0.0;
  m_itemVolDb = itemVolDb;

  if (!audioData || numFrames <= 0 || sampleRate <= 0) return;

  CollectPeaks(audioData, numFrames, numChannels, sampleRate,
               params.rmsMode, params.rmsWindowMs);
  if (m_rawPeaks.empty()) return;

  // Average peak dB (for auto-threshold when sentinel -100)
  double sumPeak = 0.0;
  for (const auto& p : m_rawPeaks) sumPeak += p.peak;
  double meanLinear = sumPeak / (double)m_rawPeaks.size();
  m_avgPeakDb = 20.0 * log10(std::max(meanLinear, EPSILON)) + itemVolDb;

  BuildEnvelope(itemVolDb, params);
}

// Stage 1: Collect peaks from audio buffer in 1ms windows
void DynamicsEngine::CollectPeaks(const double* audioData, int numFrames, int numChannels,
                                  int sampleRate, bool rmsMode, double rmsWindowMs)
{
  int samplesPerStep = std::max(1, (int)(STEP_SIZE * sampleRate));
  int nch = std::max(1, numChannels);

  // RMS: wider window for averaging (default 5ms, but never smaller than step)
  int rmsWindow = rmsMode
    ? std::max(samplesPerStep, (int)(rmsWindowMs / 1000.0 * sampleRate))
    : 0;

  m_rawPeaks.reserve((size_t)(numFrames / samplesPerStep + 1));

  for (int frame = 0; frame < numFrames; frame += samplesPerStep) {
    int windowEnd = std::min(numFrames, frame + samplesPerStep);
    double value = 0.0;

    if (rmsMode) {
      // RMS: root mean square over rmsWindow centered on this step
      int rmsStart = std::max(0, frame - rmsWindow / 2);
      int rmsEnd = std::min(numFrames, rmsStart + rmsWindow);
      double sumSq = 0.0;
      int count = 0;
      for (int s = rmsStart; s < rmsEnd; s++) {
        for (int ch = 0; ch < nch; ch++) {
          double v = audioData[(size_t)s * nch + ch];
          sumSq += v * v;
        }
        count++;
      }
      value = (count > 0) ? sqrt(sumSq / (double)(count * nch)) : 0.0;
    } else {
      // Peak: max(abs) per channel
      for (int s = frame; s < windowEnd; s++) {
        for (int ch = 0; ch < nch; ch++) {
          double v = fabs(audioData[(size_t)s * nch + ch]);
          if (v > value) value = v;
        }
      }
    }

    double time = (double)frame / (double)sampleRate;
    m_rawPeaks.push_back({time, value});
  }
}

// Stage 2: Asymmetric attack/release envelope follower with lookahead
void DynamicsEngine::BuildEnvelope(double itemVolDb, const DynamicsParams& params)
{
  if (m_rawPeaks.empty()) return;

  double dt = (m_rawPeaks.size() > 1)
    ? (m_rawPeaks[1].time - m_rawPeaks[0].time) : STEP_SIZE;

  double attCoeff = (params.attackMs > 0.0)
    ? exp(-dt / (params.attackMs / 1000.0)) : 0.0;
  double relCoeff = (params.releaseMs > 0.0)
    ? exp(-dt / (params.releaseMs / 1000.0)) : 0.0;

  int lookaheadFrames = (params.lookaheadMs > 0.0)
    ? (int)(params.lookaheadMs / 1000.0 / dt + 0.5) : 0;

  int count = (int)m_rawPeaks.size();
  m_results.resize((size_t)count);

  double env = m_rawPeaks[0].peak;

  for (int i = 0; i < count; i++) {
    double x = m_rawPeaks[i].peak;

    if (lookaheadFrames > 0) {
      int ahead_end = std::min(count, i + 1 + lookaheadFrames);
      for (int j = i + 1; j < ahead_end; j++) {
        if (m_rawPeaks[j].peak > x) x = m_rawPeaks[j].peak;
      }
    }

    if (x >= env) {
      env = attCoeff * env + (1.0 - attCoeff) * x;
    } else {
      env = relCoeff * env + (1.0 - relCoeff) * x;
    }

    double db = 20.0 * log10(std::max(env, EPSILON)) + itemVolDb;
    double norm = (db - params.minDb) / (params.maxDb - params.minDb);
    norm = std::max(0.0, std::min(1.0, norm));

    m_results[i] = {m_rawPeaks[i].time, m_rawPeaks[i].peak, db, norm, 0.0};
  }
}

double DynamicsEngine::GetThreshold() const
{
  return (m_params.threshold <= -99.0) ? m_avgPeakDb : m_params.threshold;
}

// Stage 3: Gain-smoothing compressor model (industry standard).
// Instant GR computed from raw peaks, then attack/release smooth the GR signal.
// This matches FabFilter Pro-C, Waves, UAD, ReaComp behavior.
std::vector<DynamicsEngine::CompressPoint> DynamicsEngine::ComputeCompression()
{
  std::vector<CompressPoint> out;
  if (m_results.empty()) return out;

  double thresh = GetThreshold();
  double ratio = std::max(1.0, m_params.ratio);
  double knee = std::max(0.0, m_params.kneeDb);
  double slope = 1.0 / ratio - 1.0; // negative for compression
  double halfKnee = knee / 2.0;

  // Attack/release coefficients for GR smoothing
  double dt = (m_results.size() > 1)
    ? (m_results[1].time - m_results[0].time) : STEP_SIZE;
  double attCoeff = (m_params.attackMs > 0.0)
    ? exp(-dt / (m_params.attackMs / 1000.0)) : 0.0;
  double relCoeff = (m_params.releaseMs > 0.0)
    ? exp(-dt / (m_params.releaseMs / 1000.0)) : 0.0;

  // Lookahead: scan N frames ahead for max peak before computing instant GR.
  // This lets the compressor "see" transients before they arrive.
  int lookaheadFrames = (m_params.lookaheadMs > 0.0 && dt > 0.0)
    ? (int)(m_params.lookaheadMs / 1000.0 / dt + 0.5) : 0;

  // Gate parameters (operates on post-compression+makeup level)
  bool gateEnabled = (m_params.gateThreshDb > -99.0);
  double gateRange = std::min(0.0, m_params.gateRangeDb); // always <= 0
  double gateThresh = m_params.gateThreshDb;
  // Gate smoothing: fast attack (2ms open), moderate release (100ms close)
  static constexpr double GATE_ATTACK_MS = 2.0;
  static constexpr double GATE_RELEASE_MS = 100.0;
  double gateAttCoeff = exp(-dt / (GATE_ATTACK_MS / 1000.0));
  double gateRelCoeff = exp(-dt / (GATE_RELEASE_MS / 1000.0));
  int gateHoldFrames = (m_params.gateHoldMs > 0.0 && dt > 0.0)
    ? (int)(m_params.gateHoldMs / 1000.0 / dt + 0.5) : 0;

  out.reserve(m_results.size());
  double grSum = 0.0;
  int grCount = 0;
  double smoothGR = 0.0; // smoothed compressor gain reduction (dB, <=0)
  double smoothGateGR = 0.0; // smoothed gate gain reduction (dB, <=0)
  int gateHoldCounter = 0;   // hold timer (frames remaining)
  int n = (int)m_results.size();

  // First pass: compute compressor GR + auto-makeup (needed before gate)
  std::vector<double> compGRs(n);
  for (int i = 0; i < n; i++) {
    auto& pt = m_results[i];

    // With lookahead: use max peak from current position to lookahead window
    double peakVal = pt.peakLinear;
    if (lookaheadFrames > 0) {
      int end = std::min(n, i + 1 + lookaheadFrames);
      for (int j = i + 1; j < end; j++) {
        if (m_results[j].peakLinear > peakVal)
          peakVal = m_results[j].peakLinear;
      }
    }

    // Instant gain computation from peak (with lookahead)
    double rawDb = 20.0 * log10(std::max(peakVal, EPSILON)) + m_itemVolDb;
    double overshoot = rawDb - thresh;
    double instantGR = 0.0;

    if (knee > 0.0 && overshoot > -halfKnee && overshoot < halfKnee) {
      double x = overshoot + halfKnee;
      instantGR = slope * x * x / (2.0 * knee);
    } else if (overshoot > 0.0) {
      instantGR = slope * overshoot;
    }

    // Smooth compressor GR
    if (instantGR < smoothGR) {
      smoothGR = attCoeff * smoothGR + (1.0 - attCoeff) * instantGR;
    } else {
      smoothGR = relCoeff * smoothGR + (1.0 - relCoeff) * instantGR;
    }

    compGRs[i] = smoothGR;
    if (smoothGR < -0.01) { grSum += smoothGR; grCount++; }
  }

  m_avgGR = (grCount > 0) ? grSum / (double)grCount : 0.0;
  double makeup = m_params.autoMakeup ? -m_avgGR : m_params.makeupDb;

  // Second pass: gate (on post-compression+makeup level) + output
  for (int i = 0; i < n; i++) {
    auto& pt = m_results[i];
    double compGR = compGRs[i];
    double totalGR = compGR;

    if (gateEnabled) {
      double rawDb = 20.0 * log10(std::max(pt.peakLinear, EPSILON)) + m_itemVolDb;
      double postLevel = rawDb + compGR + makeup;

      double instantGateGR = 0.0;
      if (postLevel < gateThresh) {
        // Signal below gate threshold — apply reduction
        if (gateHoldCounter > 0) {
          gateHoldCounter--; // hold: keep gate open
        } else {
          instantGateGR = -(gateThresh - postLevel);
          instantGateGR = std::max(instantGateGR, gateRange); // clamp to range
        }
      } else {
        // Signal above threshold — gate open, reset hold
        gateHoldCounter = gateHoldFrames;
      }

      // Smooth gate GR independently
      // "Attack" = gate opening (GR toward 0), "Release" = gate closing (GR more negative)
      if (instantGateGR < smoothGateGR) {
        smoothGateGR = gateRelCoeff * smoothGateGR + (1.0 - gateRelCoeff) * instantGateGR;
      } else {
        smoothGateGR = gateAttCoeff * smoothGateGR + (1.0 - gateAttCoeff) * instantGateGR;
      }

      totalGR = compGR + smoothGateGR;
    }

    pt.smoothedGR = totalGR;
    out.push_back({pt.time, totalGR + makeup});
  }

  return out;
}

// Iterative Ramer-Douglas-Peucker curve simplification
std::vector<DynamicsEngine::CompressPoint>
DynamicsEngine::SimplifyCurve(const std::vector<CompressPoint>& pts, double epsilonDb)
{
  int n = (int)pts.size();
  if (n <= 2) return pts;

  std::vector<bool> keep(n, false);
  keep[0] = true;
  keep[n - 1] = true;

  std::vector<std::pair<int, int>> stack;
  stack.reserve(32);
  stack.push_back({0, n - 1});

  while (!stack.empty()) {
    auto range = stack.back();
    stack.pop_back();
    int start = range.first;
    int end = range.second;

    if (end - start < 2) continue;

    double t0 = pts[start].time, db0 = pts[start].dbAdjust;
    double t1 = pts[end].time,   db1 = pts[end].dbAdjust;
    double dt = t1 - t0;

    double maxDist = 0.0;
    int maxIdx = start;

    for (int i = start + 1; i < end; i++) {
      double frac = (dt > 0.0) ? (pts[i].time - t0) / dt : 0.0;
      double interp = db0 + (db1 - db0) * frac;
      double dist = fabs(pts[i].dbAdjust - interp);
      if (dist > maxDist) {
        maxDist = dist;
        maxIdx = i;
      }
    }

    if (maxDist > epsilonDb) {
      keep[maxIdx] = true;
      stack.push_back({start, maxIdx});
      stack.push_back({maxIdx, end});
    }
  }

  std::vector<CompressPoint> result;
  result.reserve(n / 10);
  for (int i = 0; i < n; i++) {
    if (keep[i]) result.push_back(pts[i]);
  }
  return result;
}
