// audio_engine.cpp — WAV file I/O and REAPER source management
#include "audio_engine.h"
#include "debug.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <algorithm>

// --- WAV format structures ---

#pragma pack(push, 1)
struct RiffHeader {
  char riffId[4];       // "RIFF"
  uint32_t fileSize;    // file size - 8
  char waveId[4];       // "WAVE"
};

struct FmtChunk {
  char fmtId[4];        // "fmt "
  uint32_t chunkSize;   // 16 for PCM
  uint16_t audioFormat; // 1=PCM, 3=IEEE float
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
};

struct DataChunkHeader {
  char dataId[4];       // "data"
  uint32_t dataSize;
};
#pragma pack(pop)

// --- Implementation ---

std::string AudioEngine::GetSourceFilePath(MediaItem_Take* take)
{
  if (!take || !g_GetMediaItemTake_Source || !g_GetMediaSourceFileName) return "";

  PCM_source* src = g_GetMediaItemTake_Source(take);
  if (!src) return "";

  char buf[2048] = {};
  g_GetMediaSourceFileName(src, buf, sizeof(buf));
  return std::string(buf);
}

bool AudioEngine::ReadWavHeader(const std::string& path, WavInfo& info)
{
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;

  RiffHeader riff;
  if (fread(&riff, sizeof(riff), 1, f) != 1 ||
      memcmp(riff.riffId, "RIFF", 4) != 0 ||
      memcmp(riff.waveId, "WAVE", 4) != 0) {
    fclose(f);
    return false;
  }

  // Find fmt chunk
  FmtChunk fmt = {};
  bool foundFmt = false;
  bool foundData = false;
  uint32_t dataSize = 0;

  while (!feof(f)) {
    char chunkId[4];
    uint32_t chunkSize;
    if (fread(chunkId, 4, 1, f) != 1) break;
    if (fread(&chunkSize, 4, 1, f) != 1) break;

    if (memcmp(chunkId, "fmt ", 4) == 0) {
      // Read fmt data
      fseek(f, -8, SEEK_CUR);
      if (fread(&fmt, sizeof(fmt), 1, f) != 1) break;
      // Skip any extra fmt bytes
      if (fmt.chunkSize > 16) {
        fseek(f, (long)(fmt.chunkSize - 16), SEEK_CUR);
      }
      foundFmt = true;
    } else if (memcmp(chunkId, "data", 4) == 0) {
      dataSize = chunkSize;
      foundData = true;
      break;
    } else {
      // Skip unknown chunk
      fseek(f, (long)chunkSize, SEEK_CUR);
      // Align to 2-byte boundary
      if (chunkSize & 1) fseek(f, 1, SEEK_CUR);
    }
  }

  fclose(f);

  if (!foundFmt || !foundData) return false;

  info.audioFormat = fmt.audioFormat;
  info.numChannels = fmt.numChannels;
  info.sampleRate = fmt.sampleRate;
  info.bitsPerSample = fmt.bitsPerSample;

  int bytesPerFrame = (fmt.bitsPerSample / 8) * fmt.numChannels;
  if (bytesPerFrame > 0) {
    info.numFrames = (int)(dataSize / (uint32_t)bytesPerFrame);
  }

  DBG("[AudioEngine] WAV header: %dch %dHz %dbit fmt=%d frames=%d\n",
      info.numChannels, info.sampleRate, info.bitsPerSample, info.audioFormat, info.numFrames);

  return true;
}

static inline int16_t doubleToS16(double v)
{
  v = std::max(-1.0, std::min(1.0, v));
  return (int16_t)(v * 32767.0);
}

static inline void doubleToS24(double v, uint8_t* out)
{
  v = std::max(-1.0, std::min(1.0, v));
  int32_t i = (int32_t)(v * 8388607.0);
  out[0] = (uint8_t)(i & 0xFF);
  out[1] = (uint8_t)((i >> 8) & 0xFF);
  out[2] = (uint8_t)((i >> 16) & 0xFF);
}

static inline float doubleToF32(double v)
{
  return (float)v;
}

bool AudioEngine::WriteWavFile(const std::string& path, const double* samples,
                                int numFrames, int numChannels, int sampleRate,
                                int bitsPerSample, int audioFormat)
{
  std::string tmpPath = path + ".sneakpeak.tmp";
  FILE* f = fopen(tmpPath.c_str(), "wb");
  if (!f) {
    DBG("[AudioEngine] Failed to open tmp file for writing: %s\n", tmpPath.c_str());
    return false;
  }

  int bytesPerSample = bitsPerSample / 8;
  int bytesPerFrame = bytesPerSample * numChannels;
  int64_t dataSizeCheck = (int64_t)numFrames * (int64_t)bytesPerFrame;
  if (dataSizeCheck > UINT32_MAX) {
    DBG("[AudioEngine] WAV size overflow: %lld bytes\n", (long long)dataSizeCheck);
    fclose(f); remove(tmpPath.c_str()); return false;
  }
  uint32_t dataSize = (uint32_t)dataSizeCheck;

  // RIFF header
  RiffHeader riff;
  memcpy(riff.riffId, "RIFF", 4);
  riff.fileSize = 36 + dataSize;
  memcpy(riff.waveId, "WAVE", 4);
  fwrite(&riff, sizeof(riff), 1, f);

  // fmt chunk
  FmtChunk fmt;
  memcpy(fmt.fmtId, "fmt ", 4);
  fmt.chunkSize = 16;
  fmt.audioFormat = (uint16_t)audioFormat;
  fmt.numChannels = (uint16_t)numChannels;
  fmt.sampleRate = (uint32_t)sampleRate;
  fmt.byteRate = (uint32_t)(sampleRate * bytesPerFrame);
  fmt.blockAlign = (uint16_t)bytesPerFrame;
  fmt.bitsPerSample = (uint16_t)bitsPerSample;
  fwrite(&fmt, sizeof(fmt), 1, f);

  // data chunk header
  DataChunkHeader data;
  memcpy(data.dataId, "data", 4);
  data.dataSize = dataSize;
  fwrite(&data, sizeof(data), 1, f);

  // Write sample data
  size_t totalSamples = (size_t)numFrames * (size_t)numChannels;

  if (audioFormat == 3 && bitsPerSample == 32) {
    // 32-bit float
    for (size_t i = 0; i < totalSamples; i++) {
      float v = doubleToF32(samples[i]);
      fwrite(&v, sizeof(float), 1, f);
    }
  } else if (bitsPerSample == 16) {
    for (size_t i = 0; i < totalSamples; i++) {
      int16_t v = doubleToS16(samples[i]);
      fwrite(&v, sizeof(int16_t), 1, f);
    }
  } else if (bitsPerSample == 24) {
    for (size_t i = 0; i < totalSamples; i++) {
      uint8_t v[3];
      doubleToS24(samples[i], v);
      fwrite(v, 3, 1, f);
    }
  } else {
    // Unsupported format — write as 16-bit PCM fallback
    for (size_t i = 0; i < totalSamples; i++) {
      int16_t v = doubleToS16(samples[i]);
      fwrite(&v, sizeof(int16_t), 1, f);
    }
  }

  fclose(f);

  // Atomic rename
  if (rename(tmpPath.c_str(), path.c_str()) != 0) {
    DBG("[AudioEngine] rename failed: %s -> %s\n", tmpPath.c_str(), path.c_str());
    remove(tmpPath.c_str());
    return false;
  }

  DBG("[AudioEngine] Wrote WAV: %s (%d frames, %dch, %dHz, %dbit)\n",
      path.c_str(), numFrames, numChannels, sampleRate, bitsPerSample);
  return true;
}

void AudioEngine::RefreshItemSource(MediaItem* item, MediaItem_Take* take)
{
  if (!item || !take) return;

  std::string path = GetSourceFilePath(take);
  if (path.empty()) return;

  if (g_PCM_Source_CreateFromFile && g_SetMediaItemTake_Source) {
    PCM_source* newSrc = g_PCM_Source_CreateFromFile(path.c_str());
    if (newSrc) {
      g_SetMediaItemTake_Source(take, newSrc);
    }
  }

  // Rebuild peaks
  if (g_Main_OnCommand) g_Main_OnCommand(40441, 0);

  // Refresh arrange view
  if (g_UpdateArrange) g_UpdateArrange();
}

std::string AudioEngine::WriteTempWav(const double* samples, int numFrames,
                                       int numChannels, int sampleRate)
{
  // Generate unique temp path (use $TMPDIR if available)
  const char* tmpDir = getenv("TMPDIR");
  if (!tmpDir) tmpDir = "/tmp";
  char tmpPath[512];
  snprintf(tmpPath, sizeof(tmpPath), "%s/sneakpeak_export_%d.wav", tmpDir, (int)getpid());

  if (!WriteWavFile(tmpPath, samples, numFrames, numChannels, sampleRate, 16, 1)) {
    DBG("[AudioEngine] WriteTempWav failed\n");
    return {};
  }

  return std::string(tmpPath);
}
