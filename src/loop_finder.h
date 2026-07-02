// loop_finder.h — automatic loop-point finder (v2.4 INC-A2)
// Pure computation — no REAPER API calls, no GDI (same contract as the other
// DSP modules). Textbook math per the v2.4 plan: candidate points are RISING
// zero crossings; a pair scores by normalized cross-correlation of the ~30 ms
// windows BEFORE the end point vs BEFORE the start point (after the wrap, the
// tail of the loop continues into its head, so those two histories must
// match), with an FFT-magnitude L2 distance as a timbre tie-break. Behavior
// references (no code ported): PyMusicLooper, LoopFinder, music-dsp threads.
#pragma once

#include <vector>

struct LoopCandidate {
  int startFrame = 0;
  int endFrame = 0;     // end-exclusive (first frame NOT played)
  double score = 0.0;   // higher = better (mode-relative, roughly (0.5, 1])
  bool texture = false; // scored by the texture fallback: the seam has
                        // PERCEPTUAL continuity but no waveform continuity -
                        // weld it after picking
};

// Find up to maxCandidates loop-point pairs in the buffer (interleaved
// doubles). Loop length is constrained to [max(1 s, 10% of file),
// file - 100 ms]; candidates come back sorted by score, de-duplicated
// (> 250 ms apart). Two scoring tiers:
//  1. Waveform NCC (tonal material - hums, tones, music): sample-accurate
//     seams, the 0.5 floor from the plan.
//  2. TEXTURE fallback when tier 1 finds nothing (stochastic ambiences -
//     birds, rain, wind - never correlate at the sample level): spectral
//     similarity + level match, penalized for cutting mid-transient.
//     Flagged texture = true; the seam wants a Weld crossfade.
// Empty result = file too short, silent, or nothing above either floor.
std::vector<LoopCandidate> FindLoopCandidates(const double* audio,
                                              int numFrames, int numChannels,
                                              int sampleRate,
                                              int maxCandidates = 5);

// Loop Weld (INC-A3): equal-power crossfade of the seam. The last
// crossfadeFrames frames before endFrame blend from the original tail into
// the material that PRECEDES startFrame, so the wrap end->start becomes
// continuous by construction:
//   buf[end-L+i] = buf[end-L+i]*cos(t*pi/2) + buf[start-L+i]*sin(t*pi/2),
//   t = (i+1)/(L+1)
// Length-preserving; touches ONLY [endFrame-L, endFrame). Requires
// startFrame >= L (there must be L frames before the start to weld with)
// and L <= endFrame-startFrame. Returns false on invalid args (buffer
// untouched).
bool WeldLoopSeam(double* audio, int numFrames, int numChannels,
                  int startFrame, int endFrame, int crossfadeFrames);
