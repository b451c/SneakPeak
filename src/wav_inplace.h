// wav_inplace.h — sample edits applied IN PLACE to a WAV file (v2.5, F6/F12)
// Destructive ITEM edits used to write the working buffer back as the whole
// file - but that buffer is DOWNSAMPLED on long items (the 10M-frame load
// cap, F6) and covers only the item's window of the source (a trimmed item
// truncated the file, F12). These ops stream through the file itself instead:
// same inode (REAPER's pooled decoders see the edit at once - F7), every other
// chunk untouched, O(chunk) memory. Ranges are [startFrame, endFrame) in FILE
// frames and are clamped to the data chunk. PCM 16/24-bit and 32-bit float,
// the formats the rest of SneakPeak reads and writes. Pure stdio, no REAPER deps.
#pragma once

#include <cstdint>
#include <string>

namespace WavInplace {

// Optional per-chunk hook (F5: the ops run on a worker): frac = fraction of
// the work done; return false to stop - the op then returns false with the
// chunks so far written, which the caller rolls back from its snapshot.
struct Progress {
  void* user = nullptr;
  bool (*fn)(void* user, double frac) = nullptr;
};

// Reverse the frame order of the range.
bool Reverse(const std::string& path, int64_t startFrame, int64_t endFrame,
             const Progress* prog = nullptr);

// Multiply the range by factor. fadeFrames > 0 ramps the gain linearly from 1
// to factor over the first fadeFrames and back to 1 over the last (the edge
// blend the buffer path applies to a partial selection); it is clamped to
// half the range and ignored below 2 frames, like that path.
bool Gain(const std::string& path, int64_t startFrame, int64_t endFrame,
          double factor, int fadeFrames, const Progress* prog = nullptr);

// Subtract the per-channel mean of the range (two passes).
bool DCRemove(const std::string& path, int64_t startFrame, int64_t endFrame,
              const Progress* prog = nullptr);

} // namespace WavInplace
