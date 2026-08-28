// dyn_chunk_test.cpp — chunked-detector equivalence check (v2.5 8f)
//
// DynTraceBuilder replaces DynamicsEngine::CollectPeaks and DeEssBandTrace with
// a resumable pass. Two exact claims, checked with memcmp on the doubles
// (exit 0 = all PASS, loop_finder_test style):
//   1. one whole-buffer Feed == the LEGACY loops (peak / RMS / band lanes);
//   2. Feed in chunks of every awkward size == one whole-buffer Feed.
// Deterministic signals (fixed-seed LCG), two sample rates, odd lengths so the
// last window is partial and RMS windows straddle every chunk boundary.

#include "dyn_trace.h"
#include "deess_engine.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int kNch = 2;
constexpr double kPi = 3.14159265358979323846;
constexpr double kStep = 0.001;

int g_failures = 0;

void Check(bool cond, const char* what)
{
  printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
  if (!cond) g_failures++;
}

struct Lcg {
  uint64_t state = 0x5EEDCAFEF00D1234ULL;
  double Next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return ((double)(state >> 11) / (double)(1ULL << 53)) * 2.0 - 1.0;
  }
};

// Stereo, right = -0.7 * left + its own noise so the channels differ.
std::vector<double> BuildSignal(int sr, int frames)
{
  Lcg lcg;
  std::vector<double> s((size_t)frames * kNch, 0.0);
  for (int i = 0; i < frames; i++) {
    const double t = (double)i / (double)sr;
    double l = 0.0;
    if (t < 0.4) l = 0.6 * std::sin(2.0 * kPi * 2000.0 * t);          // tone
    else if (t < 0.8) l = 0.02 * lcg.Next();                             // floor noise
    else if (t < 1.0) l = 0.8 * std::sin(2.0 * kPi * 12000.0 * t);      // HF burst
    else if (t < 1.3) l = 0.5 * std::sin(2.0 * kPi * 7000.0 * t) * std::fabs(std::sin(2.0 * kPi * 3.0 * t));
    else if (t < 1.6) l = 0.25;                                           // DC step
    else l = 0.1 * lcg.Next();
    if ((i % 9973) == 0) l = 0.95;                                        // isolated clicks
    s[(size_t)i * kNch] = l;
    s[(size_t)i * kNch + 1] = -0.7 * l + 0.01 * lcg.Next();
  }
  return s;
}

// --- Legacy detectors, copied verbatim from dynamics_engine.cpp @ 8912905 ---
void LegacyCollectPeaks(const double* audioData, int numFrames, int numChannels,
                        int sampleRate, bool rmsMode, double rmsWindowMs,
                        std::vector<double>& out)
{
  int samplesPerStep = std::max(1, (int)(kStep * sampleRate));
  int nch = std::max(1, numChannels);
  int rmsWindow = rmsMode
    ? std::max(samplesPerStep, (int)(rmsWindowMs / 1000.0 * sampleRate))
    : 0;
  out.clear();
  for (int frame = 0; frame < numFrames; frame += samplesPerStep) {
    int windowEnd = std::min(numFrames, frame + samplesPerStep);
    double value = 0.0;
    if (rmsMode) {
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
      for (int s = frame; s < windowEnd; s++) {
        for (int ch = 0; ch < nch; ch++) {
          double v = fabs(audioData[(size_t)s * nch + ch]);
          if (v > value) value = v;
        }
      }
    }
    out.push_back(value);
  }
}

bool SameDoubles(const std::vector<double>& a, const std::vector<double>& b)
{
  return a.size() == b.size() &&
         (a.empty() || memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0);
}

std::shared_ptr<const DynTrace> BuildChunked(const std::vector<double>& sig,
                                             const DynTraceKey& key, int chunk)
{
  DynTraceBuilder b;
  b.Begin(key);
  const int frames = (int)key.numFrames;
  for (int at = 0; at < frames; at += chunk)
    b.Feed(sig.data() + (size_t)at * kNch, std::min(chunk, frames - at));
  return b.Finish();
}

struct Mode {
  const char* name;
  bool rms; double rmsMs;
  bool ds; int dsMode; double f0, q;
};

void RunRate(int sr)
{
  const int frames = (int)(2.3 * sr) + 13;   // odd tail: partial last window
  const std::vector<double> sig = BuildSignal(sr, frames);
  const Mode modes[] = {
    { "peak",        false, 0.0,   false, 0, 0.0, 0.0 },
    { "rms5",        true,  5.0,   false, 0, 0.0, 0.0 },
    { "rms120",      true,  120.0, false, 0, 0.0, 0.0 },
    { "peak+bp6k",   false, 0.0,   true,  DEESS_MODE_BANDPASS, 6000.0, 2.0 },
    { "rms5+hp8k",   true,  5.0,   true,  DEESS_MODE_HIGHPASS, 8000.0, 2.0 },
    { "peak+hp20k",  false, 0.0,   true,  DEESS_MODE_HIGHPASS, 20000.0, 0.5 },  // f0 clamp
  };
  char label[160];
  for (const Mode& m : modes) {
    DynTraceKey key;
    key.sampleRate = sr;
    key.numChannels = kNch;
    key.numFrames = frames;
    key.rmsMode = m.rms;
    key.rmsWindowMs = m.rmsMs;
    key.dsEnable = m.ds;
    key.dsMode = m.dsMode;
    key.dsFreqHz = m.f0;
    key.dsQ = m.q;

    std::shared_ptr<const DynTrace> whole = BuildChunked(sig, key, frames);
    const int step = std::max(1, (int)(kStep * sr));
    snprintf(label, sizeof(label), "%d Hz %s: count == ceil(N/step)", sr, m.name);
    Check((int)whole->Count() == (frames + step - 1) / step, label);

    std::vector<double> legacyPeak, legacyBand;
    LegacyCollectPeaks(sig.data(), frames, kNch, sr, m.rms, m.rmsMs, legacyPeak);
    snprintf(label, sizeof(label), "%d Hz %s: whole-buffer peak lane == legacy CollectPeaks", sr, m.name);
    Check(SameDoubles(whole->peak, legacyPeak), label);
    if (m.ds) {
      DeEssBandTrace(sig.data(), frames, kNch, sr, kStep, m.dsMode, m.f0, m.q, legacyBand);
      snprintf(label, sizeof(label), "%d Hz %s: whole-buffer band lane == legacy DeEssBandTrace", sr, m.name);
      Check(SameDoubles(whole->band, legacyBand), label);
    } else {
      snprintf(label, sizeof(label), "%d Hz %s: no band lane when ds is off", sr, m.name);
      Check(whole->band.empty(), label);
    }

    const int W = m.rms ? std::max(step, (int)(m.rmsMs / 1000.0 * sr)) : step;
    const int chunks[] = { 1, 7, step - 1, step + 1, 1000, 4097, W - 1, W + 1, 65536 };
    for (int c : chunks) {
      if (c < 1) continue;
      std::shared_ptr<const DynTrace> part = BuildChunked(sig, key, c);
      snprintf(label, sizeof(label), "%d Hz %s: chunk %d == whole (peak lane)", sr, m.name, c);
      Check(SameDoubles(part->peak, whole->peak), label);
      snprintf(label, sizeof(label), "%d Hz %s: chunk %d == whole (band lane)", sr, m.name, c);
      Check(SameDoubles(part->band, whole->band), label);
    }
  }
}

} // namespace

int main()
{
  RunRate(44100);
  RunRate(48000);
  // Time grid: TimeAt must be the legacy (double)frame / (double)rate.
  {
    DynTraceKey key;
    key.sampleRate = 44100;
    key.numChannels = 1;
    key.numFrames = 44;
    DynTraceBuilder b;
    b.Begin(key);
    const double z[44] = {};
    b.Feed(z, 44);
    auto t = b.Finish();
    Check(t->Count() == 1 && t->TimeAt(3) == (double)(3 * 44) / 44100.0, "TimeAt == legacy frame/rate");
  }
  printf("\n%s (%d failure%s)\n", g_failures ? "DYN CHUNK TEST: RED" : "DYN CHUNK TEST: GREEN",
         g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
