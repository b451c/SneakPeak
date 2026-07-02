// wav_smpl.h — WAV `smpl` chunk build/parse (v2.4 INC-A4)
// Pure computation + stdio (no REAPER deps). One forward sustain loop,
// infinite play count - the convention game engines (Unity/FMOD/Wwise) and
// samplers (sfizz & friends) read natively. Public RIFF spec; dwEnd is
// INCLUSIVE on disk (the Wavosaur/sampler consensus), so these APIs take and
// return an END-EXCLUSIVE frame and convert at the byte boundary.
#pragma once

#include <cstddef>

constexpr int kSmplChunkBytes = 68;   // "smpl" + u32 size + 60-byte payload

// Fill out[68] with a complete chunk: Manufacturer/Product 0, SamplePeriod =
// round(1e9/sampleRate) ns, MIDIUnityNote 60, one loop record (CuePointID 0,
// Type 0 = forward, Fraction 0, PlayCount 0 = infinite).
void BuildSmplChunk(int sampleRate, int loopStartFrame, int loopEndFrameExcl,
                    unsigned char out[kSmplChunkBytes]);

// Scan a WAV file for the first `smpl` sustain loop. Returns false when the
// file is not RIFF/WAVE, has no smpl chunk, or declares zero loops. End comes
// back EXCLUSIVE (disk value + 1); the caller clamps to the file length.
bool ParseWavSmplFile(const char* path, int* loopStartFrame,
                      int* loopEndFrameExcl);
