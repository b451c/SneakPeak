// dyn_params_test.cpp - DynamicsParams string round trip + range clamp (audit A7.5).
//
//   tests/run_dyn_params_test.sh    build + run (machine-independent, exit code)
//
// 1. ToString -> FromString -> ToString is the identity for the defaults, for
//    every sentinel (Thresh operating point, G.Thr off, D.Thr Auto) and for a
//    string with every field at its range edge.
// 2. Garbage values keep the defaults ("t=abc", "a=nan"); out-of-range values
//    are clamped to the knob range; sentinels survive.
#include "dynamics_engine.h"
#include "dynamics_ranges.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;

static void Check(bool cond, const char* what)
{
  printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
  if (!cond) g_failures++;
}

static std::string Str(const DynamicsParams& p)
{
  char buf[1024];
  DynamicsParamsToString(p, buf, sizeof(buf));
  return buf;
}

static bool RoundTrips(const DynamicsParams& p, const char* what)
{
  DynamicsParams q;
  const std::string a = Str(p);
  const bool ok = DynamicsParamsFromString(a.c_str(), q) && Str(q) == a;
  if (!ok) printf("  round trip broke:\n    %s\n    %s\n", a.c_str(), Str(q).c_str());
  Check(ok, what);
  return ok;
}

int main()
{
  DynamicsParams d;
  RoundTrips(d, "defaults round-trip");

  DynamicsParams s = d;
  s.threshold = -100.0; s.gateThreshDb = -100.0; s.dsThreshDb = -100.0;
  RoundTrips(s, "sentinels round-trip (Thresh operating point, G.Thr off, D.Thr Auto)");

  DynamicsParams lo = d, hi = d;
  lo.threshold = kDynParamRanges[0].lo;  hi.threshold = kDynParamRanges[0].hi;
  lo.ratio = kDynParamRanges[1].lo;      hi.ratio = kDynParamRanges[1].hi;
  lo.kneeDb = kDynParamRanges[2].lo;     hi.kneeDb = kDynParamRanges[2].hi;
  lo.attackMs = kDynParamRanges[3].lo;   hi.attackMs = kDynParamRanges[3].hi;
  lo.releaseMs = kDynParamRanges[4].lo;  hi.releaseMs = kDynParamRanges[4].hi;
  lo.makeupDb = kDynParamRanges[5].lo;   hi.makeupDb = kDynParamRanges[5].hi;
  lo.lookaheadMs = kDynParamRanges[6].lo; hi.lookaheadMs = kDynParamRanges[6].hi;
  lo.gateThreshDb = kDynParamRanges[7].lo; hi.gateThreshDb = kDynParamRanges[7].hi;
  lo.gateRangeDb = kDynParamRanges[8].lo; hi.gateRangeDb = kDynParamRanges[8].hi;
  lo.gateHoldMs = kDynParamRanges[9].lo; hi.gateHoldMs = kDynParamRanges[9].hi;
  lo.gateRatio = kDynParamRanges[10].lo; hi.gateRatio = kDynParamRanges[10].hi;
  lo.gateHystDb = kDynParamRanges[11].lo; hi.gateHystDb = kDynParamRanges[11].hi;
  lo.gateAttackMs = kDynParamRanges[12].lo; hi.gateAttackMs = kDynParamRanges[12].hi;
  lo.gateReleaseMs = kDynParamRanges[13].lo; hi.gateReleaseMs = kDynParamRanges[13].hi;
  lo.maxBoostDb = kDynParamRanges[14].lo; hi.maxBoostDb = kDynParamRanges[14].hi;
  lo.dsFreqHz = kDynParamRanges[15].lo;  hi.dsFreqHz = kDynParamRanges[15].hi;
  lo.dsQ = kDynParamRanges[16].lo;       hi.dsQ = kDynParamRanges[16].hi;
  lo.dsThreshDb = kDynParamRanges[17].lo; hi.dsThreshDb = kDynParamRanges[17].hi;
  lo.dsRatio = kDynParamRanges[18].lo;   hi.dsRatio = kDynParamRanges[18].hi;
  lo.dsRangeDb = kDynParamRanges[19].lo; hi.dsRangeDb = kDynParamRanges[19].hi;
  lo.dsAttackMs = kDynParamRanges[20].lo; hi.dsAttackMs = kDynParamRanges[20].hi;
  lo.dsReleaseMs = kDynParamRanges[21].lo; hi.dsReleaseMs = kDynParamRanges[21].hi;
  hi.dsSplit = true;
  RoundTrips(lo, "every field at its range minimum round-trips");
  RoundTrips(hi, "every field at its range maximum round-trips");

  // Garbage keeps the defaults; out-of-range clamps; sentinels survive.
  DynamicsParams g;
  DynamicsParamsFromString("t=abc r=nan k=-5 a=99999 re=-1 m=1e9 la=inf gt=-100 gr=5 gh=-3 "
                           "gx=0 ghy=7 gat=-2 gre=1 up=7 mb=-4 dse=1 dsm=3 dsf=10 dsq=100 "
                           "dst=-100 dsr=0 dsx=3 dsa=0 dsre=9999 dsb=1", g);
  Check(g.threshold == d.threshold, "t=abc keeps the default threshold");
  Check(g.ratio == d.ratio, "r=nan keeps the default ratio");
  Check(g.kneeDb == kDynParamRanges[2].lo, "k=-5 clamps to the knee minimum");
  Check(g.attackMs == kDynParamRanges[3].hi, "a=99999 clamps to the attack maximum");
  Check(g.releaseMs == kDynParamRanges[4].lo, "re=-1 clamps to the release minimum");
  Check(g.makeupDb == kDynParamRanges[5].hi, "m=1e9 clamps to the makeup maximum");
  Check(g.lookaheadMs == d.lookaheadMs, "la=inf keeps the default lookahead");
  Check(g.gateThreshDb == -100.0, "gt=-100 stays the off sentinel");
  Check(g.gateRangeDb == kDynParamRanges[8].hi, "gr=5 clamps to the gate range maximum (0)");
  Check(g.gateHoldMs == kDynParamRanges[9].lo, "gh=-3 clamps to the hold minimum");
  Check(g.gateRatio == kDynParamRanges[10].lo, "gx=0 clamps to the gate ratio minimum (1)");
  Check(g.gateHystDb == kDynParamRanges[11].hi, "ghy=7 clamps to the hysteresis maximum (0)");
  Check(g.gateAttackMs == kDynParamRanges[12].lo, "gat=-2 clamps to the gate attack minimum");
  Check(g.gateReleaseMs == kDynParamRanges[13].lo, "gre=1 clamps to the gate release minimum (10)");
  Check(g.compMode == 0, "up=7 falls back to Down");
  Check(g.maxBoostDb == kDynParamRanges[14].lo, "mb=-4 clamps to the boost minimum");
  Check(g.dsEnable, "dse=1 enables the de-esser");
  Check(g.dsFreqHz == kDynParamRanges[15].lo, "dsf=10 clamps to the de-ess frequency minimum");
  Check(g.dsQ == kDynParamRanges[16].hi, "dsq=100 clamps to the width maximum");
  Check(g.dsThreshDb == -100.0, "dst=-100 stays the Auto sentinel");
  Check(g.dsRatio == kDynParamRanges[18].lo, "dsr=0 clamps to the de-ess ratio minimum (1)");
  Check(g.dsRangeDb == kDynParamRanges[19].hi, "dsx=3 clamps to the de-ess range maximum (0)");
  Check(g.dsAttackMs == kDynParamRanges[20].lo, "dsa=0 clamps to the de-ess attack minimum");
  Check(g.dsReleaseMs == kDynParamRanges[21].hi, "dsre=9999 clamps to the de-ess release maximum");
  Check(g.dsSplit, "dsb=1 turns split-band on");

  // A near-sentinel value below the knob range is the sentinel, not the minimum.
  DynamicsParams n;
  DynamicsParamsFromString("t=-99.5 gt=-99.0 dst=-150", n);
  Check(n.threshold == -100.0 && n.gateThreshDb == -100.0 && n.dsThreshDb == -100.0,
        "values at or below -99 become the sentinel");

  printf("\n%s (%d failure%s)\n", g_failures ? "DYN PARAMS TEST: RED" : "DYN PARAMS TEST: GREEN",
         g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
