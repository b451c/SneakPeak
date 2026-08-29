// deess_split_test.cpp — split-band de-ess apply (v2.5.0 row 15)
//
//   tests/run_deess_split_test.sh    build + run (machine-independent, exit code)
//
// DeEssApplySplit: y = (x - (1 - gBand) * b) * gWide, b = the input through the
// detector chain forward and backward (zero phase, |B|^2).
// Claims on a 220 Hz bed (0.25) with 100 ms 6 kHz bursts (0.5), stereo
// (right = 0.5 * left so the per-channel filter states differ):
//   1. gBand == 1 everywhere -> out == in * lerp(gWide), sample-exact
//      (0 * B(x) == 0), and the integer-grid lerp equals the time-based one
//      of the wideband Standalone apply to 1e-12;
//   2. BP 6 kHz Q 2 with gBand = -10 dB over the burst steps: the 6 kHz burst
//      core comes out within 0.5 dB of -10 dB, the 220 Hz bed inside the burst
//      within 0.2 dB of its input, and outside the bursts out == in exactly;
//   3. HP 3 kHz (Butterworth pair, the burst an octave above): the same, bed
//      within 0.05 dB;
//   4. DeEssSetupChain: layout (nch * nStages), and the f0 / Q clamps.
#include "deess_engine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int kSr = 44100, kNch = 2, kStep = 44;   // 1 ms grid at 44.1 kHz
constexpr double kPi = 3.14159265358979323846;
constexpr double kBedHz = 220.0, kBedAmp = 0.25, kSibHz = 6000.0, kSibAmp = 0.5;
constexpr double kBursts[2] = { 1.0, 2.0 };
constexpr double kBurstLen = 0.1;
constexpr int kFrames = 3 * kSr;

int g_failures = 0;

void Check(bool cond, const char* what)
{
  printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
  if (!cond) g_failures++;
}

bool InBurst(double t)
{
  for (double b : kBursts)
    if (t >= b && t < b + kBurstLen) return true;
  return false;
}

std::vector<double> BuildSignal()
{
  std::vector<double> s((size_t)kFrames * kNch);
  for (int i = 0; i < kFrames; i++) {
    const double t = (double)i / kSr;
    double l = kBedAmp * std::sin(2.0 * kPi * kBedHz * t);
    if (InBurst(t)) l += kSibAmp * std::sin(2.0 * kPi * kSibHz * t);
    s[(size_t)i * kNch] = l;
    s[(size_t)i * kNch + 1] = 0.5 * l;
  }
  return s;
}

// Least-squares amplitude of a sine at hz over [t0, t1) on channel ch.
double Amp(const std::vector<double>& s, int ch, double t0, double t1, double hz)
{
  const int a = (int)(t0 * kSr), b = (int)(t1 * kSr);
  double ss = 0, sc = 0, cc = 0, sy = 0, cy = 0;
  for (int i = a; i < b; i++) {
    const double t = (double)i / kSr, y = s[(size_t)i * kNch + ch];
    const double sn = std::sin(2.0 * kPi * hz * t), cs = std::cos(2.0 * kPi * hz * t);
    ss += sn * sn; sc += sn * cs; cc += cs * cs; sy += sn * y; cy += cs * y;
  }
  const double det = ss * cc - sc * sc;
  const double p = (sy * cc - cy * sc) / det, q = (cy * ss - sy * sc) / det;
  return std::sqrt(p * p + q * q);
}

double Db(double x, double ref) { return 20.0 * std::log10(std::max(x, 1e-12) / ref); }

size_t Steps() { return (size_t)(kFrames / kStep + 1); }

// gBand = -10 dB over the steps whose window starts 5 ms into a burst (past the
// 1 ms attack of the real engine - the shape of a real dsGR lane).
std::vector<double> BandGains()
{
  std::vector<double> g(Steps(), 1.0);
  for (size_t k = 0; k < g.size(); k++) {
    const double t = (double)(k * kStep) / kSr;
    for (double b : kBursts)
      if (t >= b + 0.005 && t < b + kBurstLen) g[k] = std::pow(10.0, -10.0 / 20.0);
  }
  return g;
}

void BandClaims(int mode, double f0, const char* name, double bedTolDb)
{
  const std::vector<double> in = BuildSignal();
  std::vector<double> out(in.size());
  const std::vector<double> gW(Steps(), 1.0), gB = BandGains();
  DeEssApplySplit(in.data(), out.data(), kFrames, kNch, kSr, mode, f0, 2.0, gW, gB, kStep);
  char what[160];
  for (int ch = 0; ch < kNch; ch++) {
    for (double b : kBursts) {
      const double sib = Db(Amp(out, ch, b + 0.03, b + 0.09, kSibHz), Amp(in, ch, b + 0.03, b + 0.09, kSibHz));
      const double bed = Db(Amp(out, ch, b + 0.03, b + 0.09, kBedHz), Amp(in, ch, b + 0.03, b + 0.09, kBedHz));
      snprintf(what, sizeof(what), "%s ch%d burst @%.0fs: 6 kHz %+.2f dB (want -10 +-0.5)", name, ch, b, sib);
      Check(std::fabs(sib + 10.0) < 0.5, what);
      snprintf(what, sizeof(what), "%s ch%d burst @%.0fs: 220 Hz bed %+.3f dB (want 0 +-%.2f)", name, ch, b, bed, bedTolDb);
      Check(std::fabs(bed) < bedTolDb, what);
    }
  }
  bool idleExact = true;
  for (int i = (int)(0.4 * kSr); i < (int)(0.9 * kSr); i++)
    for (int c = 0; c < kNch; c++)
      if (out[(size_t)i * kNch + c] != in[(size_t)i * kNch + c]) idleExact = false;
  snprintf(what, sizeof(what), "%s: out == in exactly where gBand == 1 (0.4-0.9 s)", name);
  Check(idleExact, what);
}

} // namespace

int main()
{
  // 1. transparency: gBand == 1 -> out == in * lerp(gWide) (varying wideband gain)
  {
    const std::vector<double> in = BuildSignal();
    std::vector<double> out(in.size());
    std::vector<double> gW(Steps()), gB(Steps(), 1.0);
    for (size_t k = 0; k < gW.size(); k++) gW[k] = 0.5 + 0.5 * std::fabs(std::sin((double)k * 0.37));
    DeEssApplySplit(in.data(), out.data(), kFrames, kNch, kSr, DEESS_MODE_BANDPASS, kSibHz, 2.0, gW, gB, kStep);
    bool exact = true;
    double maxTimeLerpDiff = 0.0;
    const size_t last = gW.size() - 1;
    for (int i = 0; i < kFrames; i++) {
      size_t k = (size_t)i / kStep; if (k > last) k = last;
      double g = gW[k];
      if (k < last) {
        const double a = (double)((size_t)i - k * kStep) / (double)kStep;   // the impl's expression
        g += (gW[k + 1] - g) * a;
      }
      // the wideband apply's time-based lerp (audio_commands.cpp)
      const double t = (double)i / kSr, tk = (double)(k * kStep) / kSr, tk1 = (double)((k + 1) * kStep) / kSr;
      double gt = gW[k];
      if (k < last) gt += (gW[k + 1] - gW[k]) * std::min(1.0, std::max(0.0, (t - tk) / (tk1 - tk)));
      maxTimeLerpDiff = std::max(maxTimeLerpDiff, std::fabs(g - gt));
      for (int c = 0; c < kNch; c++)
        if (out[(size_t)i * kNch + c] != in[(size_t)i * kNch + c] * g) exact = false;
    }
    Check(exact, "gBand == 1: out == in * lerp(gWide) sample-exact (BP chain running)");
    char what[120];
    snprintf(what, sizeof(what), "grid lerp == time-based lerp of the wideband apply (max diff %.2e)", maxTimeLerpDiff);
    Check(maxTimeLerpDiff < 1e-12, what);
  }

  // 2./3. the band is cut, the bed is not
  // BP centred ON the burst (|B|^2 = 1 at f0); HP an octave BELOW it (at f0
  // itself a Butterworth corner reads -3 dB, so the cut there is -3.6 dB by design).
  BandClaims(DEESS_MODE_BANDPASS, kSibHz, "BP 6 kHz Q2", 0.2);
  BandClaims(DEESS_MODE_HIGHPASS, 3000.0, "HP 3 kHz", 0.05);

  // 4. chain setup: layout + clamps
  {
    std::vector<DeEssBiquad> bp, hp, hi, ref, lo, loRef;
    Check(DeEssSetupChain(bp, 2, kSr, DEESS_MODE_BANDPASS, 6000.0, 2.0) == 1 && bp.size() == 2,
          "BP chain: 1 stage x 2 channels");
    Check(DeEssSetupChain(hp, 2, kSr, DEESS_MODE_HIGHPASS, 6000.0, 2.0) == 2 && hp.size() == 4,
          "HP chain: 2 stages x 2 channels");
    DeEssSetupChain(hi, 1, kSr, DEESS_MODE_BANDPASS, 30000.0, 2.0);
    DeEssSetupChain(ref, 1, kSr, DEESS_MODE_BANDPASS, 0.45 * kSr, 2.0);
    Check(hi[0].b0 == ref[0].b0 && hi[0].a1 == ref[0].a1 && hi[0].a2 == ref[0].a2,
          "f0 above 0.45 fs clamps to 0.45 fs");
    DeEssSetupChain(lo, 1, kSr, DEESS_MODE_BANDPASS, 6000.0, 0.01);
    DeEssSetupChain(loRef, 1, kSr, DEESS_MODE_BANDPASS, 6000.0, 0.1);
    Check(lo[0].b0 == loRef[0].b0 && lo[0].a2 == loRef[0].a2, "Q below 0.1 clamps to 0.1");
  }

  printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASS", g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
