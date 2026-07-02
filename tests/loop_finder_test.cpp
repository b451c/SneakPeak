// loop_finder_test.cpp — offline correctness check for INC-A2 (v2.4.0)
//
// Machine-independent assertions (exit 0 = all PASS), spectral_repair_test
// style: a strictly periodic signal must yield a top candidate whose length
// is a whole number of periods with a near-perfect score; white noise must
// yield nothing above the 0.5 NCC floor. Deterministic (fixed-seed LCG).

#include "loop_finder.h"
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

struct Lcg {
  uint64_t state = 0x5EEDCAFEF00D1234ULL;
  double Next() {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return ((double)(state >> 11) / (double)(1ULL << 53)) * 2.0 - 1.0;
  }
};

}  // namespace

int main()
{
  printf("== loop_finder_test (v2.4.0 INC-A2) ==\n");

  // Periodic fixture: 220 Hz carrier with a 4 Hz tremolo - exactly periodic
  // with period 0.25 s (55 and 1 whole cycles). ANY loop spanning a whole
  // number of periods is seamless, so the finder must land on one.
  {
    const double period = 0.25;
    const int n = kSr * 10;
    std::vector<double> buf((size_t)n * kNch);
    for (int i = 0; i < n; i++) {
      const double t = (double)i / kSr;
      const double v =
          std::sin(2.0 * kPi * 220.0 * t) * (0.6 + 0.4 * std::sin(2.0 * kPi * 4.0 * t));
      buf[(size_t)i * kNch] = v;
      buf[(size_t)i * kNch + 1] = 0.5 * v;
    }
    std::vector<LoopCandidate> found =
        FindLoopCandidates(buf.data(), n, kNch, kSr, 5);
    Check(!found.empty(), "periodic signal yields candidates");
    if (!found.empty()) {
      const LoopCandidate& c = found[0];
      char what[160];
      snprintf(what, sizeof(what), "top candidate scores > 0.9 (got %.3f)",
               c.score);
      Check(c.score > 0.9, what);
      const double lenSec = (double)(c.endFrame - c.startFrame) / kSr;
      const double periods = lenSec / period;
      const double frac = std::fabs(periods - std::floor(periods + 0.5));
      snprintf(what, sizeof(what),
               "loop length %.4f s is a whole number of periods (err %.4f, <= 0.008)",
               lenSec, frac * period);
      Check(frac * period <= 0.008, what);   // within 8 ms of a period multiple
      Check(c.endFrame > c.startFrame && c.startFrame >= 0 && c.endFrame <= n,
            "candidate frames in range, ordered");
    }
  }

  // Noise fixture: no repeating structure - the 0.5 NCC floor must hold
  // (30 ms windows of independent noise correlate at ~0.1 worst-case).
  {
    const int n = kSr * 5;
    Lcg lcg;
    std::vector<double> buf((size_t)n * kNch);
    for (int i = 0; i < n; i++) {
      const double v = 0.5 * lcg.Next();
      buf[(size_t)i * kNch] = v;
      buf[(size_t)i * kNch + 1] = 0.5 * v;
    }
    std::vector<LoopCandidate> found =
        FindLoopCandidates(buf.data(), n, kNch, kSr, 5);
    char what[96];
    snprintf(what, sizeof(what), "white noise yields no candidates (got %d)",
             (int)found.size());
    Check(found.empty(), what);
  }

  // Loop Weld (INC-A3): equal-power seam crossfade.
  {
    const int n = kSr * 3;
    std::vector<double> buf((size_t)n * kNch);
    Lcg lcg;
    for (int i = 0; i < n; i++) {
      const double v = 0.4 * lcg.Next();   // structureless: a hard seam for sure
      buf[(size_t)i * kNch] = v;
      buf[(size_t)i * kNch + 1] = 0.5 * v;
    }
    const int s = kSr / 2, e = kSr * 2, L = kSr / 10;   // 0.5 s .. 2.0 s, 100 ms
    std::vector<double> orig = buf;
    Check(WeldLoopSeam(buf.data(), n, kNch, s, e, L), "weld accepts valid args");

    bool outsideIdentical = true;
    for (int i = 0; i < n && outsideIdentical; i++) {
      if (i >= e - L && i < e) continue;   // the welded range
      for (int c = 0; c < kNch; c++)
        if (buf[(size_t)i * kNch + (size_t)c] != orig[(size_t)i * kNch + (size_t)c]) {
          outsideIdentical = false;
          break;
        }
    }
    Check(outsideIdentical, "weld touches ONLY [end-L, end)");

    // Boundary continuity: the last welded frame is ~the frame before the
    // start (gTail = cos(pi/2 * L/(L+1)) is a <1% residual at L=4800), so the
    // wrap end->start continues exactly like start-1 -> start does.
    double worst = 0.0;
    for (int c = 0; c < kNch; c++) {
      const double welded = buf[(size_t)(e - 1) * kNch + (size_t)c];
      const double target = orig[(size_t)(s - 1) * kNch + (size_t)c];
      worst = std::max(worst, std::fabs(welded - target));
    }
    Check(worst < 0.02, "welded tail lands on the pre-start material (seam continuous)");

    // Equal-power midpoint: at t = 0.5 both gains are cos(pi/4) = sqrt(0.5).
    {
      const int i = (L - 1) / 2;   // t = (i+1)/(L+1) = 0.5 for odd L; ~0.5 here
      const double t = (double)(i + 1) / (double)(L + 1);
      const double expect = orig[(size_t)(e - L + i) * kNch] * std::cos(t * kPi / 2.0) +
                            orig[(size_t)(s - L + i) * kNch] * std::sin(t * kPi / 2.0);
      Check(std::fabs(buf[(size_t)(e - L + i) * kNch] - expect) < 1e-12,
            "equal-power law holds mid-crossfade");
    }

    // Guard: not enough material before the start -> refused, buffer untouched.
    std::vector<double> buf2 = orig;
    Check(!WeldLoopSeam(buf2.data(), n, kNch, L - 1, e, L) && buf2 == orig,
          "weld refuses start < L and leaves the buffer untouched");
  }

  // Degenerate inputs never crash or return garbage.
  {
    std::vector<double> tiny((size_t)100 * kNch, 0.1);
    Check(FindLoopCandidates(tiny.data(), 100, kNch, kSr, 5).empty(),
          "too-short buffer returns empty");
    Check(FindLoopCandidates(nullptr, 0, 0, 0, 5).empty(),
          "null/zero args return empty");
  }

  if (g_failures) printf("LOOP FINDER: %d FAILURE(S)\n", g_failures);
  else printf("LOOP FINDER: ALL PASS\n");
  return g_failures ? 1 : 0;
}
