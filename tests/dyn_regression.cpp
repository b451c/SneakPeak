// dyn_regression.cpp — offline envelope-diff regression harness (v2.3.0 INC-0)
//
// Synthesizes deterministic test signals, runs the full DynamicsEngine pipeline
// (Analyze -> ComputeCompression -> SimplifyCurve at the apply path's 0.3 dB
// epsilon) and prints a canonical dump to stdout. tests/run_dyn_regression.sh
// byte-diffs the dump against a recorded baseline in tests/baselines/.
//
// Purpose: every v2.3.0 engine change (gate rewrite, upward compression,
// de-esser) must keep legacy parameter sets BIT-IDENTICAL. Any diff against
// the baseline is either a bug or an intended behavior change that must be
// re-baselined in its own commit.
//
// Determinism notes: no rand()/time() — noise comes from a fixed-seed LCG and
// all signal math is closed-form. Values print at %.17g (round-trip exact for
// double), so baselines are machine-local: record and check on the same
// machine/compiler. Do NOT check baselines on CI.

#include "dynamics_engine.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr double kPi = 3.14159265358979323846;
constexpr double kRdpEpsilonDb = 0.3; // matches ApplyDynamicsToEnvelope

// Fixed-seed 64-bit LCG (Knuth constants): deterministic "breath" noise.
struct Lcg {
  uint64_t state = 0x5EEDCAFEF00D1234ULL;
  double Next() { // uniform in [-1, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return ((double)(state >> 11) / (double)(1ULL << 53)) * 2.0 - 1.0;
  }
};

// Interleaved stereo buffer; right channel = 0.5 * left so the per-channel
// peak scan in CollectPeaks has real work to do.
struct Signal {
  const char* name;
  std::vector<double> frames;

  void Push(double left) {
    frames.push_back(left);
    frames.push_back(left * 0.5);
  }
  int NumFrames() const { return (int)(frames.size() / kChannels); }
};

int SamplesFor(double seconds) { return (int)(seconds * kSampleRate + 0.5); }

void AppendSilence(Signal& sig, double seconds)
{
  int n = SamplesFor(seconds);
  for (int i = 0; i < n; i++) sig.Push(0.0);
}

void AppendNoise(Signal& sig, double seconds, double amp, Lcg& lcg)
{
  int n = SamplesFor(seconds);
  for (int i = 0; i < n; i++) sig.Push(amp * lcg.Next());
}

// Sine tone with amplitude interpolated ampStart -> ampEnd (linear or
// exponential) and an optional |sin| tremolo (speech-burst envelope).
void AppendTone(Signal& sig, double seconds, double freqHz,
                double ampStart, double ampEnd,
                bool expInterp = false, double modHz = 0.0)
{
  int n = SamplesFor(seconds);
  for (int i = 0; i < n; i++) {
    double t = (double)i / (double)kSampleRate;
    double frac = (n > 1) ? (double)i / (double)(n - 1) : 0.0;
    double amp;
    if (expInterp) {
      double a0 = (ampStart > 1e-6) ? ampStart : 1e-6;
      double a1 = (ampEnd > 1e-6) ? ampEnd : 1e-6;
      amp = a0 * pow(a1 / a0, frac);
    } else {
      amp = ampStart + (ampEnd - ampStart) * frac;
    }
    if (modHz > 0.0) amp *= fabs(sin(2.0 * kPi * modHz * t));
    sig.Push(amp * sin(2.0 * kPi * freqHz * t));
  }
}

// Voice-like program: silence, breath bed, speech burst, transient click,
// decaying tail into silence, steady passage, fade-out. Exercises threshold/
// knee traversal, attack/release, lookahead, gate open/hold/close and EPSILON.
Signal BuildVoiceSignal()
{
  Signal sig{"voice", {}};
  Lcg lcg;
  AppendSilence(sig, 0.5);
  AppendNoise(sig, 1.0, 0.005, lcg);                  // breath bed ~ -46 dB
  AppendTone(sig, 1.5, 220.0, 0.25, 0.25, false, 3.0); // speech burst ~ -12 dB
  AppendTone(sig, 0.02, 1000.0, 0.9, 0.9);             // transient click
  AppendTone(sig, 1.3, 220.0, 0.25, 0.001, true);      // decay to -60 dB
  AppendSilence(sig, 1.0);
  AppendTone(sig, 1.5, 330.0, 0.1, 0.1);               // steady -20 dB
  AppendTone(sig, 1.0, 330.0, 0.1, 0.0);               // linear fade-out
  return sig;
}

// Full-range triangle sweep: 0 -> full scale -> 0. Walks the static curve
// through the knee in both directions at every level.
Signal BuildRampSignal()
{
  Signal sig{"ramp", {}};
  AppendTone(sig, 2.0, 440.0, 0.0, 1.0);
  AppendTone(sig, 2.0, 440.0, 1.0, 0.0);
  return sig;
}

// FNV-1a 64-bit over formatted text — one line per curve point. Catches any
// numeric drift in the full (pre-RDP) curve without dumping ~8k lines each.
struct Fnv {
  uint64_t h = 1469598103934665603ULL;
  void Add(const char* s) {
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
      h ^= (uint64_t)(*p);
      h *= 1099511628211ULL;
    }
  }
};

struct Scenario {
  const char* name;
  DynamicsParams params;
  double itemVolDb;
};

void RunScenario(const Scenario& sc, const Signal& sig)
{
  DynamicsEngine engine;
  engine.Analyze(sig.frames.data(), sig.NumFrames(), kChannels,
                 kSampleRate, sc.itemVolDb, sc.params);
  auto curve = engine.ComputeCompression();
  auto simplified = DynamicsEngine::SimplifyCurve(curve, kRdpEpsilonDb);

  double avgGR = engine.GetAvgGainReduction();
  double makeup = sc.params.autoMakeup ? -avgGR : sc.params.makeupDb;

  char paramStr[256];
  DynamicsParamsToString(sc.params, paramStr, sizeof(paramStr));

  printf("=== %s | %s | itemVol=%.2f ===\n", sc.name, sig.name, sc.itemVolDb);
  printf("params: %s\n", paramStr);
  printf("avgPeakDb=%.17g threshold=%.17g avgGR=%.17g makeup=%.17g\n",
         engine.GetAveragePeakDb(), engine.GetThreshold(), avgGR, makeup);

  char line[80];
  Fnv curveHash, grHash;
  for (const auto& cp : curve) {
    snprintf(line, sizeof(line), "%.17g %.17g\n", cp.time, cp.dbAdjust);
    curveHash.Add(line);
  }
  for (const auto& pt : engine.GetResults()) {
    snprintf(line, sizeof(line), "%.17g\n", pt.smoothedGR);
    grHash.Add(line);
  }
  printf("full: n=%d hash=%016llx grhash=%016llx\n",
         (int)curve.size(),
         (unsigned long long)curveHash.h, (unsigned long long)grHash.h);

  printf("simplified: n=%d\n", (int)simplified.size());
  for (const auto& cp : simplified)
    printf("%.17g %.17g\n", cp.time, cp.dbAdjust);
  printf("\n");
}

} // namespace

int main()
{
  printf("# SneakPeak dynamics regression dump (INC-0 harness)\n");
  printf("# sampleRate=%d channels=%d rdpEpsilon=%.2f\n\n",
         kSampleRate, kChannels, kRdpEpsilonDb);

  Signal signals[] = { BuildVoiceSignal(), BuildRampSignal() };

  std::vector<Scenario> scenarios;
  for (int i = 0; i < PRESET_COUNT; i++)
    scenarios.push_back({g_dynamicsPresets[i].name, g_dynamicsPresets[i].params, 0.0});

  // Positional init — field order in DynamicsParams (same rule as the preset
  // table): threshold, ratio, kneeDb, makeupDb, autoMakeup, attackMs,
  // releaseMs, lookaheadMs, rmsMode, rmsWindowMs, gateThreshDb, gateRangeDb,
  // gateHoldMs, minDb, maxDb.
  scenarios.push_back({"sentinel-deepgate",
    { -100.0, 3.0, 6.0, 0.0, true, 5.0, 100.0, 10.0, false, 5.0,
      -50.0, -40.0, 100.0, -60.0, 6.0 }, 0.0});
  scenarios.push_back({"limiter-extremes",
    { -10.0, 20.0, 0.0, 6.0, false, 0.0, 10.0, 20.0, false, 5.0,
      -100.0, -20.0, 50.0, -60.0, 6.0 }, 0.0});
  scenarios.push_back({"itemvol-rms",
    { -24.0, 4.0, 6.0, 0.0, true, 5.0, 80.0, 5.0, true, 5.0,
      -45.0, -18.0, 80.0, -60.0, 6.0 }, -6.0});

  // v2.3.0 INC-1 gate extension: exercise the NEW trailing params
  // (gateRatio, gateHystDb, gateAttackMs, gateReleaseMs).
  scenarios.push_back({"gate-ratio4-hyst6",
    { -24.0, 2.0, 6.0, 0.0, true, 5.0, 100.0, 0.0, false, 5.0,
      -45.0, -40.0, 50.0, -60.0, 6.0,
      4.0, -6.0, 5.0, 200.0 }, 0.0});
  scenarios.push_back({"gate-deepfloor-hard",
    { -24.0, 2.0, 6.0, 0.0, true, 5.0, 100.0, 0.0, false, 5.0,
      -70.0, -80.0, 100.0, -60.0, 6.0,
      10.0, -12.0, 0.5, 50.0 }, 0.0});

  for (const auto& sig : signals)
    for (const auto& sc : scenarios)
      RunScenario(sc, sig);

  return 0;
}
