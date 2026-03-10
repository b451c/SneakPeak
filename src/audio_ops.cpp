// audio_ops.cpp — Sample-level audio processing
#include "audio_ops.h"
#include <cmath>
#include <algorithm>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace AudioOps {

void Normalize(double* samples, int numFrames, int numChannels, double targetPeak)
{
  if (!samples || numFrames <= 0 || numChannels <= 0) return;

  size_t total = (size_t)numFrames * (size_t)numChannels;

  // Find peak
  double peak = 0.0;
  for (size_t i = 0; i < total; i++) {
    double v = std::abs(samples[i]);
    if (v > peak) peak = v;
  }

  if (peak < 1e-10) return; // silence

  double scale = targetPeak / peak;
  for (size_t i = 0; i < total; i++) {
    samples[i] *= scale;
  }
}

void FadeIn(double* samples, int numFrames, int numChannels)
{
  if (!samples || numFrames <= 0 || numChannels <= 0) return;

  for (int f = 0; f < numFrames; f++) {
    double gain = (double)f / (double)numFrames;
    // Cosine curve for smoother fade
    gain = 0.5 * (1.0 - cos(gain * M_PI));
    for (int ch = 0; ch < numChannels; ch++) {
      samples[(size_t)f * numChannels + ch] *= gain;
    }
  }
}

void FadeOut(double* samples, int numFrames, int numChannels)
{
  if (!samples || numFrames <= 0 || numChannels <= 0) return;

  for (int f = 0; f < numFrames; f++) {
    double gain = 1.0 - (double)f / (double)numFrames;
    gain = 0.5 * (1.0 + cos((1.0 - gain) * M_PI));
    for (int ch = 0; ch < numChannels; ch++) {
      samples[(size_t)f * numChannels + ch] *= gain;
    }
  }
}

void Reverse(double* samples, int numFrames, int numChannels)
{
  if (!samples || numFrames <= 0 || numChannels <= 0) return;

  for (int i = 0; i < numFrames / 2; i++) {
    int j = numFrames - 1 - i;
    for (int ch = 0; ch < numChannels; ch++) {
      std::swap(samples[(size_t)i * numChannels + ch],
                samples[(size_t)j * numChannels + ch]);
    }
  }
}

void Gain(double* samples, int numFrames, int numChannels, double gainFactor)
{
  if (!samples || numFrames <= 0 || numChannels <= 0) return;

  size_t total = (size_t)numFrames * (size_t)numChannels;
  for (size_t i = 0; i < total; i++) {
    samples[i] *= gainFactor;
  }
}

void GainWithCrossfade(double* samples, int numFrames, int numChannels,
                       double gainFactor, int fadeFrames)
{
  if (!samples || numFrames <= 0 || numChannels <= 0) return;
  if (fadeFrames < 1) fadeFrames = 1;
  // Clamp fade to half the region so start/end fades don't overlap
  fadeFrames = std::min(fadeFrames, numFrames / 2);

  for (int f = 0; f < numFrames; f++) {
    double gain = gainFactor;
    if (f < fadeFrames) {
      // Crossfade in: smoothly from 1.0 to gainFactor
      double t = (double)f / (double)fadeFrames;
      double blend = 0.5 * (1.0 - cos(t * M_PI)); // 0→1 cosine
      gain = 1.0 + (gainFactor - 1.0) * blend;
    } else if (f >= numFrames - fadeFrames) {
      // Crossfade out: smoothly from gainFactor to 1.0
      double t = (double)(numFrames - 1 - f) / (double)fadeFrames;
      double blend = 0.5 * (1.0 - cos(t * M_PI)); // 0→1 cosine
      gain = 1.0 + (gainFactor - 1.0) * blend;
    }
    for (int ch = 0; ch < numChannels; ch++) {
      samples[(size_t)f * numChannels + ch] *= gain;
    }
  }
}

void DCOffsetRemove(double* samples, int numFrames, int numChannels)
{
  if (!samples || numFrames <= 0 || numChannels <= 0) return;

  // Calculate mean per channel
  for (int ch = 0; ch < numChannels; ch++) {
    double sum = 0.0;
    for (int f = 0; f < numFrames; f++) {
      sum += samples[(size_t)f * numChannels + ch];
    }
    double mean = sum / (double)numFrames;
    for (int f = 0; f < numFrames; f++) {
      samples[(size_t)f * numChannels + ch] -= mean;
    }
  }
}

void Silence(double* samples, int numFrames, int numChannels)
{
  if (!samples || numFrames <= 0 || numChannels <= 0) return;
  memset(samples, 0, (size_t)numFrames * (size_t)numChannels * sizeof(double));
}

} // namespace AudioOps
