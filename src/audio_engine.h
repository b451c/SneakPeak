// audio_engine.h — WAV file I/O and REAPER source refresh for SneakPeak
#pragma once

#include "platform.h"
#include "globals.h"
#include "wav_writer.h"
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
  // True when the take plays through a SECTION source (a section of a file or
  // a take reversed in REAPER): GetSourceFilePath then names the PARENT file.
  static bool IsSectionSource(MediaItem_Take* take);

  // Scratch directory for temp WAVs (preview, undo snapshot, paste, One-Shot
  // LUFS, export fallback): %TEMP% on Windows, $TMPDIR or /tmp elsewhere; no
  // trailing separator. A "/tmp" fallback pointed at a non-existent C:\tmp on
  // Windows (forum #105: silent Standalone preview). Paired with ProcessId()
  // for per-instance file names.
  static std::string TempDir();
  static int ProcessId();

  // Read WAV header info (does not read audio data). WAVE_FORMAT_EXTENSIBLE
  // reports the SubFormat tag; a data size of 0 / 0xFFFFFFFF (streamed WAV)
  // derives the frame count from the file size (audit A10.1 / A10.2).
  static bool ReadWavHeader(const std::string& path, WavInfo& info);

  // Every RIFF chunk of a WAV other than fmt / data / smpl, verbatim, in file
  // order (bext, iXML, LIST, cue, axml, ...). Empty when the file is not a
  // WAV. The Standalone save hands them to the writer (audit A10.4).
  static bool CollectWavCarryChunks(const std::string& path, std::vector<WavCarryChunk>& out);

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
  // Chunked decode of ANY file REAPER can open (v2.5 Convert & go): no whole-
  // file buffer - the conversion pump reads a chunk per tick and writes it
  // out. Interleaved doubles at the source's own rate and channel count.
  struct SourceReader {
    PCM_source* src = nullptr;
    int nch = 0, sr = 0;
    int64_t frames = 0, pos = 0;
  };
  static bool OpenSourceReader(const std::string& path, SourceReader& r);
  static int ReadSourceChunk(SourceReader& r, double* out, int maxFrames);   // 0 = done
  static void CloseSourceReader(SourceReader& r);

  static bool BeginStream(const std::string& path, StreamLoad& s);
  // True while more audio remains; false = finished (info.numFrames trimmed
  // to what actually decoded, source closed).
  static bool ReadStreamStep(StreamLoad& s, double budgetSec);
  static void AbortStream(StreamLoad& s);

  // Write audio data to WAV file (writes .tmp, then overwrites the destination
  // IN PLACE - same inode - so REAPER's pooled decoders see the new audio)
  // samples are interleaved doubles, will be converted to original format
  // loopStartFrame/loopEndFrame (END-EXCLUSIVE): when both valid, a `smpl`
  // sustain-loop chunk is appended after the data chunk (v2.4 INC-A4) so game
  // engines/samplers read the loop natively. -1/-1 = no chunk (default).
  // carry: foreign chunks re-emitted verbatim after the audio (A10.4).
  static bool WriteWavFile(const std::string& path, const double* samples,
                           int numFrames, int numChannels, int sampleRate,
                           int bitsPerSample, int audioFormat,
                           int loopStartFrame = -1, int loopEndFrame = -1,
                           const std::vector<WavCarryChunk>* carry = nullptr);

  // Copy src into dst opened for truncate+write: dst keeps its inode (created
  // when absent). False on any I/O error - dst may then be incomplete.
  static bool CopyFileInto(const std::string& src, const std::string& dst);
  // Channel count / rate / length of a file through REAPER's decoders, no
  // samples read - the Standalone gates (mono/stereo, buffer cap) run on it.
  static bool ProbeSource(const std::string& path, int* nch, int* sr, int64_t* frames);
  // remove() is the ANSI CRT call on Windows: a temp path under a non-ASCII
  // user name leaves the file behind. DeleteFileUTF8 there, remove() elsewhere.
  static bool RemoveFile(const std::string& path);

  // Refresh REAPER's source after modifying the file on disk
  static void RefreshItemSource(MediaItem* item, MediaItem_Take* take);

  // Destination for a drag export ([basename]_sel_HHMMSS.wav).
  // Priority: 1) project recording folder, 2) next to sourceFile, 3) /tmp
  static std::string ExportWavPath(const char* sourceFilePath);

  // Write audio to WAV file for drag export at ExportWavPath.
  // Returns path or empty on failure.
  static std::string WriteExportWav(const double* samples, int numFrames,
                                     int numChannels, int sampleRate,
                                     int bitsPerSample = 16, int audioFormat = 1,
                                     const char* sourceFilePath = nullptr);
};
