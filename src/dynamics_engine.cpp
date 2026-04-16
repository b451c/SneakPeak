// dynamics_engine.cpp — Professional dynamics processor
// Standard compressor: threshold + ratio + soft knee + attack/release + makeup
#include "dynamics_engine.h"
#include <algorithm>

static constexpr double STEP_SIZE = 0.001; // 1ms analysis windows
static constexpr double EPSILON = 1e-12;

void DynamicsEngine::Analyze(const double* audioData, int numFrames, int numChannels,
                             int sampleRate, double itemVolDb, const DynamicsParams& params)
{
  m_params = params;
  m_results.clear();
  m_rawPeaks.clear();
  m_avgGR = 0.0;

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

    m_results[i] = {m_rawPeaks[i].time, m_rawPeaks[i].peak, db, norm};
  }
}

double DynamicsEngine::GetThreshold() const
{
  return (m_params.threshold <= -99.0) ? m_avgPeakDb : m_params.threshold;
}

// Stage 3: Standard compressor gain computation with soft knee
std::vector<DynamicsEngine::CompressPoint> DynamicsEngine::ComputeCompression() const
{
  std::vector<CompressPoint> out;
  if (m_results.empty()) return out;

  double thresh = GetThreshold();
  double ratio = std::max(1.0, m_params.ratio);
  double knee = std::max(0.0, m_params.kneeDb);
  double slope = 1.0 / ratio - 1.0; // negative for compression
  double halfKnee = knee / 2.0;

  out.reserve(m_results.size());
  double grSum = 0.0;

  for (const auto& pt : m_results) {
    double overshoot = pt.db - thresh;
    double gr = 0.0; // gain reduction in dB (negative = quieter)

    if (knee > 0.0 && overshoot > -halfKnee && overshoot < halfKnee) {
      // Soft knee: quadratic interpolation around threshold
      double x = overshoot + halfKnee;
      gr = slope * x * x / (2.0 * knee);
    } else if (overshoot > 0.0) {
      // Above threshold: standard compression
      gr = slope * overshoot;
    }
    // Below threshold (and outside knee): no compression, gr = 0

    grSum += gr;
    out.push_back({pt.time, gr}); // makeup added below
  }

  // Average gain reduction (for auto-makeup and GR meter)
  m_avgGR = (out.empty()) ? 0.0 : grSum / (double)out.size();

  // Apply makeup gain
  double makeup = m_params.autoMakeup ? -m_avgGR : m_params.makeupDb;
  if (makeup != 0.0) {
    for (auto& cp : out)
      cp.dbAdjust += makeup;
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
