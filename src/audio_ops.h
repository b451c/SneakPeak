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

// Reverse samples in place
void Reverse(double* samples, int numFrames, int numChannels);

// Apply gain multiplier
void Gain(double* samples, int numFrames, int numChannels, double gainFactor);

// Remove DC offset (subtract mean)
void DCOffsetRemove(double* samples, int numFrames, int numChannels);

// Silence (zero out)
void Silence(double* samples, int numFrames, int numChannels);

} // namespace AudioOps
