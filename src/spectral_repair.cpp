// spectral_repair.cpp — Spectral Repair DSP (v2.3.0 INC-5)
// Engine B: STFT heal (Magron 2015 log-mag interpolation + phase propagation).
// Engine A: AR click repair (Oudre IPOL 2015/64 detection + Janssen IPOL
// 2018/23 interpolation). Paper math only — see the license note in the header.
#include "spectral_repair.h"
#include <algorithm>
#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

constexpr double EPS_MAG = 1e-12;

// ---------------------------------------------------------------------------
// Shared: in-place radix-2 complex FFT, double precision.
// Same textbook Cooley-Tukey as the display path (spectral_view.cpp DoFFT) but
// in double: the repair path resynthesizes audio, so it gets a full-precision
// local FFT instead of touching WDL_FFT_REALSIZE build-wide (kickoff decision).
void Fft(double* re, double* im, int n, bool inverse)
{
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
  }
  const double sign = inverse ? 2.0 : -2.0;
  for (int len = 2; len <= n; len <<= 1) {
    double ang = sign * M_PI / (double)len;
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
  if (inverse) {
    double inv = 1.0 / (double)n;
    for (int i = 0; i < n; i++) { re[i] *= inv; im[i] *= inv; }
  }
}

// --- Engine B constants (match the display spectrogram; research sec 4) ---
constexpr int HEAL_FFT = 2048;
constexpr int HEAL_HOP = HEAL_FFT / 4;   // 75% overlap
constexpr double PHASE_KEEP_RATIO = 0.5; // mild attenuation keeps original phase

// --- Engine A constants (Oudre IPOL 2015/64 defaults at 44.1 kHz, scaled) ---
constexpr double AR_NMAX_44K = 100.0;   // worst-case click length, samples @44.1k
constexpr double AR_FUSE_44K = 20.0;    // burst fusion distance, samples @44.1k
constexpr int AR_ITER = 2;              // detect+repair passes per frame
constexpr double AR_MAX_BURST_SEC = 0.023; // repair cap per missing-sample cluster
constexpr int AR_P_CAP = 1024;          // model-order ceiling at high sample rates

// Yule-Walker via Levinson-Durbin on biased autocorrelation R[0..p].
// Fills a[0..p] with the prediction-error filter (a[0] = 1, may stop early when
// the signal is fully modeled), returns the effective order, *sigma2 = final
// prediction-error variance.
int LevinsonDurbin(const double* R, int p, double* a, double* sigma2)
{
  double E = R[0] * (1.0 + 1e-9); // diagonal loading for near-singular signals
  for (int i = 0; i <= p; i++) a[i] = 0.0;
  a[0] = 1.0;
  *sigma2 = E;
  if (E <= 0.0) return 0;
  std::vector<double> tmp((size_t)p + 1);
  int order = 0;
  for (int i = 1; i <= p; i++) {
    double acc = R[i];
    for (int j = 1; j < i; j++) acc += a[j] * R[i - j];
    double k = -acc / E;
    if (std::isnan(k) || std::fabs(k) >= 1.0) break; // numerically unstable
    for (int j = 1; j < i; j++) tmp[(size_t)j] = a[j] + k * a[i - j];
    for (int j = 1; j < i; j++) a[j] = tmp[(size_t)j];
    a[i] = k;
    E *= (1.0 - k * k);
    order = i;
    *sigma2 = E;
    if (E < 1e-14 * R[0]) break; // signal fully modeled at this order
  }
  return order;
}

// b_m = sum_{l=0}^{order-m} a_l a_{l+m} (autocorrelation of the prediction-error
// filter; the entries of the Janssen quadratic form B, IPOL 2018/23).
void ArCoeffAutocorr(const double* a, int order, double* b)
{
  for (int m = 0; m <= order; m++) {
    double acc = 0.0;
    for (int l = 0; l + m <= order; l++) acc += a[l] * a[l + m];
    b[m] = acc;
  }
}

// In-place LL^T Cholesky + solve for the Janssen system B s = rhs.
// B is symmetric positive definite with a band envelope: entries outside it are
// exactly zero, and Cholesky preserves the envelope, so the factorization inner
// loop skips it via start[i] = first nonzero column of row i.
bool CholeskySolve(std::vector<double>& B, const std::vector<int>& start, int n,
                   std::vector<double>& x)
{
  for (int i = 0; i < n; i++) {
    for (int j = start[(size_t)i]; j <= i; j++) {
      double sum = B[(size_t)i * (size_t)n + (size_t)j];
      int k0 = std::max(start[(size_t)i], start[(size_t)j]);
      for (int k = k0; k < j; k++)
        sum -= B[(size_t)i * (size_t)n + (size_t)k] * B[(size_t)j * (size_t)n + (size_t)k];
      if (i == j) {
        if (sum <= 0.0) return false;
        B[(size_t)i * (size_t)n + (size_t)j] = std::sqrt(sum);
      } else {
        B[(size_t)i * (size_t)n + (size_t)j] = sum / B[(size_t)j * (size_t)n + (size_t)j];
      }
    }
  }
  for (int i = 0; i < n; i++) { // forward: L y = rhs
    double sum = x[(size_t)i];
    for (int k = start[(size_t)i]; k < i; k++)
      sum -= B[(size_t)i * (size_t)n + (size_t)k] * x[(size_t)k];
    x[(size_t)i] = sum / B[(size_t)i * (size_t)n + (size_t)i];
  }
  for (int i = n - 1; i >= 0; i--) { // back: L^T s = y (envelope zeros are benign)
    double sum = x[(size_t)i];
    for (int k = i + 1; k < n; k++)
      sum -= B[(size_t)k * (size_t)n + (size_t)i] * x[(size_t)k];
    x[(size_t)i] = sum / B[(size_t)i * (size_t)n + (size_t)i];
  }
  return true;
}

// Contiguous runs of set bytes (burst count for the result stats).
int CountRuns(const std::vector<unsigned char>& mask)
{
  int runs = 0;
  bool in = false;
  for (unsigned char v : mask) {
    if (v && !in) { runs++; in = true; }
    else if (!v) in = false;
  }
  return runs;
}

} // namespace

// ---------------------------------------------------------------------------
// Engine B — STFT heal
SpectralHealResult StftRepairRect(double* audio, int numFrames, int numChannels,
                                  int sampleRate, double t0, double t1,
                                  double freqLo, double freqHi, double strength)
{
  SpectralHealResult res;
  if (!audio || numFrames <= 0 || numChannels <= 0 || sampleRate <= 0) return res;
  if (!(t1 > t0) || (t1 - t0) > SPECTRAL_HEAL_MAX_SEC + 1e-9) return res;
  strength = std::max(0.0, std::min(1.0, strength));

  const int N = HEAL_FFT, H = HEAL_HOP, half = N / 2;
  if (numFrames < N) return res; // shorter than one analysis window

  int s0 = (int)(t0 * (double)sampleRate + 0.5);
  int s1 = (int)(t1 * (double)sampleRate + 0.5);
  s0 = std::max(0, std::min(numFrames, s0));
  s1 = std::max(0, std::min(numFrames, s1));
  if (s1 <= s0) return res;

  int kLo = (int)std::ceil(freqLo * (double)N / (double)sampleRate);
  int kHi = (int)std::floor(freqHi * (double)N / (double)sampleRate);
  kLo = std::max(1, kLo);          // DC and Nyquist stay untouched
  kHi = std::min(half - 1, kHi);
  if (kHi < kLo) return res;
  const int nBins = kHi - kLo + 1;

  // Selected frames: window center m*H + N/2 inside [s0, s1).
  const int mMax = (numFrames - N) / H; // last full in-buffer frame index
  int selLo = (int)std::ceil((double)(s0 - half) / (double)H);
  int selHi = (int)std::floor((double)(s1 - 1 - half) / (double)H);
  selLo = std::max(0, std::min(mMax, selLo));
  selHi = std::max(0, std::min(mMax, selHi));
  if (selHi < selLo) { // selection narrower than one hop: take the nearest frame
    int mid = (int)std::lround(((double)(s0 + s1) * 0.5 - (double)half) / (double)H);
    selLo = selHi = std::max(0, std::min(mMax, mid));
  }
  const int numSel = selHi - selLo + 1;
  const int ctxLen = std::max(8, numSel / 2); // auto context (kickoff decision)

  const int ctxB0 = std::max(0, selLo - ctxLen), ctxB1 = selLo - 1;
  const int ctxA0 = selHi + 1, ctxA1 = std::min(mMax, selHi + ctxLen);
  const int nCtxB = std::max(0, ctxB1 - ctxB0 + 1);
  const int nCtxA = std::max(0, ctxA1 - ctxA0 + 1);
  if (nCtxB + nCtxA == 0) return res; // nothing to interpolate from

  // Periodic Hann. The per-sample num/den division below makes reconstruction
  // exact for unmodified frames regardless of COLA details.
  std::vector<double> win((size_t)N);
  for (int i = 0; i < N; i++)
    win[(size_t)i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * (double)i / (double)N));

  // All frames whose windows overlap the modified sample span take part in the
  // weighted OLA; the unmodified neighbors blend the edges (crossfade for free).
  const int spread = N / H - 1;
  const int mLo = std::max(0, selLo - spread);
  const int mHi = std::min(mMax, selHi + spread);
  const int span0 = selLo * H;
  const int span1 = selHi * H + N; // <= numFrames since selHi <= mMax
  const int spanLen = span1 - span0;

  const double dPhiScale = 2.0 * M_PI * (double)H / (double)N; // * k = rad/hop

  std::vector<double> re((size_t)N), im((size_t)N);
  std::vector<double> ctxLogB((size_t)nBins), ctxLogA((size_t)nBins);
  std::vector<double> phiSeed((size_t)nBins);
  std::vector<double> num((size_t)spanLen), den((size_t)spanLen);
  double attenSum = 0.0;
  long long attenCount = 0;

  for (int ch = 0; ch < numChannels; ch++) {
    auto loadFrame = [&](int m) {
      for (int i = 0; i < N; i++) {
        re[(size_t)i] =
            audio[(size_t)(m * H + i) * (size_t)numChannels + (size_t)ch] * win[(size_t)i];
        im[(size_t)i] = 0.0;
      }
      Fft(re.data(), im.data(), N, false);
    };

    // Pass 1: per-bin average log-magnitudes of the context on each side, plus
    // the phase seed from the last frame before the selection.
    std::fill(ctxLogB.begin(), ctxLogB.end(), 0.0);
    std::fill(ctxLogA.begin(), ctxLogA.end(), 0.0);
    bool haveSeed = false;
    for (int m = ctxB0; m <= ctxB1; m++) {
      loadFrame(m);
      for (int k = kLo; k <= kHi; k++)
        ctxLogB[(size_t)(k - kLo)] +=
            std::log(std::hypot(re[(size_t)k], im[(size_t)k]) + EPS_MAG);
      if (m == selLo - 1) {
        for (int k = kLo; k <= kHi; k++)
          phiSeed[(size_t)(k - kLo)] = std::atan2(im[(size_t)k], re[(size_t)k]);
        haveSeed = true;
      }
    }
    for (int m = ctxA0; m <= ctxA1; m++) {
      loadFrame(m);
      for (int k = kLo; k <= kHi; k++)
        ctxLogA[(size_t)(k - kLo)] +=
            std::log(std::hypot(re[(size_t)k], im[(size_t)k]) + EPS_MAG);
    }
    for (int b = 0; b < nBins; b++) { // averages; one-sided selections extrapolate
      double lb = nCtxB ? ctxLogB[(size_t)b] / (double)nCtxB : 0.0;
      double la = nCtxA ? ctxLogA[(size_t)b] / (double)nCtxA : 0.0;
      if (!nCtxB) lb = la;
      if (!nCtxA) la = lb;
      ctxLogB[(size_t)b] = lb;
      ctxLogA[(size_t)b] = la;
    }

    // Pass 2: modify selected frames, resynthesize the span by weighted OLA.
    std::fill(num.begin(), num.end(), 0.0);
    std::fill(den.begin(), den.end(), 0.0);
    for (int m = mLo; m <= mHi; m++) {
      loadFrame(m);
      if (m >= selLo && m <= selHi) {
        double alpha = (double)(m - selLo + 1) / (double)(numSel + 1);
        for (int k = kLo; k <= kHi; k++) {
          size_t bi = (size_t)(k - kLo);
          double origRe = re[(size_t)k], origIm = im[(size_t)k];
          double origMag = std::hypot(origRe, origIm);
          double interpMag =
              std::exp((1.0 - alpha) * ctxLogB[bi] + alpha * ctxLogA[bi]);
          double target = std::min(origMag, interpMag); // only ever pull DOWN
          double newMag = (1.0 - strength) * origMag + strength * target;
          double phi;
          if (newMag >= PHASE_KEEP_RATIO * origMag || !haveSeed)
            phi = std::atan2(origIm, origRe); // mild attenuation: keep phase
          else                                // replaced: propagate at bin freq
            phi = phiSeed[bi] + (double)(m - (selLo - 1)) * dPhiScale * (double)k;
          re[(size_t)k] = newMag * std::cos(phi);
          im[(size_t)k] = newMag * std::sin(phi);
          re[(size_t)(N - k)] = re[(size_t)k]; // Hermitian mirror
          im[(size_t)(N - k)] = -im[(size_t)k];
          attenSum += 20.0 * std::log10((newMag + EPS_MAG) / (origMag + EPS_MAG));
          attenCount++;
        }
      }
      Fft(re.data(), im.data(), N, true);
      const int base = m * H - span0;
      const int iFrom = std::max(0, -base);
      const int iTo = std::min(N, spanLen - base);
      for (int i = iFrom; i < iTo; i++) {
        num[(size_t)(base + i)] += re[(size_t)i] * win[(size_t)i];
        den[(size_t)(base + i)] += win[(size_t)i] * win[(size_t)i];
      }
    }
    for (int o = 0; o < spanLen; o++) {
      if (den[(size_t)o] > 1e-9)
        audio[(size_t)(span0 + o) * (size_t)numChannels + (size_t)ch] =
            num[(size_t)o] / den[(size_t)o];
    }
  }

  res.ok = true;
  res.framesHealed = numSel;
  res.binsPerFrame = nBins;
  res.avgAttenDb = attenCount ? attenSum / (double)attenCount : 0.0;
  return res;
}

// ---------------------------------------------------------------------------
// Engine A — AR click repair
ClickRepairResult RepairClicksAR(double* audio, int numFrames, int numChannels,
                                 int sampleRate, double t0, double t1,
                                 double sensitivityK)
{
  ClickRepairResult res;
  if (!audio || numFrames <= 0 || numChannels <= 0 || sampleRate <= 0) return res;
  if (!(t1 > t0) || (t1 - t0) > CLICK_REPAIR_MAX_SEC + 1e-9) return res;
  const double K = std::max(1.0, std::min(10.0, sensitivityK));

  int s0 = (int)(t0 * (double)sampleRate + 0.5);
  int s1 = (int)(t1 * (double)sampleRate + 0.5);
  s0 = std::max(0, std::min(numFrames, s0));
  s1 = std::max(0, std::min(numFrames, s1));
  if (s1 <= s0) return res;

  // Paper defaults scaled to the session rate: N_max = 100 @44.1k,
  // p = 3*N_max + 2, N_w = 8p (> (8/3)p constraint), hop = N_w/4 (75% overlap).
  int p = 3 * (int)std::lround(AR_NMAX_44K * (double)sampleRate / 44100.0) + 2;
  p = std::min(p, AR_P_CAP);
  p = std::min(p, numFrames / 8 - 1); // short buffers: shrink the model to fit
  if (p < 16) return res;
  const int Nw = 8 * p;
  const int hop = Nw / 4;
  const int fuse = std::max(1, (int)std::lround(AR_FUSE_44K * (double)sampleRate / 44100.0));
  const int maxCluster = (int)(AR_MAX_BURST_SEC * (double)sampleRate);

  // Work region: selection + one window of context on each side. Frames may
  // extend outside the selection (AR context), detections never do.
  const int r0 = std::max(0, s0 - Nw);
  const int r1 = std::min(numFrames, s1 + Nw);
  const int regionLen = r1 - r0;
  const int selLen = s1 - s0;
  const int selLoR = s0 - r0, selHiR = s1 - r0; // selection in region coords

  std::vector<double> reg((size_t)regionLen);
  std::vector<double> R((size_t)p + 1), a((size_t)p + 1), bc((size_t)p + 1);
  std::vector<unsigned char> missing((size_t)Nw);
  std::vector<unsigned char> chRep((size_t)selLen), chSkip((size_t)selLen);
  std::vector<int> tIdx;

  for (int ch = 0; ch < numChannels; ch++) {
    for (int i = 0; i < regionLen; i++)
      reg[(size_t)i] = audio[(size_t)(r0 + i) * (size_t)numChannels + (size_t)ch];
    std::fill(chRep.begin(), chRep.end(), 0);
    std::fill(chSkip.begin(), chSkip.end(), 0);

    // Frame walk: 75% overlap guarantees every selected sample lands in the
    // interior [p, Nw-p) of some frame (the Janssen edge constraint). Repairs
    // write straight back into reg, so later frames see repaired data — with
    // only missing samples ever altered, windowed recombination is unnecessary.
    for (int f = std::max(0, selLoR - 2 * p);; f += hop) {
      bool lastFrame = false;
      if (f + Nw > regionLen) { f = regionLen - Nw; lastFrame = true; }
      if (f + p >= selHiR) break; // frame interior starts past the selection
      double* w = reg.data() + f;

      for (int iter = 0; iter < AR_ITER; iter++) {
        // Biased autocorrelation, rectangular window (as in IPOL 2018/23; the
        // 2015/64 Hamming variant only changes the estimate marginally).
        for (int tau = 0; tau <= p; tau++) {
          double acc = 0.0;
          for (int i = tau; i < Nw; i++) acc += w[i] * w[i - tau];
          R[(size_t)tau] = acc / (double)Nw;
        }
        if (R[0] < 1e-20) break; // silence
        double sig2 = 0.0;
        const int order = LevinsonDurbin(R.data(), p, a.data(), &sig2);
        if (order < 1 || sig2 <= 0.0) break;
        const double lambda = K * std::sqrt(sig2);

        // Detection: |prediction error| > lambda, restricted to the selection
        // and the frame interior; then burst fusion (gaps < fuse merge).
        std::fill(missing.begin(), missing.end(), 0);
        const int detLo = std::max(p, selLoR - f);
        const int detHi = std::min(Nw - p, selHiR - f);
        int nHits = 0, lastHit = -1;
        for (int t = detLo; t < detHi; t++) {
          double d = w[t];
          for (int k = 1; k <= order; k++) d += a[(size_t)k] * w[t - k];
          if (std::fabs(d) > lambda) {
            if (lastHit >= 0 && t - lastHit < fuse)
              for (int u = lastHit + 1; u < t; u++) missing[(size_t)u] = 1;
            missing[(size_t)t] = 1;
            lastHit = t;
            nHits++;
          }
        }
        if (!nHits) break;                       // clean frame
        if (nHits > (detHi - detLo) / 3) break;  // flooded: not click-like, skip

        tIdx.clear();
        for (int t = detLo; t < detHi; t++)
          if (missing[(size_t)t]) tIdx.push_back(t);

        ArCoeffAutocorr(a.data(), order, bc.data());

        // Solve per cluster: samples further apart than the model order do not
        // couple through B (b_m = 0 beyond order), so the system decouples.
        size_t c0 = 0;
        while (c0 < tIdx.size()) {
          size_t c1 = c0 + 1;
          while (c1 < tIdx.size() && tIdx[c1] - tIdx[c1 - 1] <= order) c1++;
          const int n = (int)(c1 - c0);
          const int* T = &tIdx[c0];
          const int spanSamples = T[n - 1] - T[0] + 1;
          const int gBase = r0 + f - s0; // frame-local -> selection-local
          if (spanSamples > maxCluster) {
            for (int i = 0; i < n; i++) {
              const int g = gBase + T[i];
              if (g >= 0 && g < selLen) chSkip[(size_t)g] = 1;
            }
            c0 = c1;
            continue;
          }
          std::vector<double> B((size_t)n * (size_t)n, 0.0);
          std::vector<double> rhs((size_t)n);
          std::vector<int> start((size_t)n);
          for (int i = 0; i < n; i++) {
            int st = 0;
            while (T[i] - T[st] > order) st++;
            start[(size_t)i] = st;
            for (int j = st; j <= i; j++) {
              double v = bc[(size_t)(T[i] - T[j])];
              B[(size_t)i * (size_t)n + (size_t)j] = v;
              B[(size_t)j * (size_t)n + (size_t)i] = v;
            }
            B[(size_t)i * (size_t)n + (size_t)i] += 1e-9 * bc[0]; // tiny ridge
            double d = 0.0; // d_t = sum over KNOWN neighbors within +-order
            for (int k = -order; k <= order; k++) {
              const int u = T[i] - k;
              if (!missing[(size_t)u])
                d += bc[(size_t)std::abs(k)] * w[u];
            }
            rhs[(size_t)i] = -d;
          }
          if (CholeskySolve(B, start, n, rhs)) {
            for (int i = 0; i < n; i++) {
              w[T[i]] = rhs[(size_t)i];
              const int g = gBase + T[i];
              if (g >= 0 && g < selLen) chRep[(size_t)g] = 1;
            }
          }
          c0 = c1;
        }
      }
      if (lastFrame) break;
    }

    for (int i = 0; i < selLen; i++) { // write back only repaired samples
      if (chRep[(size_t)i]) {
        audio[(size_t)(s0 + i) * (size_t)numChannels + (size_t)ch] =
            reg[(size_t)(selLoR + i)];
        res.samplesRepaired++;
      }
    }
    res.clicksRepaired = std::max(res.clicksRepaired, CountRuns(chRep));
    res.clicksSkipped = std::max(res.clicksSkipped, CountRuns(chSkip));
  }

  res.ok = true;
  return res;
}
