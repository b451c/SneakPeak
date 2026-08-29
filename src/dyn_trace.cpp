// dyn_trace.cpp — see dyn_trace.h. Every window/sum below mirrors the legacy
// DynamicsEngine::CollectPeaks / DeEssBandTrace loops operation for operation
// (same expressions, same summation order) so a trace built from N chunks is
// bit-identical to one built from the whole buffer - tests/dyn_chunk_test.cpp.
#include "dyn_trace.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr double STEP_SIZE = 0.001;   // 1 ms analysis windows (dynamics_engine.cpp)
}

void DynTraceBuilder::Begin(const DynTraceKey& key)
{
  m_trace = std::make_shared<DynTrace>();
  DynTrace& T = *m_trace;
  T.key = key;
  m_nch = std::max(1, key.numChannels);
  m_step = std::max(1, (int)(STEP_SIZE * key.sampleRate));
  T.samplesPerStep = m_step;
  m_fed = 0;
  m_winFill = 0;
  m_winMax = m_bandMax = 0.0;
  m_hist.clear();
  m_histStart = 0;
  m_nextRmsStep = 0;
  // RMS: wider window for averaging, never smaller than the step (legacy expression).
  m_rmsWindow = key.rmsMode
    ? std::max(m_step, (int)(key.rmsWindowMs / 1000.0 * key.sampleRate)) : 0;
  const size_t steps = (size_t)(key.numFrames / m_step + 1);
  T.peak.reserve(steps);
  m_nStages = 0;
  m_filt.clear();
  if (key.dsEnable) {
    m_nStages = DeEssSetupChain(m_filt, m_nch, (double)key.sampleRate, key.dsMode,
                                key.dsFreqHz, key.dsQ);
    T.band.reserve(steps);
  }
}

void DynTraceBuilder::Feed(const double* frames, int n)
{
  if (!m_trace || !frames) return;
  DynTrace& T = *m_trace;
  n = (int)std::min<int64_t>(n, T.key.numFrames - m_fed);   // never past the declared end
  if (n <= 0) return;
  const bool rms = T.key.rmsMode;
  const bool ds = m_nStages > 0;
  if (rms) m_hist.insert(m_hist.end(), frames, frames + (size_t)n * (size_t)m_nch);
  if (!rms || ds) {
    for (int f = 0; f < n; f++) {
      const double* x = frames + (size_t)f * (size_t)m_nch;
      for (int ch = 0; ch < m_nch; ch++) {
        if (!rms) {
          const double v = fabs(x[ch]);
          if (v > m_winMax) m_winMax = v;
        }
        if (ds) {
          double y = x[ch];
          DeEssBiquad* s = &m_filt[(size_t)ch * (size_t)m_nStages];
          for (int st = 0; st < m_nStages; st++) y = s[st].Process(y);
          const double a = fabs(y);
          if (a > m_bandMax) m_bandMax = a;
        }
      }
      if (++m_winFill == m_step) {
        if (!rms) T.peak.push_back(m_winMax);
        if (ds) T.band.push_back(m_bandMax);
        m_winFill = 0;
        m_winMax = m_bandMax = 0.0;
      }
    }
  }
  m_fed += n;
  if (rms) EmitRmsSteps(T.key.numFrames);
}

// Legacy per step: rmsStart = max(0, frame - W/2), rmsEnd = min(N, rmsStart + W),
// sum of squares s ascending / channel inner, sqrt(sum / (count * nch)).
void DynTraceBuilder::EmitRmsSteps(int64_t numFrames)
{
  DynTrace& T = *m_trace;
  const int64_t W = m_rmsWindow;
  for (;;) {
    const int64_t frame = m_nextRmsStep * m_step;
    if (frame >= numFrames) break;
    const int64_t rmsStart = std::max<int64_t>(0, frame - W / 2);
    const int64_t rmsEnd = std::min(numFrames, rmsStart + W);
    if (rmsEnd > m_fed) break;   // the window's tail has not arrived yet
    double sumSq = 0.0;
    int64_t count = 0;
    for (int64_t s = rmsStart; s < rmsEnd; s++) {
      const double* x = m_hist.data() + (size_t)(s - m_histStart) * (size_t)m_nch;
      for (int ch = 0; ch < m_nch; ch++) {
        const double v = x[ch];
        sumSq += v * v;
      }
      count++;
    }
    T.peak.push_back((count > 0) ? sqrt(sumSq / (double)(count * m_nch)) : 0.0);
    m_nextRmsStep++;
  }
  // History only has to reach back to the next step's window start; trim in
  // batches so a 1-frame feed does not pay an erase per call.
  const int64_t keepFrom = std::max<int64_t>(0, m_nextRmsStep * m_step - W / 2);
  if (keepFrom - m_histStart > 2 * (W + m_step)) {
    m_hist.erase(m_hist.begin(),
                 m_hist.begin() + (ptrdiff_t)((keepFrom - m_histStart) * m_nch));
    m_histStart = keepFrom;
  }
}

std::shared_ptr<const DynTrace> DynTraceBuilder::Finish()
{
  if (!m_trace) return nullptr;
  DynTrace& T = *m_trace;
  if (T.key.rmsMode) EmitRmsSteps(m_fed);   // == numFrames when fed in full
  if (m_winFill > 0) {   // partial last window (legacy: windowEnd = min(N, frame + step))
    if (!T.key.rmsMode) T.peak.push_back(m_winMax);
    if (m_nStages > 0) T.band.push_back(m_bandMax);
    m_winFill = 0;
  }
  m_hist.clear();
  m_hist.shrink_to_fit();
  m_filt.clear();
  std::shared_ptr<const DynTrace> out = std::move(m_trace);
  m_trace.reset();
  return out;
}
