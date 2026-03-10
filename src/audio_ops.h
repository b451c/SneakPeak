// audio_ops.h — Sample-level audio processing operations for SneakPeak
#pragma once

#include <cstddef>

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
