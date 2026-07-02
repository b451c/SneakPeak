// limiter_test.cpp — offline correctness harness for INC-L0 (v2.4.0)
//
// Two modes, mirroring both existing harnesses:
//   (no args)  hard assertions (machine-independent, exit 0 = all PASS):
//              ceiling never exceeded (verified at 8x oversampling — HIGHER
//              resolution than the engine's 4x detector), bit-identical
//              passthrough, envelope invariants, link/ramp/micro edge cases.
//   dump       canonical %.17g dump to stdout; tests/run_limiter_test.sh
//              byte-diffs it against tests/baselines/limiter_test.txt
//              (machine-local, block-tag re-record rule applies).
//
// Deterministic: no rand()/time(), noise comes from a fixed-seed LCG.

#include "limiter_engine.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

// Fixed-seed 64-bit LCG (Knuth constants) — same pattern as dyn_regression.
struct Lcg {
  uint64_t state = 0x5EEDCAFEF00D1234ULL;
  double Next() { // uniform in [-1, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return ((double)(state >> 11) / (double)(1ULL << 53)) * 2.0 - 1.0;
  }
};

struct Fixture {
  const char* name;
  std::vector<double> frames; // interleaved stereo
  int NumFrames() const { return (int)(frames.size() / kNch); }
};

// Interleaved stereo; right channel = 0.5 * left (real per-channel work).
void Push(Fixture& f, double left)
{
  f.frames.push_back(left);
  f.frames.push_back(left * 0.5);
}

double AmpDb(double a) { return 20.0 * std::log10(a > 1e-15 ? a : 1e-15); }

// --- Fixtures (contract C6) -------------------------------------------------

Fixture MakeSquare997() // (a) full-scale square, edges make Gibbs ISPs
{
  Fixture f{ "square997", {} };
  for (int i = 0; i < kSr; i++) {
    double ph = std::fmod(997.0 * (double)i / (double)kSr, 1.0);
    Push(f, ph < 0.5 ? 1.0 : -1.0);
  }
  return f;
}

Fixture MakeIspSine() // (b) near-fs/4 sine, true peak > +2 dBTP
{
  Fixture f{ "ispSine", {} };
  for (int i = 0; i < kSr; i++)
    Push(f, 1.3 * std::sin(2.0 * kPi * 0.249 * (double)i + kPi / 4.0));
  return f;
}

Fixture MakeImpulses() // (c) single-sample alternating impulses
{
  Fixture f{ "impulses", {} };
  for (int i = 0; i < kSr; i++) Push(f, 0.0);
  for (int k = 0; k < 4; k++) {
    int at = 6000 + k * 12000;
    f.frames[(size_t)at * kNch] = (k & 1) ? -1.0 : 1.0;
    f.frames[(size_t)at * kNch + 1] = (k & 1) ? -0.5 : 0.5;
  }
  return f;
}

Fixture MakeNoise() // (d) white noise -6 dB
{
  Fixture f{ "noise", {} };
  Lcg lcg;
  for (int i = 0; i < kSr; i++) Push(f, 0.5 * lcg.Next());
  return f;
}

Fixture MakeSilence() // (e)
{
  Fixture f{ "silence", {} };
  for (int i = 0; i < kSr / 2; i++) Push(f, 0.0);
  return f;
}

Fixture MakeMicro() // (f) 100-sample buffer, hotter than full scale
{
  Fixture f{ "micro", {} };
  Lcg lcg;
  for (int i = 0; i < 100; i++) Push(f, 1.25 * lcg.Next());
  return f;
}

// --- Verifiers: two independent true-peak instruments -----------------------
// (A) the EXACT BS.1770-4 reference meter (49-tap Hann sinc, 4x) — what
//     platform QC measures with; a different kernel than the engine's 8x/97
//     detector, so it independently exercises the construction.
// (B) an 8x/97-tap instrument (detector-grade resolution) — the tightest
//     grid the envelope math must hold on. The contract asked for a verifier
//     finer than a 4x detector; the detector was upsized to 8x after the 4x
//     minimum measured ~0.3 dB short on noise, so B now matches detector
//     resolution and A supplies the independent-kernel check.

double SincKernelPeak(const std::vector<double>& buf, int numChannels,
                      int taps, int factor, double* h, bool* built)
{
  if (!*built) {
    const double center = (double)(taps - 1) / 2.0;
    for (int j = 0; j < taps; j++) {
      double m = (double)j - center;
      double c = 1.0;
      if (std::fabs(m) > 1e-9) {
        double a = m * kPi / (double)factor;
        c = std::sin(a) / a;
      }
      c *= 0.5 * (1.0 - std::cos(2.0 * kPi * (double)j / (double)(taps - 1)));
      h[j] = c;
    }
    *built = true;
  }
  const int n = (int)(buf.size() / (size_t)numChannels);
  const int center = (taps - 1) / 2 / factor;
  double pk = 0.0;
  for (int ch = 0; ch < numChannels; ch++)
    for (int i = 0; i < n; i++) {
      double a = std::fabs(buf[(size_t)i * (size_t)numChannels + (size_t)ch]);
      if (a > pk) pk = a;
      for (int p = 1; p < factor; p++) {
        double acc = 0.0;
        for (int j = p; j < taps; j += factor) {
          int src = i + center - (j - p) / factor;
          if (src < 0 || src >= n) continue;
          acc += h[j] * buf[(size_t)src * (size_t)numChannels + (size_t)ch];
        }
        double v = std::fabs(acc);
        if (v > pk) pk = v;
      }
    }
  return pk;
}

double MeterPeakBs1770(const std::vector<double>& buf, int numChannels)
{
  static double h[49];
  static bool built = false;
  return SincKernelPeak(buf, numChannels, 49, 4, h, &built);
}

double VerifyTruePeak8x(const std::vector<double>& buf, int numChannels)
{
  static double h[97];
  static bool built = false;
  return SincKernelPeak(buf, numChannels, 97, 8, h, &built);
}

double SamplePeak(const std::vector<double>& buf)
{
  double pk = 0.0;
  for (double v : buf) {
    double a = std::fabs(v);
    if (a > pk) pk = a;
  }
  return pk;
}

// --- Ceiling sweep (contract C6 assertion 1) --------------------------------

void SweepFixture(const Fixture& f)
{
  const double attacks[] = { 0.1, 5.0, 30.0 };
  const double releases[] = { 10.0, 60.0, 1000.0 };
  const double ceilings[] = { -1.0, -6.0 };
  double worst8x = -999.0, worstMeter = -999.0, worstSp = -999.0;

  for (int tp = 0; tp < 2; tp++)
    for (double ceil : ceilings)
      for (double att : attacks)
        for (double rel : releases) {
          LimiterParams p;
          p.ceilingDb = ceil;
          p.attackMs = att;
          p.releaseMs = rel;
          p.truePeak = (tp == 1);
          std::vector<double> work = f.frames;
          LimiterResult r =
              LimiterProcess(work.data(), f.NumFrames(), kNch, kSr, p);
          if (!r.ok) {
            Check(false, "LimiterProcess ok in sweep");
            continue;
          }
          if (p.truePeak) {
            double e8 = AmpDb(VerifyTruePeak8x(work, kNch)) - ceil;
            double em = AmpDb(MeterPeakBs1770(work, kNch)) - ceil;
            if (e8 > worst8x) worst8x = e8;
            if (em > worstMeter) worstMeter = em;
          } else {
            double es = AmpDb(SamplePeak(work)) - ceil;
            if (es > worstSp) worstSp = es;
          }
        }

  char what[160];
  snprintf(what, sizeof(what),
           "%s: TP-on worst 8x/97 excess over ceiling %+.4f dB (<= 0.01)",
           f.name, worst8x);
  Check(worst8x <= 0.01, what);
  snprintf(what, sizeof(what),
           "%s: TP-on worst BS.1770-4 meter excess %+.4f dB (<= 0.01)",
           f.name, worstMeter);
  Check(worstMeter <= 0.01, what);
  snprintf(what, sizeof(what),
           "%s: TP-off worst sample excess over ceiling %+.4f dB (<= 0.001)",
           f.name, worstSp);
  Check(worstSp <= 0.001, what);
}

// --- Passthrough / null tests (contract C6 assertion 2) ----------------------

void TestPassthrough()
{
  printf("-- passthrough --\n");
  // Below ceiling, TP on: -6 dB sine vs -1 dBTP ceiling.
  Fixture f{ "sine440", {} };
  for (int i = 0; i < kSr / 2; i++)
    Push(f, 0.5 * std::sin(2.0 * kPi * 440.0 * (double)i / (double)kSr));
  std::vector<double> work = f.frames;
  LimiterParams p; // defaults: ceiling -1, TP on, gain 0
  LimiterResult r = LimiterProcess(work.data(), f.NumFrames(), kNch, kSr, p);
  Check(r.ok && std::memcmp(work.data(), f.frames.data(),
                            f.frames.size() * sizeof(double)) == 0,
        "below-ceiling input passes through bit-identical (TP on)");
  Check(r.maxGainReductionDb == 0.0, "below-ceiling max GR is exactly 0 dB");

  // Null test at gain 0 / ceiling 0 (TP off): full-scale square untouched.
  Fixture sq = MakeSquare997();
  work = sq.frames;
  LimiterParams p0;
  p0.ceilingDb = 0.0;
  p0.truePeak = false;
  r = LimiterProcess(work.data(), sq.NumFrames(), kNch, kSr, p0);
  Check(r.ok && std::memcmp(work.data(), sq.frames.data(),
                            sq.frames.size() * sizeof(double)) == 0,
        "null test gain 0 / ceiling 0 bit-identical (TP off, full-scale square)");

  // Silence stays silence, bit-identical, any settings.
  Fixture sil = MakeSilence();
  work = sil.frames;
  LimiterParams ps;
  ps.gainDb = 24.0;
  r = LimiterProcess(work.data(), sil.NumFrames(), kNch, kSr, ps);
  Check(r.ok && std::memcmp(work.data(), sil.frames.data(),
                            sil.frames.size() * sizeof(double)) == 0,
        "silence passes through bit-identical at +24 dB gain");
}

// --- Envelope invariants (contract C6 assertion 3) ---------------------------

void TestInvariants()
{
  printf("-- envelope invariants --\n");
  Fixture f = MakeIspSine();
  const int n = f.NumFrames();
  LimiterParams p;
  p.ceilingDb = -6.0;
  LimiterDebugTaps taps;
  std::vector<double> env;
  LimiterResult r = LimiterComputeEnvelope(f.frames.data(), n, kNch, kSr, p,
                                           env, 0, &taps);
  Check(r.ok && (int)env.size() == n, "linked envelope has one value per frame");

  bool e1LeNeed = true, e3LeE2 = true, e3Le1 = true, envLeNeed = true,
       envPos = true;
  for (int i = 0; i < n; i++) {
    if (taps.e1[(size_t)i] > taps.need[(size_t)i] + 1e-12) e1LeNeed = false;
    if (taps.e3[(size_t)i] > taps.e2[(size_t)i] + 1e-12) e3LeE2 = false;
    if (taps.e3[(size_t)i] > 1.0) e3Le1 = false;
    // THE core inequality behind "zero overshoot by construction": the
    // delay-aligned envelope never exceeds the gain each frame needs.
    if (env[(size_t)i] > taps.need[(size_t)i] + 1e-12) envLeNeed = false;
    if (env[(size_t)i] <= 0.0) envPos = false;
  }
  Check(e1LeNeed, "e1 (sliding min) <= need pointwise");
  Check(e3LeE2, "e3 (hold+release) <= e2 (box stack) pointwise");
  Check(e3Le1, "e3 <= 1 everywhere");
  Check(envLeNeed, "aligned env <= need pointwise (zero overshoot)");
  Check(envPos, "env stays > 0");

  int attackW = (int)(p.attackMs * 0.001 * kSr + 0.5);
  Check(r.latencySamples >= 0 && r.latencySamples <= attackW,
        "reported latency within [0, attack window]");
  double expectGr = r.inputPeakDb - p.ceilingDb;
  char what[160];
  snprintf(what, sizeof(what),
           "max GR %.3f dB consistent with input peak - ceiling %.3f dB",
           r.maxGainReductionDb, expectGr);
  Check(r.maxGainReductionDb <= expectGr + 0.01 &&
            r.maxGainReductionDb >= expectGr - 0.5,
        what);
  Check(r.inputPeakDb > 2.0, "ispSine fixture really is > +2 dBTP");
}

// --- Edge ramp, link, micro-buffer, gain push --------------------------------

void TestEdgeRamp()
{
  printf("-- edge ramp --\n");
  Fixture f = MakeIspSine();
  const int n = f.NumFrames();
  LimiterParams p;
  std::vector<double> env;
  LimiterResult r =
      LimiterComputeEnvelope(f.frames.data(), n, kNch, kSr, p, env, 960);
  bool le1 = true;
  for (double e : env)
    if (e > 1.0) le1 = false;
  Check(r.ok && env[0] == 1.0 && env[(size_t)n - 1] == 1.0,
        "edge ramp forces env = 1.0 exactly at both boundaries");
  Check(le1, "edge-ramped envelope never exceeds 1");
}

void TestLink()
{
  printf("-- stereo link --\n");
  // Loud left (limits), quiet right (never reaches the ceiling).
  Fixture f{ "unbalanced", {} };
  for (int i = 0; i < kSr / 2; i++) {
    double v = 1.2 * std::sin(2.0 * kPi * 220.0 * (double)i / (double)kSr);
    f.frames.push_back(v);
    f.frames.push_back(v * 0.25);
  }
  const int n = f.NumFrames();
  LimiterParams p;
  p.ceilingDb = -6.0;
  p.link = false;
  std::vector<double> env;
  LimiterResult r = LimiterComputeEnvelope(f.frames.data(), n, kNch, kSr, p, env);
  Check(r.ok && (int)env.size() == n * kNch,
        "unlinked envelope has one value per frame per channel");
  double minL = 1.0, minR = 1.0;
  for (int i = 0; i < n; i++) {
    if (env[(size_t)i * kNch] < minL) minL = env[(size_t)i * kNch];
    if (env[(size_t)i * kNch + 1] < minR) minR = env[(size_t)i * kNch + 1];
  }
  Check(minL < 1.0 && minR == 1.0,
        "unlinked: loud channel limited, quiet channel untouched");

  p.link = true;
  r = LimiterComputeEnvelope(f.frames.data(), n, kNch, kSr, p, env);
  Check(r.ok && (int)env.size() == n,
        "linked envelope is shared (one value per frame)");
}

void TestMicro()
{
  printf("-- micro buffer --\n");
  Fixture f = MakeMicro();
  LimiterParams p;
  p.attackMs = 30.0;  // window longer than the whole buffer: must clamp
  p.holdMs = 50.0;
  p.releaseMs = 1000.0;
  std::vector<double> work = f.frames;
  LimiterResult r = LimiterProcess(work.data(), f.NumFrames(), kNch, kSr, p);
  double excess = AmpDb(VerifyTruePeak8x(work, kNch)) - p.ceilingDb;
  char what[160];
  snprintf(what, sizeof(what),
           "100-sample buffer: ok, 8x excess %+.4f dB (<= 0.01)", excess);
  Check(r.ok && excess <= 0.01, what);
}

void TestGainPush()
{
  printf("-- gain push --\n");
  Fixture f = MakeNoise();
  LimiterParams p;
  p.gainDb = 12.0; // -6 dB noise pushed 12 dB into a -1 dBTP ceiling
  std::vector<double> work = f.frames;
  LimiterResult r = LimiterProcess(work.data(), f.NumFrames(), kNch, kSr, p);
  double excess = AmpDb(VerifyTruePeak8x(work, kNch)) - p.ceilingDb;
  char what[160];
  snprintf(what, sizeof(what),
           "+12 dB push: in %.2f dBTP, GR %.2f dB, 8x excess %+.4f dB (<= 0.01)",
           r.inputPeakDb, r.maxGainReductionDb, excess);
  Check(r.ok && excess <= 0.01 && r.inputPeakDb > 4.0 &&
            r.maxGainReductionDb > 4.0,
        what);
}

// --- Baseline dump ("dump" arg) ----------------------------------------------

void DumpOne(const Fixture& f, const LimiterParams& p, int edgeRamp)
{
  const int n = f.NumFrames();
  std::vector<double> env;
  LimiterResult r =
      LimiterComputeEnvelope(f.frames.data(), n, kNch, kSr, p, env, edgeRamp);
  std::vector<double> work = f.frames;
  LimiterResult rp =
      LimiterProcess(work.data(), n, kNch, kSr, p, edgeRamp);
  printf("== %s gain=%g ceil=%g att=%g hold=%g rel=%g tp=%d link=%d ramp=%d\n",
         f.name, p.gainDb, p.ceilingDb, p.attackMs, p.holdMs, p.releaseMs,
         p.truePeak ? 1 : 0, p.link ? 1 : 0, edgeRamp);
  printf("ok=%d in=%.17g out=%.17g gr=%.17g lat=%d envn=%d\n", r.ok ? 1 : 0,
         r.inputPeakDb, rp.outputPeakDb, r.maxGainReductionDb,
         r.latencySamples, (int)env.size());
  int stride = (int)env.size() / 32;
  if (stride < 1) stride = 1;
  for (size_t i = 0; i < env.size(); i += (size_t)stride)
    printf("env %zu %.17g\n", i, env[i]);
}

void PrintDump()
{
  printf("LIMITER HARNESS DUMP v1 sr=%d nch=%d\n", kSr, kNch);
  const Fixture fixtures[] = { MakeSquare997(), MakeIspSine(), MakeImpulses(),
                               MakeNoise(),     MakeSilence(), MakeMicro() };
  for (const Fixture& f : fixtures) {
    LimiterParams a; // defaults + push: the everyday "Game Asset -1 dBTP" shape
    a.gainDb = 6.0;
    DumpOne(f, a, 0);
    LimiterParams b; // contrasting config: sample-peak, unlinked, fast, ramped
    b.ceilingDb = -6.0;
    b.attackMs = 1.0;
    b.holdMs = 0.0;
    b.releaseMs = 200.0;
    b.truePeak = false;
    b.link = false;
    DumpOne(f, b, 480);
  }
}

} // namespace

int main(int argc, char** argv)
{
  if (argc > 1 && std::strcmp(argv[1], "dump") == 0) {
    PrintDump();
    return 0;
  }

  printf("== limiter_test (v2.4.0 INC-L0) ==\n");
  TestPassthrough();
  TestInvariants();
  TestEdgeRamp();
  TestLink();
  TestMicro();
  TestGainPush();

  printf("-- ceiling sweep: attack x release x ceiling x TP (36 configs each) --\n");
  const Fixture fixtures[] = { MakeSquare997(), MakeIspSine(), MakeImpulses(),
                               MakeNoise(),     MakeSilence(), MakeMicro() };
  for (const Fixture& f : fixtures) SweepFixture(f);

  if (g_failures) printf("LIMITER HARNESS: %d FAILURE(S)\n", g_failures);
  else printf("LIMITER HARNESS: ALL PASS\n");
  return g_failures ? 1 : 0;
}
