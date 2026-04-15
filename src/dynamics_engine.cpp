// dynamics_engine.cpp — Peak analysis and envelope smoothing
// Translated from saxmand's Lua: CollectTakePeaks + BuildEnvelopeValues
#include "dynamics_engine.h"
#include <algorithm>

static constexpr double STEP_SIZE = 0.001; // 1ms windows (matches saxmand's code)
static constexpr double EPSILON = 1e-12;   // floor for log10

void DynamicsEngine::Analyze(const double* audioData, int numFrames, int numChannels,
                             int sampleRate, double itemVolDb, const DynamicsParams& params)
{
  m_params = params;
  m_results.clear();
  m_rawPeaks.clear();

  if (!audioData || numFrames <= 0 || sampleRate <= 0) return;

  CollectPeaks(audioData, numFrames, numChannels, sampleRate);
  if (m_rawPeaks.empty()) return;

  // Compute average peak dB (for initial target in C4)
  double sumPeak = 0.0;
  for (const auto& p : m_rawPeaks) sumPeak += p.peak;
  double meanLinear = sumPeak / (double)m_rawPeaks.size();
  m_avgPeakDb = 20.0 * log10(std::max(meanLinear, EPSILON)) + itemVolDb;

  BuildEnvelope(itemVolDb, params);
}

// Stage 1: Collect peaks from audio buffer in 1ms windows
// Stereo: max(abs(L), abs(R)) per window. Much faster than saxmand's API approach.
void DynamicsEngine::CollectPeaks(const double* audioData, int numFrames, int numChannels,
                                  int sampleRate)
{
  int samplesPerStep = std::max(1, (int)(STEP_SIZE * sampleRate));
  int nch = std::max(1, numChannels);

  m_rawPeaks.reserve((size_t)(numFrames / samplesPerStep + 1));

  for (int frame = 0; frame < numFrames; frame += samplesPerStep) {
    int windowEnd = std::min(numFrames, frame + samplesPerStep);
    double peak = 0.0;

    for (int s = frame; s < windowEnd; s++) {
      for (int ch = 0; ch < nch; ch++) {
        double v = fabs(audioData[(size_t)s * nch + ch]);
        if (v > peak) peak = v;
      }
    }

    double time = (double)frame / (double)sampleRate;
    m_rawPeaks.push_back({time, peak});
  }
}

// Stage 2: Asymmetric attack/release follower with optional lookahead
// Direct translation of saxmand's BuildEnvelopeValues
void DynamicsEngine::BuildEnvelope(double itemVolDb, const DynamicsParams& params)
{
  if (m_rawPeaks.empty()) return;

  double dt = (m_rawPeaks.size() > 1)
    ? (m_rawPeaks[1].time - m_rawPeaks[0].time)
    : STEP_SIZE;

  // Exponential smoothing coefficients
  double attCoeff = (params.attackMs > 0.0)
    ? exp(-dt / (params.attackMs / 1000.0))
    : 0.0; // instant attack
  double relCoeff = (params.releaseMs > 0.0)
    ? exp(-dt / (params.releaseMs / 1000.0))
    : 0.0; // instant release

  int lookaheadFrames = (params.lookaheadMs > 0.0)
    ? (int)(params.lookaheadMs / 1000.0 / dt + 0.5)
    : 0;

  int count = (int)m_rawPeaks.size();
  m_results.resize((size_t)count);

  double env = m_rawPeaks[0].peak;

  for (int i = 0; i < count; i++) {
    double x = m_rawPeaks[i].peak;

    // Lookahead: take max peak within lookahead window
    if (lookaheadFrames > 0) {
      int ahead_end = std::min(count, i + 1 + lookaheadFrames);
      for (int j = i + 1; j < ahead_end; j++) {
        if (m_rawPeaks[j].peak > x) x = m_rawPeaks[j].peak;
      }
    }

    // Asymmetric attack/release
    if (x >= env) {
      env = attCoeff * env + (1.0 - attCoeff) * x;
    } else {
      env = relCoeff * env + (1.0 - relCoeff) * x;
    }

    // Convert to dB, add item volume
    double db = 20.0 * log10(std::max(env, EPSILON)) + itemVolDb;

    // Normalize to 0..1 within minDb..maxDb range
    double norm = (db - params.minDb) / (params.maxDb - params.minDb);
    norm = std::max(0.0, std::min(1.0, norm));

    m_results[i] = {m_rawPeaks[i].time, m_rawPeaks[i].peak, db, norm};
  }
}
