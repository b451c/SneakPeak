// audio_ops.h — Sample-level audio processing operations for SneakPeak
#pragma once

#include <cstddef>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Shared fade shape curve (used by waveform_view, audio_ops, edit_view)
inline double ApplyFadeShape(double t, int shape)
{
  t = std::max(0.0, std::min(1.0, t));
  switch (shape) {
    default:
    case 0: return t;                                       // linear
    case 1: return sqrt(t);                                 // fast start
    case 2: return t * t;                                   // slow start
    case 3: return pow(t, 0.25);                            // fast start steep
    case 4: return t * t * t * t;                           // slow start steep
    case 5: return 0.5 - 0.5 * cos(M_PI * t);              // S-curve
    case 6: { double s = 0.5 - 0.5 * cos(M_PI * t);       // S-curve steep
              return s * s * (3.0 - 2.0 * s); }
  }
}

namespace AudioOps {

// Normalize to target peak (default 1.0 = 0dB)
void Normalize(double* samples, int numFrames, int numChannels, double targetPeak = 1.0);

// Linear fade in over the given samples
void FadeIn(double* samples, int numFrames, int numChannels);

// Linear fade out over the given samples
void FadeOut(double* samples, int numFrames, int numChannels);

// Shaped fade in (shape: 0=linear, 1=fast start, 2=slow start, 3=fast steep, 4=slow steep, 5=S-curve, 6=S-curve steep)
void FadeInShaped(double* samples, int numFrames, int numChannels, int shape);

// Shaped fade out (same shape codes)
void FadeOutShaped(double* samples, int numFrames, int numChannels, int shape);

// Reverse samples in place
void Reverse(double* samples, int numFrames, int numChannels);

// Apply gain multiplier
void Gain(double* samples, int numFrames, int numChannels, double gainFactor);

// Apply gain with cosine crossfade at boundaries (fadeFrames on each edge)
// samples points to start of region, gainFactor is the target gain
// Crossfade smoothly transitions from 1.0 to gainFactor at start, gainFactor to 1.0 at end
void GainWithCrossfade(double* samples, int numFrames, int numChannels,
                       double gainFactor, int fadeFrames);

// Remove DC offset (subtract mean)
void DCOffsetRemove(double* samples, int numFrames, int numChannels);

// Silence (zero out)
void Silence(double* samples, int numFrames, int numChannels);

} // namespace AudioOps
