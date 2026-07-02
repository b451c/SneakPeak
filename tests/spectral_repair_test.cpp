// spectral_repair_test.cpp — offline correctness harness for INC-5 (v2.3.0)
//
// Synthetic click / beep scenarios through spectral_repair with SNR-improvement
// assertions — NOT bit-baselines (unlike dyn_regression), so it is safe to run
// on any machine/compiler. Deterministic: no rand()/time(), noise comes from a
// fixed-seed LCG. Exit 0 = all PASS.

#include "spectral_repair.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr int kSr = 48000;
constexpr int kNch = 2;
constexpr double kPi = 3.14159265358979323846;

int g_failures = 0;

void Check(bool cond, const char* what)
{
  printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
  if (!cond) g_failures++;
}

void CheckNear(double value, double atLeast, const char* what)
{
  printf("%s: %s (%.2f, need >= %.2f)\n", value >= atLeast ? "PASS" : "FAIL",
         what, value, atLeast);
  if (value < atLeast) g_failures++;
}

// Fixed-seed 64-bit LCG (Knuth constants) — same pattern as dyn_regression.
struct Lcg {
  uint64_t state = 0x5EEDCAFEF00D1234ULL;
  double Next() { // uniform in [-1, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return ((double)(state >> 11) / (double)(1ULL << 53)) * 2.0 - 1.0;
  }
};

int SamplesFor(double seconds) { return (int)(seconds * kSr + 0.5); }

// Interleaved stereo; right channel = 0.5 * left (real per-channel work).
void Push(std::vector<double>& buf, double left)
{
  buf.push_back(left);
  buf.push_back(left * 0.5);
}

// SNR of test against reference over frame range [a, b), channel 0, in dB.
double SnrDb(const std::vector<double>& ref, const std::vector<double>& test,
             int a, int b)
{
  double sig = 0.0, err = 0.0;
  for (int i = a; i < b; i++) {
    double r = ref[(size_t)i * kNch];
    double e = test[(size_t)i * kNch] - r;
    sig += r * r;
    err += e * e;
  }
  if (err <= 0.0) return 999.0;
  return 10.0 * std::log10(sig / err);
}

// Single-bin DFT amplitude of a tone over frame range [a, b) (rectangular
// window; callers pick ranges holding an integer number of cycles).
double ToneAmp(const std::vector<double>& buf, int ch, int a, int b, double freq)
{
  double re = 0.0, im = 0.0;
  for (int i = a; i < b; i++) {
    double ph = 2.0 * kPi * freq * (double)(i - a) / (double)kSr;
    double x = buf[(size_t)i * kNch + (size_t)ch];
    re += x * std::cos(ph);
    im -= x * std::sin(ph);
  }
  return 2.0 * std::hypot(re, im) / (double)(b - a);
}

// Max |test - ref| on both channels over frame range [a, b).
double MaxDiff(const std::vector<double>& ref, const std::vector<double>& test,
               int a, int b)
{
  double md = 0.0;
  for (int i = a; i < b; i++)
    for (int c = 0; c < kNch; c++) {
      double d = std::fabs(test[(size_t)i * kNch + (size_t)c] -
                           ref[(size_t)i * kNch + (size_t)c]);
      if (d > md) md = d;
    }
  return md;
}

// --- Scenario 1: Engine A — clicks on a sine + noise floor ---------------
void TestClickRepair()
{
  printf("-- Engine A: RepairClicksAR --\n");
  Lcg lcg;
  std::vector<double> clean;
  int total = SamplesFor(4.0);
  clean.reserve((size_t)total * kNch);
  for (int i = 0; i < total; i++) {
    double t = (double)i / (double)kSr;
    Push(clean, 0.5 * std::sin(2.0 * kPi * 440.0 * t) + 0.001 * lcg.Next());
  }

  // 7 clicks, 0.5 s apart: 4-sample alternating spikes riding on the signal.
  std::vector<double> dirty = clean;
  const double spike[4] = { 0.8, -0.7, 0.75, -0.6 };
  for (int c = 0; c < 7; c++) {
    int at = SamplesFor(0.5 + 0.5 * (double)c);
    for (int j = 0; j < 4; j++) {
      dirty[(size_t)(at + j) * kNch] += spike[j];
      dirty[(size_t)(at + j) * kNch + 1] += spike[j] * 0.5;
    }
  }
  const std::vector<double> dirtyOrig = dirty;

  const double t0 = 0.25, t1 = 3.75;
  const int s0 = SamplesFor(t0), s1 = SamplesFor(t1);
  double snrBefore = SnrDb(clean, dirty, s0, s1);

  ClickRepairResult r = RepairClicksAR(dirty.data(), total, kNch, kSr, t0, t1, 2.0);

  Check(r.ok, "click repair returns ok");
  printf("  clicks=%d samples=%d skipped=%d\n", r.clicksRepaired,
         r.samplesRepaired, r.clicksSkipped);
  Check(r.clicksRepaired >= 5 && r.clicksRepaired <= 9,
        "burst count near the 7 injected clicks");
  Check(r.clicksSkipped == 0, "no bursts over the repair cap");
  double snrAfter = SnrDb(clean, dirty, s0, s1);
  printf("  SNR before=%.1f dB after=%.1f dB\n", snrBefore, snrAfter);
  CheckNear(snrAfter - snrBefore, 20.0, "SNR improvement");
  CheckNear(snrAfter, 45.0, "absolute SNR after repair");
  Check(MaxDiff(dirtyOrig, dirty, 0, s0) == 0.0 &&
        MaxDiff(dirtyOrig, dirty, s1, total) == 0.0,
        "samples outside the selection bit-identical");

  // Over-cap selection must hard-fail (defense in depth).
  std::vector<double> big((size_t)SamplesFor(5.0) * kNch, 0.0);
  ClickRepairResult r2 =
      RepairClicksAR(big.data(), SamplesFor(5.0), kNch, kSr, 0.0, 4.5, 2.0);
  Check(!r2.ok, "selection over CLICK_REPAIR_MAX_SEC rejected");
}

// --- Scenario 2: Engine B — beep inside a band --------------------------
void TestSpectralHeal()
{
  printf("-- Engine B: StftRepairRect --\n");
  Lcg lcg;
  std::vector<double> base;
  int total = SamplesFor(6.0);
  base.reserve((size_t)total * kNch);
  int b0 = SamplesFor(2.5), b1 = SamplesFor(3.0);
  for (int i = 0; i < total; i++) {
    double t = (double)i / (double)kSr;
    double x = 0.4 * std::sin(2.0 * kPi * 500.0 * t) + 0.005 * lcg.Next();
    if (i >= b0 && i < b1) x += 0.5 * std::sin(2.0 * kPi * 3000.0 * t);
    Push(base, x);
  }
  const std::vector<double> dirtyOrig = base;

  const double t0 = 2.4, t1 = 3.1;
  SpectralHealResult r = StftRepairRect(base.data(), total, kNch, kSr, t0, t1,
                                        2500.0, 3500.0, 1.0);

  Check(r.ok, "heal returns ok");
  printf("  framesHealed=%d binsPerFrame=%d avgAtten=%.1f dB\n", r.framesHealed,
         r.binsPerFrame, r.avgAttenDb);
  Check(r.framesHealed > 0, "frames were healed");
  Check(r.avgAttenDb < 0.0, "average attenuation is negative");

  // Measure over [2.55, 2.95]: 0.4 s = integer cycles of both 3 kHz and 500 Hz.
  int ma = SamplesFor(2.55), mb = SamplesFor(2.95);
  for (int ch = 0; ch < kNch; ch++) {
    double beepBefore = ToneAmp(dirtyOrig, ch, ma, mb, 3000.0);
    double beepAfter = ToneAmp(base, ch, ma, mb, 3000.0);
    double drop = 20.0 * std::log10(beepBefore / (beepAfter + 1e-12));
    char label[64];
    snprintf(label, sizeof(label), "3 kHz beep attenuation, channel %d", ch);
    CheckNear(drop, 30.0, label);
    double toneBefore = ToneAmp(dirtyOrig, ch, ma, mb, 500.0);
    double toneAfter = ToneAmp(base, ch, ma, mb, 500.0);
    double shift = std::fabs(20.0 * std::log10(toneAfter / toneBefore));
    snprintf(label, sizeof(label), "500 Hz kept within 0.5 dB, channel %d", ch);
    Check(shift <= 0.5, label);
  }

  // Untouched outside the modified span (selection +- one analysis window).
  int guard = 2048;
  Check(MaxDiff(dirtyOrig, base, 0, SamplesFor(t0) - guard) == 0.0 &&
        MaxDiff(dirtyOrig, base, SamplesFor(t1) + guard, total) == 0.0,
        "samples outside selection +- window bit-identical");

  // Strength 0 must be a no-op through the whole STFT round-trip.
  std::vector<double> noop = dirtyOrig;
  SpectralHealResult r0 = StftRepairRect(noop.data(), total, kNch, kSr, t0, t1,
                                         2500.0, 3500.0, 0.0);
  Check(r0.ok, "strength 0 returns ok");
  Check(MaxDiff(dirtyOrig, noop, 0, total) < 1e-9,
        "strength 0 output equals input (< 1e-9)");

  // Over-cap selection must hard-fail (defense in depth).
  SpectralHealResult r2 = StftRepairRect(noop.data(), total, kNch, kSr, 0.1,
                                         10.5, 2500.0, 3500.0, 1.0);
  Check(!r2.ok, "selection over SPECTRAL_HEAL_MAX_SEC rejected");
}

} // namespace

int main()
{
  TestClickRepair();
  TestSpectralHeal();
  printf("%s (%d failure%s)\n", g_failures ? "RESULT: FAIL" : "RESULT: PASS",
         g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
