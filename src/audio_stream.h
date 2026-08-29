// audio_stream.h — chunked full-rate reads of a view's rendered audio (v2.5 8e)
// See .harness/design_streaming_source.md. Long items keep a DOWNSAMPLED working
// buffer (PlanRead's 10M-frame cap); anything that must write or analyse real
// samples reads them here instead, chunk by chunk, through take AudioAccessors:
// one per segment, all created and destroyed on the MAIN thread (SDK rule),
// Read() calling nothing but GetAudioAccessorSamples so it may run sliced on the
// main thread (exports) or on a worker (Dynamics, SWS Loudness precedent).
// Memory = one chunk. The accessor renders take offset, playrate and pitch; the
// I_CHANMODE fold and item volume are applied here with the loader's helpers so
// a stream equals the buffer the loader would have built at full rate.
#pragma once

#include "platform.h"
#include "globals.h"
#include <cstdint>
#include <mutex>
#include <vector>

struct AudioStreamSegment {
  MediaItem* item = nullptr;
  MediaItem_Take* take = nullptr;
  double takeStartSec = 0.0;      // accessor time of the segment's first frame
  int64_t dstFrame = 0;           // placement in the stream, at the stream rate
  int64_t frames = 0;
  int srcNch = 1;                 // channels requested (< read channels = duplicate)
  double volume = 1.0;            // multiplied per chunk (ItemTakeVolume) or 1.0
  AudioAccessor* accessor = nullptr;   // owned while open
};

// Loader-parity helpers (also used by StepItemAudioLoad / FinishItemAudioLoad).
double ItemTakeVolume(MediaItem* item, MediaItem_Take* take);   // D_VOL x take D_VOL
int TakeChanMode(MediaItem_Take* take);                          // I_CHANMODE, 0 if n/a
// Channels left after the I_CHANMODE fold: 1 for stereo in modes 2/3/4, else srcNch.
int FoldedChannels(int srcNch, int chanMode);
// Fold interleaved stereo in place: 2 = (L+R)/2, 3 = L, 4 = R. Result contiguous.
void FoldChanMode(double* frames, int64_t n, int chanMode);

class AudioStream {
public:
  ~AudioStream();
  // The one lock around every REAPER audio-accessor call (create / validate /
  // state-changed / destroy / read): the trace worker reads its accessor while
  // the main thread creates, polls and destroys others - never inside the API
  // at the same time (A8.1). Held per call, never across a chunk loop.
  static std::mutex& ApiLock();
  // MAIN THREAD. readNch = channels the buffer would carry before the fold;
  // outNch = FoldedChannels(readNch, chanMode). Creates one accessor per segment
  // (a failed create yields silence for that segment, like the loader).
  bool Open(std::vector<AudioStreamSegment> segs, int rate, int readNch,
            int chanMode, int64_t totalFrames);
  // ANY THREAD while open: the next `frames` frames into out[frames * Channels()],
  // zeros in gaps and past the end. False on an accessor error (stream stays
  // open; the consumer aborts).
  bool Read(double* out, int frames);
  // MAIN THREAD: any segment's underlying audio changed since Open.
  bool Changed() const;
  void Close();                    // MAIN THREAD
  bool IsOpen() const { return m_open; }
  int Rate() const { return m_rate; }
  int Channels() const { return m_outNch; }
  int64_t Frames() const { return m_total; }
  int64_t Remaining() const { return m_total - m_cursor; }

private:
  std::vector<AudioStreamSegment> m_segs;
  int m_rate = 0, m_readNch = 0, m_outNch = 0, m_chanMode = 0;
  int64_t m_total = 0, m_cursor = 0;
  bool m_open = false;
  std::vector<double> m_tmp;       // accessor scratch, one chunk
};
