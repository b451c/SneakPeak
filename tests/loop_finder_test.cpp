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
