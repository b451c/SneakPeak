// limiter_engine.cpp — True-Peak Hard Limiter DSP core (v2.4.0 INC-L0)
// See limiter_engine.h for the design and licensing notes.
//
// Zero-overshoot proof sketch (the harness asserts this end to end):
//   e1[i] = min(need[i-S+1 .. i])            sliding min, window S
//   e2[i] = sum_s w_s * e1[i-s], s in [0,S)  cascaded boxes, support S,
//                                            w_s >= 0, sum w_s = 1
//   For every s in the box support, the window of e1[i-s] contains index
//   i-S+1, so e1[i-s] <= need[i-S+1] and therefore e2[i] <= need[i-S+1].
//   Hold (a second sliding min) and the release cascade only ever go lower
//   pointwise, so with D = S-1 the aligned envelope env[j] = e3[j+D]
//   satisfies env[j] <= need[j] = min(1, ceiling/peak[j]). QED (sample
//   domain; the interpolated domain additionally needs the true-peak
//   refinement loop — see the comment at its site).

#include "limiter_engine.h"

#include <cmath>
#include <cstddef>
#include <deque>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// --- True-peak interpolator (BS.1770-family windowed sinc, libebur128) -----
// Same kernel family as the BS.1770-4 reference (Hann-windowed sinc polyphase,
// 12 input samples of support) but at 8x with 97 taps instead of the standard
// 4x/49: the 4x minimum under-reads inter-sample peaks by up to ~0.3 dB on
// broadband content (measured in the harness), which is fine for a METER but
// not for a HARD ceiling. Detecting at 8x strictly dominates any BS.1770-4
// 4x compliance meter (the 4x grid is a subset of the 8x grid), so platform
// QC reads at or below our ceiling. Offline, the extra taps are free.
// At/above 192 kHz detection falls back to sample peaks (libebur128 rule).

constexpr int kTpTaps = 97;                       // 12 * factor + 1
constexpr int kTpCenter = (kTpTaps - 1) / 2;      // 48

int TpFactorForRate(int sampleRate)
{
  return sampleRate < 192000 ? 8 : 1;
}

void BuildTpCoeffs(int factor, double* h /* kTpTaps */)
{
  for (int j = 0; j < kTpTaps; j++) {
    double m = (double)j - (double)kTpCenter;
    double c = 1.0;
    if (std::fabs(m) > 1e-9) {
      double a = m * kPi / (double)factor;
      c = std::sin(a) / a;
    }
    c *= 0.5 * (1.0 - std::cos(2.0 * kPi * (double)j / (double)(kTpTaps - 1)));
    h[j] = c;
  }
}

// Max-accumulate the detector peak per frame for one channel: |x[i]| plus the
// interpolated sub-samples between x[i] and x[i+1]. Offline two-pass lets us
// index the FIR symmetrically (x[i - 24/F .. i + 24/F], zero-padded edges),
// so detection adds no latency that would need folding into the dry delay.
void TpPeakPerFrame(const double* audio, int numFrames, int numChannels,
                    int channel, int factor, const double* h,
                    std::vector<double>& tpInOut)
{
  const int center = kTpCenter / factor; // 6 input samples at 8x
  for (int i = 0; i < numFrames; i++) {
    double pk = std::fabs(audio[(size_t)i * (size_t)numChannels + (size_t)channel]);
    for (int p = 1; p < factor; p++) {
      double acc = 0.0;
      for (int j = p; j < kTpTaps; j += factor) {
        int src = i + center - (j - p) / factor;
        if (src < 0 || src >= numFrames) continue;
        acc += h[j] * audio[(size_t)src * (size_t)numChannels + (size_t)channel];
      }
      double a = std::fabs(acc);
      if (a > pk) pk = a;
    }
    if (pk > tpInOut[(size_t)i]) tpInOut[(size_t)i] = pk;
  }
}

// --- Sliding minimum over a trailing window (Lemire monotonic wedge) -------
// Same result as Signalsmith's PeakHold with min ordering; O(1) amortized.
// Pre-history counts as 1.0 (no reduction), which falls out for free because
// every pushed value is <= 1.
class SlidingMin {
public:
  void Init(int window)
  {
    m_window = window > 1 ? window : 1;
    m_idx = 0;
    m_q.clear();
  }
  double Push(double v)
  {
    while (!m_q.empty() && m_q.back().value >= v) m_q.pop_back();
    m_q.push_back({ m_idx, v });
    if (m_q.front().index <= m_idx - (long long)m_window) m_q.pop_front();
    m_idx++;
    return m_q.front().value;
  }

private:
  struct Entry {
    long long index;
    double value;
  };
  int m_window = 1;
  long long m_idx = 0;
  std::deque<Entry> m_q;
};

// --- Running-sum box filter, exact re-sum on every wrap (no FP drift) ------
class BoxFilter {
public:
  void Init(int len)
  {
    m_len = len > 1 ? len : 1;
    m_buf.assign((size_t)m_len, 1.0);
    m_sum = (double)m_len;
    m_pos = 0;
  }
  double Push(double v)
  {
    m_sum += v - m_buf[(size_t)m_pos];
    m_buf[(size_t)m_pos] = v;
    if (++m_pos == m_len) {
      m_pos = 0;
      double s = 0.0; // exact re-sum once per wrap: O(1) amortized
      for (double x : m_buf) s += x;
      m_sum = s;
    }
    return m_sum / (double)m_len;
  }
  int Length() const { return m_len; }

private:
  std::vector<double> m_buf;
  double m_sum = 1.0;
  int m_len = 1;
  int m_pos = 0;
};

// --- Cascaded box smoothing (BoxStackFilter layer ratios, Signalsmith MIT) --
// K=4 fixed in v1; stop-band peak ~= (5 - 18*K) dB -> ~ -67 dB gain-ripple
// harmonics. Ratios normalized so the lengths sum to ~window (each >= 1).
constexpr int kBoxLayers = 4;
constexpr double kBoxRatios[kBoxLayers] = { 1.0, 0.58224186169, 0.41775813831,
                                            0.404078562416 };

class BoxStack {
public:
  void Init(int window)
  {
    double ratioSum = 0.0;
    for (double r : kBoxRatios) ratioSum += r;
    int total = 0;
    for (int k = 0; k < kBoxLayers; k++) {
      int len = (int)std::floor(kBoxRatios[k] * (double)window / ratioSum + 0.5);
      if (len < 1) len = 1;
      m_box[k].Init(len);
      total += len;
    }
    m_support = total - kBoxLayers + 1; // FIR support of the cascade
  }
  double Push(double v)
  {
    for (int k = 0; k < kBoxLayers; k++) v = m_box[k].Push(v);
    return v;
  }
  int Support() const { return m_support; }

private:
  BoxFilter m_box[kBoxLayers];
  int m_support = 1;
};

// --- Hold + cascaded exponential release ------------------------------------
// 3 one-poles, rising direction only (falling follows instantly), slew =
// 1 / (releaseSamples/3 + 1) per stage: the cascaded step response is
// C1-smooth (no release-corner thump). Every stage's output is <= its input,
// so the final envelope never exceeds the smoothed envelope.
class ReleaseCascade {
public:
  void Init(double releaseSamples)
  {
    double rs = releaseSamples > 0.0 ? releaseSamples : 0.0;
    m_slew = 1.0 / (rs / 3.0 + 1.0);
    m_s[0] = m_s[1] = m_s[2] = 1.0;
  }
  double Push(double v)
  {
    double in = v;
    for (int k = 0; k < 3; k++) {
      if (in < m_s[k]) m_s[k] = in;
      else m_s[k] += (in - m_s[k]) * m_slew;
      in = m_s[k];
    }
    return in;
  }

private:
  double m_s[3] = { 1.0, 1.0, 1.0 };
  double m_slew = 1.0;
};

// Full chain over need[] for one gain chain (linked mono chain or a single
// unlinked channel). Writes the delay-aligned envelope; the chain is fed
// need=1.0 for D extra samples past the end to flush the alignment. Hold is
// realized as a second sliding min (window holdW+1): identical behavior to a
// "hold counter reset while falling" and trivially <= its input.
int RunChain(const std::vector<double>& need, int attackW, int holdW,
             double releaseSamples, std::vector<double>& env, int stride,
             int offset, LimiterDebugTaps* taps)
{
  const int n = (int)need.size();
  BoxStack box;
  box.Init(attackW);
  const int support = box.Support();
  const int delay = support - 1;
  SlidingMin slideMin;
  slideMin.Init(support);
  SlidingMin holdMin;
  holdMin.Init(holdW + 1);
  ReleaseCascade release;
  release.Init(releaseSamples);

  for (int i = 0; i < n + delay; i++) {
    const double nd = i < n ? need[(size_t)i] : 1.0;
    const double e1 = slideMin.Push(nd);
    const double e2 = box.Push(e1);
    const double e3 = release.Push(holdMin.Push(e2));
    if (taps && i < n) {
      const size_t at = (size_t)i * (size_t)stride + (size_t)offset;
      taps->need[at] = nd;
      taps->e1[at] = e1;
      taps->e2[at] = e2;
      taps->e3[at] = e3;
    }
    if (i >= delay)
      env[(size_t)(i - delay) * (size_t)stride + (size_t)offset] = e3;
  }
  return delay;
}

} // namespace

LimiterResult LimiterComputeEnvelope(const double* audio, int numFrames,
                                     int numChannels, int sampleRate,
                                     const LimiterParams& params,
                                     std::vector<double>& envOut,
                                     int edgeRampFrames,
                                     LimiterDebugTaps* debugTaps)
{
  LimiterResult res;
  if (!audio || numFrames <= 0 || numChannels <= 0 || sampleRate <= 0)
    return res;

  const double gainLin = std::pow(10.0, params.gainDb / 20.0);
  const double ceilingLin = std::pow(10.0, params.ceilingDb / 20.0);
  const int factor = params.truePeak ? TpFactorForRate(sampleRate) : 1;
  const int numChains = (params.link || numChannels == 1) ? 1 : numChannels;
  const bool linked = (numChains == 1);

  int attackW = (int)std::floor(params.attackMs * 0.001 * (double)sampleRate + 0.5);
  attackW = ClampInt(attackW, 1, numFrames); // buffer shorter than W: clamp
  int holdW = (int)std::floor(params.holdMs * 0.001 * (double)sampleRate + 0.5);
  holdW = ClampInt(holdW, 0, numFrames);
  const double releaseSamples = params.releaseMs * 0.001 * (double)sampleRate;

  double h[kTpTaps] = { 0.0 };
  if (factor > 1) BuildTpCoeffs(factor, h);

  const size_t envSize = (size_t)numFrames * (size_t)numChains;
  envOut.assign(envSize, 1.0);
  if (debugTaps) {
    debugTaps->need.assign(envSize, 1.0);
    debugTaps->e1.assign(envSize, 1.0);
    debugTaps->e2.assign(envSize, 1.0);
    debugTaps->e3.assign(envSize, 1.0);
  }

  std::vector<double> peak((size_t)numFrames, 0.0);
  std::vector<double> need((size_t)numFrames, 1.0);
  std::vector<double> tmp, otp;
  if (factor > 1) {
    tmp.resize((size_t)numFrames);
    otp.resize((size_t)numFrames);
  }
  double maxPeakAll = 0.0;
  int latency = 0;

  for (int chain = 0; chain < numChains; chain++) {
    std::fill(peak.begin(), peak.end(), 0.0);
    const int chFirst = linked ? 0 : chain;
    const int chLast = linked ? numChannels - 1 : chain;
    for (int ch = chFirst; ch <= chLast; ch++) {
      if (factor > 1) {
        TpPeakPerFrame(audio, numFrames, numChannels, ch, factor, h, peak);
      } else {
        for (int i = 0; i < numFrames; i++) {
          double a = std::fabs(audio[(size_t)i * (size_t)numChannels + (size_t)ch]);
          if (a > peak[(size_t)i]) peak[(size_t)i] = a;
        }
      }
    }
    for (int i = 0; i < numFrames; i++) {
      const double p = peak[(size_t)i] * gainLin; // input gain pre-detection
      if (p > maxPeakAll) maxPeakAll = p;
      const double nd = ceilingLin / (p > 1e-12 ? p : 1e-12);
      need[(size_t)i] = nd < 1.0 ? nd : 1.0;
    }
    latency = RunChain(need, attackW, holdW, releaseSamples, envOut, numChains,
                       chain, debugTaps);

    // True-peak refinement. Gain modulation is NOT multiplicative in the
    // interpolated domain: an envelope step de-weights cancelling terms in
    // the interpolation kernel, so the first-pass output can grow NEW
    // inter-sample peaks (measured up to ~0.2 dB on noise). Construction
    // alone bounds the sample domain only; offline we can afford to close
    // the loop — re-measure the output with the detector, tighten need where
    // it still exceeds the ceiling, re-run the chain, repeat until the
    // detector reads clean. Bit-identical passthrough is preserved: with no
    // violation the loop breaks before touching anything.
    if (factor > 1) {
      const double breakLin = ceilingLin * 1.000115; // ~ +0.001 dB slack
      for (int iter = 0; iter < 8; iter++) {
        std::fill(otp.begin(), otp.end(), 0.0);
        for (int ch = chFirst; ch <= chLast; ch++) {
          for (int k = 0; k < numFrames; k++)
            tmp[(size_t)k] =
                audio[(size_t)k * (size_t)numChannels + (size_t)ch] * gainLin *
                envOut[(size_t)k * (size_t)numChains + (size_t)chain];
          TpPeakPerFrame(tmp.data(), numFrames, 1, 0, factor, h, otp);
        }
        bool violated = false;
        for (int k = 0; k < numFrames; k++)
          if (otp[(size_t)k] > breakLin) {
            violated = true;
            need[(size_t)k] *= ceilingLin / otp[(size_t)k];
          }
        if (!violated) break;
        latency = RunChain(need, attackW, holdW, releaseSamples, envOut,
                           numChains, chain, debugTaps);
      }
    }
  }

  // Selection-apply handoff: blend the envelope to exactly 1.0 at both buffer
  // edges over edgeRampFrames (the ramp region may exceed the ceiling by
  // design — it hands off to untouched audio outside the selection).
  if (edgeRampFrames > 0) {
    const int ramp = edgeRampFrames < numFrames ? edgeRampFrames : numFrames;
    for (int i = 0; i < numFrames; i++) {
      int edge = i < numFrames - 1 - i ? i : numFrames - 1 - i;
      if (edge >= ramp) continue;
      const double w = (double)edge / (double)ramp;
      for (int c = 0; c < numChains; c++) {
        const size_t at = (size_t)i * (size_t)numChains + (size_t)c;
        envOut[at] = 1.0 + (envOut[at] - 1.0) * w;
      }
    }
  }

  double minEnv = 1.0;
  for (double e : envOut)
    if (e < minEnv) minEnv = e;

  res.ok = true;
  res.latencySamples = latency;
  if (debugTaps) debugTaps->latencySamples = latency;
  if (maxPeakAll > 0.0) res.inputPeakDb = 20.0 * std::log10(maxPeakAll);
  if (minEnv < 1.0 && minEnv > 0.0)
    res.maxGainReductionDb = -20.0 * std::log10(minEnv);
  return res;
}

LimiterResult LimiterProcess(double* audio, int numFrames, int numChannels,
                             int sampleRate, const LimiterParams& params,
                             int edgeRampFrames)
{
  std::vector<double> env;
  LimiterResult res = LimiterComputeEnvelope(audio, numFrames, numChannels,
                                             sampleRate, params, env,
                                             edgeRampFrames, nullptr);
  if (!res.ok) return res;

  const double gainLin = std::pow(10.0, params.gainDb / 20.0);
  const int numChains = (params.link || numChannels == 1) ? 1 : numChannels;
  for (int i = 0; i < numFrames; i++)
    for (int ch = 0; ch < numChannels; ch++) {
      const size_t envAt =
          (size_t)i * (size_t)numChains + (size_t)(numChains == 1 ? 0 : ch);
      audio[(size_t)i * (size_t)numChannels + (size_t)ch] *= gainLin * env[envAt];
    }

  const double outPk = LimiterMeasurePeak(audio, numFrames, numChannels,
                                          sampleRate, params.truePeak);
  if (outPk > 0.0) res.outputPeakDb = 20.0 * std::log10(outPk);
  return res;
}

double LimiterMeasurePeak(const double* audio, int numFrames, int numChannels,
                          int sampleRate, bool truePeak)
{
  if (!audio || numFrames <= 0 || numChannels <= 0 || sampleRate <= 0)
    return 0.0;
  const int factor = truePeak ? TpFactorForRate(sampleRate) : 1;
  double pk = 0.0;
  if (factor == 1) {
    const size_t total = (size_t)numFrames * (size_t)numChannels;
    for (size_t i = 0; i < total; i++) {
      const double a = std::fabs(audio[i]);
      if (a > pk) pk = a;
    }
  } else {
    double h[kTpTaps];
    BuildTpCoeffs(factor, h);
    std::vector<double> tp((size_t)numFrames, 0.0);
    for (int ch = 0; ch < numChannels; ch++)
      TpPeakPerFrame(audio, numFrames, numChannels, ch, factor, h, tp);
    for (double v : tp)
      if (v > pk) pk = v;
  }
  return pk;
}
