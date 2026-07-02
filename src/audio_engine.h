// audio_engine.h — WAV file I/O and REAPER source refresh for SneakPeak
#pragma once

#include "platform.h"
#include "globals.h"
#include <vector>
#include <string>
#include <cstdint>

struct WavInfo {
  int numChannels = 0;
  int sampleRate = 0;
  int bitsPerSample = 0;
  int numFrames = 0;
  int audioFormat = 0; // 1=PCM, 3=IEEE float
};

class AudioEngine {
public:
  // Get the source file path for a take
  static std::string GetSourceFilePath(MediaItem_Take* take);

  // Read WAV header info (does not read audio data)
  static bool ReadWavHeader(const std::string& path, WavInfo& info);

  // Read WAV file into double samples (interleaved). Fills info + samples vector.
  static bool ReadWavFile(const std::string& path, WavInfo& info,
                          std::vector<double>& samples);

  // Read any audio file via REAPER's PCM_Source (supports WAV, MP3, FLAC, OGG, AIFF, etc.)
  // Falls back to ReadWavFile if REAPER API is unavailable.
  static bool ReadAudioFile(const std::string& path, WavInfo& info,
                            std::vector<double>& samples);

  // Incremental streaming load (STA-1): the same decode as ReadAudioFile but
  // sliced - BeginStream opens + preallocates, ReadStreamStep decodes for
  // ~budgetSec of wall time per call (driven from the window timer, so the UI
  // stays responsive and the REAPER API is never touched off-thread), and the
  // caller installs `samples` when the step returns false. AbortStream cancels.
  struct StreamLoad {
    PCM_source* src = nullptr;
    WavInfo info;
    std::vector<double> samples; // preallocated by BeginStream, filled by steps
    int framesRead = 0;
    int totalFrames = 0;
    std::string path;
  };
  // False when PCM_Source is unavailable or the file is unreadable - the
  // caller falls back to the synchronous ReadAudioFile path.
  static bool BeginStream(const std::string& path, StreamLoad& s);
  // True while more audio remains; false = finished (info.numFrames trimmed
  // to what actually decoded, source closed).
  static bool ReadStreamStep(StreamLoad& s, double budgetSec);
  static void AbortStream(StreamLoad& s);

  // Write audio data to WAV file (atomic: writes to .tmp then renames)
  // samples are interleaved doubles, will be converted to original format
  // loopStartFrame/loopEndFrame (END-EXCLUSIVE): when both valid, a `smpl`
  // sustain-loop chunk is appended after the data chunk (v2.4 INC-A4) so game
  // engines/samplers read the loop natively. -1/-1 = no chunk (default).
  static bool WriteWavFile(const std::string& path, const double* samples,
                           int numFrames, int numChannels, int sampleRate,
                           int bitsPerSample, int audioFormat,
                           int loopStartFrame = -1, int loopEndFrame = -1);

  // Refresh REAPER's source after modifying the file on disk
  static void RefreshItemSource(MediaItem* item, MediaItem_Take* take);

  // Write audio to WAV file for drag export.
  // Priority: 1) project recording folder, 2) next to sourceFile, 3) /tmp
  // Returns path or empty on failure.
  static std::string WriteExportWav(const double* samples, int numFrames,
                                     int numChannels, int sampleRate,
                                     int bitsPerSample = 16, int audioFormat = 1,
                                     const char* sourceFilePath = nullptr);
};
