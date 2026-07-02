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
  int endFrame = 0;    // end-exclusive (first frame NOT played)
  double score = 0.0;  // NCC - 0.25 * spectral distance; higher = better
};

// Find up to maxCandidates loop-point pairs in the buffer (interleaved
// doubles). Loop length is constrained to [max(1 s, 10% of file),
// file - 100 ms]; candidates come back sorted by score, de-duplicated
// (> 250 ms apart), scores in roughly (0.5, 1]. Empty result = file too
// short or nothing scored above the 0.5 NCC floor.
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
