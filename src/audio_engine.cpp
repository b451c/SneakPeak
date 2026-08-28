// audio_engine.cpp — WAV file I/O and REAPER source management
#include "audio_engine.h"
#include "wav_writer.h"
#include "reaper_plugin.h"
#include "debug.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <cerrno>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

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

bool AudioEngine::ReadWavFile(const std::string& path, WavInfo& info,
                               std::vector<double>& samples)
{
  samples.clear();
  if (!ReadWavHeader(path, info)) return false;
  if (info.numFrames <= 0 || info.numChannels <= 0) return false;

  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;

  // Skip RIFF header
  RiffHeader riff;
  if (fread(&riff, sizeof(riff), 1, f) != 1) { fclose(f); return false; }

  // Find data chunk
  bool found = false;
  while (!feof(f)) {
    char chunkId[4];
    uint32_t chunkSize;
    if (fread(chunkId, 4, 1, f) != 1) break;
    if (fread(&chunkSize, 4, 1, f) != 1) break;
    if (memcmp(chunkId, "data", 4) == 0) { found = true; break; }
    fseek(f, (long)chunkSize, SEEK_CUR);
    if (chunkSize & 1) fseek(f, 1, SEEK_CUR);
  }
  if (!found) { fclose(f); return false; }

  size_t totalSamples = (size_t)info.numFrames * (size_t)info.numChannels;
  samples.resize(totalSamples, 0.0);

  if (info.audioFormat == 3 && info.bitsPerSample == 32) {
    // 32-bit float
    std::vector<float> buf(totalSamples);
    size_t read = fread(buf.data(), sizeof(float), totalSamples, f);
    for (size_t i = 0; i < read; i++) samples[i] = (double)buf[i];
  } else if (info.bitsPerSample == 16) {
    std::vector<int16_t> buf(totalSamples);
    size_t read = fread(buf.data(), sizeof(int16_t), totalSamples, f);
    for (size_t i = 0; i < read; i++) samples[i] = (double)buf[i] / 32768.0;
  } else if (info.bitsPerSample == 24) {
    std::vector<uint8_t> buf(totalSamples * 3);
    size_t bytesRead = fread(buf.data(), 1, totalSamples * 3, f);
    size_t framesRead = bytesRead / 3;
    for (size_t i = 0; i < framesRead; i++) {
      int32_t v = (int32_t)buf[i*3] | ((int32_t)buf[i*3+1] << 8) | ((int32_t)buf[i*3+2] << 16);
      if (v & 0x800000) v |= (int32_t)0xFF000000; // sign extend
      samples[i] = (double)v / 8388608.0;
    }
  } else {
    fclose(f); return false;
  }

  fclose(f);
  DBG("[AudioEngine] ReadWavFile: %s (%d frames, %dch, %dHz, %dbit)\n",
      path.c_str(), info.numFrames, info.numChannels, info.sampleRate, info.bitsPerSample);
  return true;
}

bool AudioEngine::WriteWavFile(const std::string& path, const double* samples,
                                int numFrames, int numChannels, int sampleRate,
                                int bitsPerSample, int audioFormat,
                                int loopStartFrame, int loopEndFrame)
{
  // One encoder for whole buffers and streamed exports (wav_writer.h).
  WavWriter w;
  if (!w.Begin(path, numChannels, sampleRate, bitsPerSample, audioFormat)) return false;
  if (!w.Write(samples, numFrames)) return false;
  return w.End(loopStartFrame, loopEndFrame);
}

bool AudioEngine::CopyFileInto(const std::string& src, const std::string& dst)
{
  FILE* in = fopen(src.c_str(), "rb");
  FILE* out = in ? fopen(dst.c_str(), "wb") : nullptr;
  if (!in || !out) {
    DBG("[AudioEngine] CopyFileInto: cannot open %s (errno=%d)\n",
        (in ? dst : src).c_str(), errno);
    if (in) fclose(in);
    return false;
  }
  std::vector<char> buf(1 << 20);
  bool ok = true;
  size_t n;
  while (ok && (n = fread(buf.data(), 1, buf.size(), in)) > 0)
    ok = fwrite(buf.data(), 1, n, out) == n;
  fclose(in);
  return (fclose(out) == 0) && ok;
}

bool AudioEngine::ReadAudioFile(const std::string& path, WavInfo& info,
                                 std::vector<double>& samples)
{
  samples.clear();

  // Try REAPER's PCM_Source first — supports WAV, MP3, FLAC, OGG, AIFF, etc.
  if (g_PCM_Source_CreateFromFile) {
    PCM_source* src = g_PCM_Source_CreateFromFile(path.c_str());
    if (src) {
      int nch = src->GetNumChannels();
      int sr = (int)src->GetSampleRate();
      double length = src->GetLength();
      int totalFrames = (int)(length * sr);

      if (nch > 0 && sr > 0 && totalFrames > 0) {
        if (nch > 2) nch = 2; // cap at stereo

        info.numChannels = nch;
        info.sampleRate = sr;
        info.numFrames = totalFrames;
        info.bitsPerSample = 32; // PCM_Source always gives us float-quality
        info.audioFormat = 3;    // treat as float for write-back

        // Check if source is WAV — preserve original format for write-back
        {
          WavInfo wavInfo;
          if (ReadWavHeader(path, wavInfo)) {
            info.bitsPerSample = wavInfo.bitsPerSample;
            info.audioFormat = wavInfo.audioFormat;
          }
        }

        samples.resize((size_t)totalFrames * nch, 0.0);

        // Read in chunks
        static const int CHUNK = 65536;
        std::vector<double> buf((size_t)CHUNK * nch);
        int framesRead = 0;
        while (framesRead < totalFrames) {
          int chunk = std::min(CHUNK, totalFrames - framesRead);
          PCM_source_transfer_t transfer = {};
          transfer.time_s = (double)framesRead / (double)sr;
          transfer.length = chunk;
          transfer.nch = nch;
          transfer.samplerate = sr;
          transfer.samples = buf.data();
          src->GetSamples(&transfer);

          int got = transfer.samples_out;
          if (got <= 0) break;
          memcpy(samples.data() + (size_t)framesRead * nch,
                 buf.data(), (size_t)got * nch * sizeof(double));
          framesRead += got;
        }

        delete src;
        info.numFrames = framesRead;
        DBG("[AudioEngine] ReadAudioFile (PCM_Source): %s (%d frames, %dch, %dHz)\n",
            path.c_str(), framesRead, nch, sr);
        return framesRead > 0;
      }
      delete src;
    }
  }

  // Fallback: direct WAV reading
  return ReadWavFile(path, info, samples);
}

// --- Incremental streaming load (STA-1) ---

bool AudioEngine::BeginStream(const std::string& path, StreamLoad& s)
{
  AbortStream(s);
  if (!g_PCM_Source_CreateFromFile) return false;
  PCM_source* src = g_PCM_Source_CreateFromFile(path.c_str());
  if (!src) return false;
  int nch = src->GetNumChannels();
  int sr = (int)src->GetSampleRate();
  double length = src->GetLength();
  int totalFrames = (int)(length * sr);
  if (nch <= 0 || sr <= 0 || totalFrames <= 0) { delete src; return false; }
  if (nch > 2) nch = 2; // cap at stereo (matches ReadAudioFile)

  s.src = src;
  s.path = path;
  s.info.numChannels = nch;
  s.info.sampleRate = sr;
  s.info.numFrames = totalFrames;
  s.info.bitsPerSample = 32; // PCM_Source always gives float-quality
  s.info.audioFormat = 3;
  {
    WavInfo wavInfo; // WAV source: preserve the original format for write-back
    if (ReadWavHeader(path, wavInfo)) {
      s.info.bitsPerSample = wavInfo.bitsPerSample;
      s.info.audioFormat = wavInfo.audioFormat;
    }
  }
  s.framesRead = 0;
  s.totalFrames = totalFrames;
  s.samples.assign((size_t)totalFrames * nch, 0.0);
  return true;
}

bool AudioEngine::ReadStreamStep(StreamLoad& s, double budgetSec)
{
  if (!s.src) return false;
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  static const int CHUNK = 65536;
  const int nch = s.info.numChannels;
  std::vector<double> buf((size_t)CHUNK * nch);

  while (s.framesRead < s.totalFrames) {
    int chunk = std::min(CHUNK, s.totalFrames - s.framesRead);
    PCM_source_transfer_t transfer = {};
    transfer.time_s = (double)s.framesRead / (double)s.info.sampleRate;
    transfer.length = chunk;
    transfer.nch = nch;
    transfer.samplerate = s.info.sampleRate;
    transfer.samples = buf.data();
    s.src->GetSamples(&transfer);
    int got = transfer.samples_out;
    if (got <= 0) break; // decoder end/error: finish with what decoded
    memcpy(s.samples.data() + (size_t)s.framesRead * nch,
           buf.data(), (size_t)got * nch * sizeof(double));
    s.framesRead += got;
    if (std::chrono::duration<double>(clock::now() - t0).count() >= budgetSec)
      return s.framesRead < s.totalFrames;
  }

  // Finished (or the decoder stopped early - same tolerance as ReadAudioFile)
  s.info.numFrames = s.framesRead;
  s.samples.resize((size_t)s.framesRead * (size_t)nch);
  delete s.src;
  s.src = nullptr;
  return false;
}

void AudioEngine::AbortStream(StreamLoad& s)
{
  if (s.src) { delete s.src; s.src = nullptr; }
  s.samples.clear();
  s.samples.shrink_to_fit();
  s.framesRead = 0;
  s.totalFrames = 0;
  s.path.clear();
}

void AudioEngine::RefreshItemSource(MediaItem* item, MediaItem_Take* take)
{
  if (!item || !take) return;

  std::string path = GetSourceFilePath(take);
  if (path.empty()) return;

  // Use P_SOURCE via GetSetMediaItemTakeInfo for proper ownership management.
  // SetMediaItemTake_Source leaks the old source (REAPER SDK docs: "C/C++ code
  // should not use this and instead use GetSetMediaItemTakeInfo with P_SOURCE").
#ifndef _WIN32
  if (g_PCM_Source_CreateFromFile && g_GetSetMediaItemTakeInfo) {
    PCM_source* newSrc = g_PCM_Source_CreateFromFile(path.c_str());
    if (newSrc) {
      // P_SOURCE with set=true transfers ownership; REAPER destroys the old source
      g_GetSetMediaItemTakeInfo(take, "P_SOURCE", newSrc);
    }
  }
#else
  // Windows: the swap leaves a deny-write handle on the file (the replaced
  // source's decoder never lets go), so every LATER in-place write - the undo
  // restore above all - dies with a sharing violation for the rest of the
  // session. Since F7 the edit rewrites the SAME inode, and on Windows
  // UpdateItemInProject + the peak rebuild below are enough for REAPER to
  // serve the new audio (proven by the reverse spec's track-accessor oracle),
  // so the swap is skipped here.
#endif

  // Invalidate REAPER's cached media data. On macOS UpdateArrange alone is enough, but on Linux the audio cache survives until UpdateItemInProject fires, leaving destructive edits (Reverse, DC remove, Normalize) inaudible until REAPER restart.
  if (g_UpdateItemInProject) g_UpdateItemInProject(item);

  // Rebuild peaks
  if (g_Main_OnCommand) g_Main_OnCommand(40441, 0);

  // Refresh arrange view
  if (g_UpdateArrange) g_UpdateArrange();
}

std::string AudioEngine::TempDir()
{
#ifdef _WIN32
  wchar_t w[MAX_PATH] = {};
  DWORD n = GetTempPathW(MAX_PATH, w);
  if (n == 0 || n >= MAX_PATH) return ".";
  char utf8[MAX_PATH * 4] = {};
  WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, sizeof(utf8), nullptr, nullptr);
  std::string dir(utf8);
  while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
  return dir;
#else
  const char* tmpDir = getenv("TMPDIR");
  std::string dir(tmpDir && tmpDir[0] ? tmpDir : "/tmp");
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  return dir;
#endif
}

int AudioEngine::ProcessId()
{
#ifdef _WIN32
  return _getpid();
#else
  return (int)getpid();
#endif
}

std::string AudioEngine::ExportWavPath(const char* sourceFilePath)
{
  // Generate filename: [basename]_sel_HHMMSS.wav (includes original name)
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  char baseName[128] = "sneakpeak";
  if (sourceFilePath && sourceFilePath[0]) {
    const char* fn = sourceFilePath;
    const char* lastSlash = strrchr(fn, '/');
#ifdef _WIN32
    const char* lastBackslash = strrchr(fn, '\\');   // REAPER hands us native separators
    if (lastBackslash && (!lastSlash || lastBackslash > lastSlash)) lastSlash = lastBackslash;
#endif
    if (lastSlash) fn = lastSlash + 1;
    snprintf(baseName, sizeof(baseName), "%s", fn);
    // Strip extension
    char* dot = strrchr(baseName, '.');
    if (dot) *dot = '\0';
  }
  char filename[256];
  snprintf(filename, sizeof(filename), "%s_sel_%02d%02d%02d.wav",
           baseName, t->tm_hour, t->tm_min, t->tm_sec);

  char exportPath[512] = {};
  const char* chosen = nullptr;

  // Priority 1: project recording folder (if project is saved)
  if (g_GetProjectPathEx && g_EnumProjects) {
    char projFile[512] = {};
    g_EnumProjects(-1, projFile, sizeof(projFile));
    if (projFile[0]) {
      char projPath[512] = {};
      g_GetProjectPathEx(nullptr, projPath, sizeof(projPath));
      if (projPath[0]) {
        snprintf(exportPath, sizeof(exportPath), "%s/%s", projPath, filename);
        chosen = "project";
      }
    }
  }

  // Priority 2: next to source file (standalone mode)
  if (!chosen && sourceFilePath && sourceFilePath[0]) {
    // Extract directory from source path
    std::string srcDir(sourceFilePath);
    size_t lastSlash = srcDir.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
      srcDir.resize(lastSlash);
      snprintf(exportPath, sizeof(exportPath), "%s/%s", srcDir.c_str(), filename);
      chosen = "source";
    }
  }

  // Priority 3: temp directory
  if (!chosen) {
    snprintf(exportPath, sizeof(exportPath), "%s/%s", TempDir().c_str(), filename);
    chosen = "tmp";
  }
  DBG("[AudioEngine] Export destination: %s (%s)\n", exportPath, chosen);
  return std::string(exportPath);
}

std::string AudioEngine::WriteExportWav(const double* samples, int numFrames,
                                         int numChannels, int sampleRate,
                                         int bitsPerSample, int audioFormat,
                                         const char* sourceFilePath)
{
  const std::string exportPath = ExportWavPath(sourceFilePath);
  if (!WriteWavFile(exportPath, samples, numFrames, numChannels, sampleRate,
                    bitsPerSample, audioFormat)) {
    DBG("[AudioEngine] WriteExportWav failed: %s\n", exportPath.c_str());
    return {};
  }
  return exportPath;
}
