// dynamics_ranges.h - the one source of the Dynamics knob ranges.
// DynamicsPanel::SLIDER_DEFS (the knobs) and ClampDynamicsParams (every
// parsed parameter string: item P_EXT, presets) read the same table, and
// tests/docs_knob_ranges_test.py keeps the guide's table equal to it (A7.5/A7.6).
#pragma once

constexpr int kDynParamCount = 22;
struct DynParamRange { double lo, hi; };
constexpr DynParamRange kDynParamRanges[kDynParamCount] = {
  { -60.0,     0.0 },  // 0  Thresh   (-100 = operating point sentinel)
  { -20.0,   100.0 },  // 1  Ratio    (extended encoding: >=1 classic, 0 = Inf, <0 = over-comp)
  {   0.0,    24.0 },  // 2  Knee
  {   0.0,   500.0 },  // 3  Attack
  {   0.0,  1000.0 },  // 4  Release
  { -24.0,    24.0 },  // 5  Makeup
  {   0.0,    20.0 },  // 6  L.ahead
  { -90.0,     0.0 },  // 7  G.Thr    (-100 = off sentinel)
  { -80.0,     0.0 },  // 8  G.Range
  {   0.0,   500.0 },  // 9  G.Hold
  {   1.0,    10.0 },  // 10 G.Ratio
  { -24.0,     0.0 },  // 11 G.Hyst
  {   0.0,    50.0 },  // 12 G.Att
  {  10.0,  1000.0 },  // 13 G.Rel
  {   0.0,    24.0 },  // 14 M.Boost
  { 2000.0, 16000.0 }, // 15 Freq
  {   0.5,     8.0 },  // 16 Width
  { -60.0,     0.0 },  // 17 D.Thr    (-100 = Auto sentinel: the band's average level)
  {   1.0,    20.0 },  // 18 D.Ratio
  { -24.0,     0.0 },  // 19 D.Range
  {   0.5,    10.0 },  // 20 D.Att
  {  20.0,   200.0 },  // 21 D.Rel
};
