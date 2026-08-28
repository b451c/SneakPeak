// deess_engine.cpp — De-esser sidechain filter (v2.3.0 INC-3)
// RBJ biquad filters (W3C Audio EQ Cookbook, reimplemented from the spec).
#include "deess_engine.h"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// RBJ cookbook, band-pass (constant 0 dB peak gain):
//   b0 = alpha, b1 = 0, b2 = -alpha; a0 = 1+alpha, a1 = -2cos(w0), a2 = 1-alpha
void DeEssBiquad::SetBandpass(double fs, double f0, double q)
{
  double w0 = 2.0 * M_PI * f0 / fs;
  double alpha = std::sin(w0) / (2.0 * q);
  double cw = std::cos(w0);
  double a0 = 1.0 + alpha;
  b0 = alpha / a0;
  b1 = 0.0;
  b2 = -alpha / a0;
  a1 = -2.0 * cw / a0;
  a2 = (1.0 - alpha) / a0;
}

// RBJ cookbook, high-pass:
//   b0 = (1+cos w0)/2, b1 = -(1+cos w0), b2 = (1+cos w0)/2
//   a0 = 1+alpha, a1 = -2cos(w0), a2 = 1-alpha
void DeEssBiquad::SetHighpass(double fs, double f0, double q)
{
  double w0 = 2.0 * M_PI * f0 / fs;
  double alpha = std::sin(w0) / (2.0 * q);
  double cw = std::cos(w0);
  double a0 = 1.0 + alpha;
  b0 = (1.0 + cw) / (2.0 * a0);
  b1 = -(1.0 + cw) / a0;
  b2 = (1.0 + cw) / (2.0 * a0);
  a1 = -2.0 * cw / a0;
  a2 = (1.0 - alpha) / a0;
}
