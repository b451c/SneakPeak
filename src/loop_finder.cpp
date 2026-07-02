// loop_finder.cpp — automatic loop-point finder (v2.4 INC-A2)
// See loop_finder.h. Runs on a worker thread in the host (a full search on a
// few minutes of audio is a few hundred ms), so everything here is
// straight-line single-threaded code on a caller-owned buffer.

#include "loop_finder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kFftN = 1024;   // spectral tie-break transform size

// In-place radix-2 complex FFT (double) — same textbook local-FFT pattern as
// spectral_repair.cpp (kickoff decision: no WDL_FFT_REALSIZE dependency).
void Fft(double* re, double* im, int n)
{
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
  }
  for (int len = 2; len <= n; len <<= 1) {
    double ang = -2.0 * kPi / (double)len;
    double wRe = std::cos(ang), wIm = std::sin(ang);
    for (int i = 0; i < n; i += len) {
      double cRe = 1.0, cIm = 0.0;
      int half = len / 2;
      for (int j = 0; j < half; j++) {
        int a = i + j, b = i + j + half;
        double tRe = re[b] * cRe - im[b] * cIm;
        double tIm = re[b] * cIm + im[b] * cRe;
        re[b] = re[a] - tRe; im[b] = im[a] - tIm;
        re[a] += tRe; im[a] += tIm;
        double nRe = cRe * wRe - cIm * wIm;
        cIm = cRe * wIm + cIm * wRe;
        cRe = nRe;
      }
    }
  }
}

// Nearest RISING zero crossing (x[i-1] < 0 <= x[i]) to `frame` within
// +-rangeFrames on the mono mix; -1 if none.
int NearestRisingZc(const std::vector<double>& mono, int frame, int rangeFrames)
{
  const int n = (int)mono.size();
  for (int d = 0; d <= rangeFrames; d++) {
    for (int sgn = 0; sgn < 2; sgn++) {
      const int i = sgn ? frame - d : frame + d;
      if (i < 1 || i >= n) continue;
      if (mono[(size_t)i - 1] < 0.0 && mono[(size_t)i] >= 0.0) return i;
    }
  }
  return -1;
}

// Normalized cross-correlation of the w frames BEFORE points a and b.
double NccBefore(const std::vector<double>& mono, int a, int b, int w)
{
  double dot = 0.0, ea = 0.0, eb = 0.0;
  const double* pa = mono.data() + (size_t)(a - w);
  const double* pb = mono.data() + (size_t)(b - w);
  for (int i = 0; i < w; i++) {
    dot += pa[i] * pb[i];
    ea += pa[i] * pa[i];
    eb += pb[i] * pb[i];
  }
  const double denom = std::sqrt(ea * eb);
  if (denom < 1e-12) return 0.0;   // silence on either side scores nothing
  return dot / denom;
}

// Normalized 1024-FFT magnitude spectrum of the frames BEFORE `at` (Hann).
std::vector<double> SpectrumBefore(const std::vector<double>& mono, int at)
{
  std::vector<double> re((size_t)kFftN, 0.0), im((size_t)kFftN, 0.0);
  const int from = at - kFftN;
  for (int i = 0; i < kFftN; i++) {
    const double hann = 0.5 * (1.0 - std::cos(2.0 * kPi * (double)i / (kFftN - 1)));
    re[(size_t)i] = (from + i >= 0) ? mono[(size_t)(from + i)] * hann : 0.0;
  }
  Fft(re.data(), im.data(), kFftN);
  std::vector<double> mag((size_t)(kFftN / 2), 0.0);
  double energy = 0.0;
  for (int i = 0; i < kFftN / 2; i++) {
    mag[(size_t)i] = std::hypot(re[(size_t)i], im[(size_t)i]);
    energy += mag[(size_t)i] * mag[(size_t)i];
  }
  const double inv = energy > 1e-24 ? 1.0 / std::sqrt(energy) : 0.0;
  for (double& m : mag) m *= inv;
  return mag;
}

// Averaged magnitude spectrum over the ~1 s BEFORE `at` (up to 8 Hann
// windows, unit-norm). A single 1024-window estimate is itself noise (each
// bin Rayleigh-distributed): two independent windows of the SAME stationary
// texture only reach ~0.6 similarity. Averaging drops the estimator variance
// so the texture score measures the timbre, not the snapshot.
std::vector<double> SpectrumAvgBefore(const std::vector<double>& mono, int at,
                                      int sampleRate)
{
  const int longW = std::min(at, sampleRate);
  if (longW <= kFftN) return SpectrumBefore(mono, at);
  const int K = 8;
  const int step = (longW - kFftN) / (K - 1);
  std::vector<double> acc((size_t)(kFftN / 2), 0.0);
  for (int k = 0; k < K; k++) {
    const std::vector<double> m = SpectrumBefore(mono, at - (K - 1 - k) * step);
    for (size_t i = 0; i < acc.size(); i++) acc[i] += m[i];
  }
  double energy = 0.0;
  for (double v : acc) energy += v * v;
  const double inv = energy > 1e-24 ? 1.0 / std::sqrt(energy) : 0.0;
  for (double& v : acc) v *= inv;
  return acc;
}

double SpectralDistance(const std::vector<double>& a, const std::vector<double>& b)
{
  double d = 0.0;
  for (size_t i = 0; i < a.size(); i++) {
    const double e = a[i] - b[i];
    d += e * e;
  }
  return std::sqrt(d);   // in [0, sqrt(2)] for unit-norm inputs
}

}  // namespace

std::vector<LoopCandidate> FindLoopCandidates(const double* audio,
                                              int numFrames, int numChannels,
                                              int sampleRate, int maxCandidates)
{
  std::vector<LoopCandidate> out;
  if (!audio || numFrames <= 0 || numChannels <= 0 || sampleRate <= 0 ||
      maxCandidates <= 0)
    return out;

  const int w = (int)(0.030 * sampleRate + 0.5);         // 30 ms NCC window
  const int lenMin = std::max(sampleRate, numFrames / 10); // max(1 s, 10%)
  const int lenMax = numFrames - sampleRate / 10;          // file - 100 ms
  if (lenMax <= lenMin || numFrames < lenMin + w) return out;

  // Mono mix: scoring wants ONE continuity judgement, not per-channel votes.
  std::vector<double> mono((size_t)numFrames);
  for (int i = 0; i < numFrames; i++) {
    double s = 0.0;
    for (int c = 0; c < numChannels; c++)
      s += audio[(size_t)i * (size_t)numChannels + (size_t)c];
    mono[(size_t)i] = s / (double)numChannels;
  }

  // Candidate points: rising zero crossings nearest to uniform grids (a full
  // all-pairs ZC search is O(Z^2) and pointless - neighbouring crossings
  // score nearly identically). The +-5 ms snap keeps every candidate a true
  // rising crossing, which also satisfies the slope-sign-match requirement
  // by construction.
  const int zcRange = sampleRate / 200;   // +-5 ms
  auto gridCandidates = [&](int from, int to, int count) {
    std::vector<int> pts;
    if (to <= from || count <= 0) return pts;
    pts.reserve((size_t)count);
    int last = -1;
    for (int k = 0; k < count; k++) {
      const int target = from + (int)((long long)(to - from) * k / count);
      const int zc = NearestRisingZc(mono, target, zcRange);
      if (zc >= w && zc != last && zc <= numFrames) {
        pts.push_back(zc);
        last = zc;
      }
    }
    return pts;
  };
  const std::vector<int> starts = gridCandidates(w, numFrames - lenMin, 160);
  const std::vector<int> ends = gridCandidates(lenMin + w, numFrames, 400);
  if (starts.empty() || ends.empty()) return out;

  // Pass 1: NCC over all length-valid pairs; keep everything above the floor.
  struct Scored { int s, e; double ncc; double score; bool tex; };
  std::vector<Scored> pool;
  for (int s : starts)
    for (int e : ends) {
      const int len = e - s;
      if (len < lenMin || len > lenMax) continue;
      const double ncc = NccBefore(mono, s, e, w);
      if (ncc < 0.5) continue;   // plan floor: reject weak continuations
      pool.push_back({ s, e, ncc, ncc, false });
    }

  if (!pool.empty()) {
    // Pass 2: spectral tie-break on the strongest survivors only (the FFT is
    // the expensive part; NCC ordering is already roughly right).
    std::sort(pool.begin(), pool.end(),
              [](const Scored& a, const Scored& b) { return a.ncc > b.ncc; });
    const size_t refine = std::min(pool.size(), (size_t)200);
    for (size_t i = 0; i < refine; i++) {
      const std::vector<double> sa = SpectrumBefore(mono, pool[i].s);
      const std::vector<double> se = SpectrumBefore(mono, pool[i].e);
      pool[i].score = pool[i].ncc - 0.25 * SpectralDistance(sa, se);
    }
    pool.resize(refine);
  } else {
    // TEXTURE fallback: stochastic ambiences (birds, rain, wind) never
    // correlate at the sample level - the honest waveform answer is "none",
    // but the game-audio practice is: cut where the texture is STATISTICALLY
    // the same and weld the seam. Score = spectral similarity, penalized for
    // a level mismatch and for cutting mid-transient (a chirp under the
    // knife); silence is excluded (looping silence is meaningless).
    struct PointFeat {
      std::vector<double> spec;
      double rmsLong = 0.0, rmsShort = 0.0;
    };
    auto features = [&](int at) {
      PointFeat f;
      f.spec = SpectrumAvgBefore(mono, at, sampleRate);
      const int longW = std::min(at, sampleRate);       // 1 s before the point
      const int shortW = std::min(at, w);               // the 30 ms window
      double accL = 0.0, accS = 0.0;
      for (int i = at - longW; i < at; i++) accL += mono[(size_t)i] * mono[(size_t)i];
      for (int i = at - shortW; i < at; i++) accS += mono[(size_t)i] * mono[(size_t)i];
      f.rmsLong = longW > 0 ? std::sqrt(accL / longW) : 0.0;
      f.rmsShort = shortW > 0 ? std::sqrt(accS / shortW) : 0.0;
      return f;
    };
    std::vector<PointFeat> fs, fe;
    fs.reserve(starts.size());
    fe.reserve(ends.size());
    for (int s : starts) fs.push_back(features(s));
    for (int e : ends) fe.push_back(features(e));

    for (size_t i = 0; i < starts.size(); i++)
      for (size_t j = 0; j < ends.size(); j++) {
        const int s = starts[i], e = ends[j];
        const int len = e - s;
        if (len < lenMin || len > lenMax) continue;
        const PointFeat& a = fs[i];
        const PointFeat& b = fe[j];
        if (a.rmsLong < 1e-6 || b.rmsLong < 1e-6) continue;   // silence
        const double sim = 1.0 - SpectralDistance(a.spec, b.spec) /
                                     std::sqrt(2.0);           // 0..1
        const double lvl = std::fabs(a.rmsLong - b.rmsLong) /
                           std::max(a.rmsLong, b.rmsLong);     // 0..1
        const double trans = std::max(
            0.0, std::max(a.rmsShort / a.rmsLong, b.rmsShort / b.rmsLong) - 1.3);
        const double score = sim * (1.0 - 0.5 * lvl) / (1.0 + trans);
        if (score < 0.5) continue;
        pool.push_back({ s, e, score, score, true });
      }
  }
  if (pool.empty()) return out;

  std::sort(pool.begin(), pool.end(),
            [](const Scored& a, const Scored& b) { return a.score > b.score; });

  // De-duplicate: a kept candidate owns its neighbourhood (a pair whose start
  // AND end both sit within 250 ms of a better one is the same loop).
  const int dedup = sampleRate / 4;
  for (const Scored& c : pool) {
    bool dup = false;
    for (const LoopCandidate& k : out)
      if (std::abs(c.s - k.startFrame) < dedup &&
          std::abs(c.e - k.endFrame) < dedup) {
        dup = true;
        break;
      }
    if (dup) continue;
    out.push_back({ c.s, c.e, c.score, c.tex });
    if ((int)out.size() >= maxCandidates) break;
  }
  return out;
}

bool WeldLoopSeam(double* audio, int numFrames, int numChannels,
                  int startFrame, int endFrame, int crossfadeFrames)
{
  if (!audio || numFrames <= 0 || numChannels <= 0) return false;
  const int L = crossfadeFrames;
  if (L <= 0) return false;
  if (startFrame < L) return false;              // needs L frames before start
  if (endFrame <= startFrame) return false;
  if (endFrame > numFrames) return false;
  if (L > endFrame - startFrame) return false;   // weld can't exceed the loop

  for (int i = 0; i < L; i++) {
    const double t = (double)(i + 1) / (double)(L + 1);
    const double gTail = std::cos(t * kPi * 0.5);   // original tail fades out
    const double gHead = std::sin(t * kPi * 0.5);   // pre-start fades in
    const size_t dst = (size_t)(endFrame - L + i) * (size_t)numChannels;
    const size_t src = (size_t)(startFrame - L + i) * (size_t)numChannels;
    for (int c = 0; c < numChannels; c++)
      audio[dst + (size_t)c] = audio[dst + (size_t)c] * gTail +
                               audio[src + (size_t)c] * gHead;
  }
  return true;
}
