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

  // Write audio data to WAV file (atomic: writes to .tmp then renames)
  // samples are interleaved doubles, will be converted to original format
  static bool WriteWavFile(const std::string& path, const double* samples,
                           int numFrames, int numChannels, int sampleRate,
                           int bitsPerSample, int audioFormat);

  // Refresh REAPER's source after modifying the file on disk
  static void RefreshItemSource(MediaItem* item, MediaItem_Take* take);

  // Write selection to a temp WAV file, returns path or empty on failure
  static std::string WriteTempWav(const double* samples, int numFrames,
                                   int numChannels, int sampleRate,
                                   int bitsPerSample = 16, int audioFormat = 1);
};
