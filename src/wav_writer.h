// wav_writer.h — chunked WAV encoder (v2.5 increment 8e, design_streaming_source.md 5.4)
// The header/encode/same-inode policy of AudioEngine::WriteWavFile, fed in
// pieces so a streamed export never holds the file in memory. Begin writes the
// header to <path>.sneakpeak.tmp; Write appends interleaved doubles as 16/24-bit
// PCM or 32-bit float; End patches the RIFF/data sizes, appends the optional
// smpl loop chunk and copies the image INTO <path> (same inode - F7). A failure
// before End removes the tmp; a failed final copy keeps it as the recovery copy,
// exactly like WriteWavFile. Pure stdio, no REAPER deps.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// A foreign RIFF chunk copied verbatim from the file being overwritten (bext,
// iXML, LIST, cue, axml, ...): the Standalone save re-emits the original's
// metadata after the audio (audit A10.4). Cue points are byte-copied and may
// point past a shortened file - documented, never rewritten.
struct WavCarryChunk {
  char id[4];
  std::vector<unsigned char> payload;
};

class WavWriter {
public:
  ~WavWriter();   // Abort() when still open

  // Unsupported depths fall back to 16-bit PCM (WriteWavFile parity).
  bool Begin(const std::string& path, int numChannels, int sampleRate,
             int bitsPerSample, int audioFormat);
  // False (and the tmp removed) on I/O failure or past the RIFF 4 GB limit.
  bool Write(const double* samples, int numFrames);
  // Queue a foreign chunk for End() (written after data and smpl, pad byte
  // honoured, counted in the RIFF size). Call between Begin and End.
  void AddCarryChunk(const WavCarryChunk& chunk) { m_carry.push_back(chunk); }
  // Loop frames END-EXCLUSIVE: a valid in-range pair appends a smpl chunk.
  // False (tmp removed) when the RIFF total would pass 4 GB.
  bool End(int loopStartFrame = -1, int loopEndFrame = -1);
  void Abort();
  int64_t Frames() const { return m_frames; }

private:
  FILE* m_f = nullptr;
  std::string m_path, m_tmp;
  int m_nch = 0, m_sr = 0, m_bits = 0, m_fmt = 0, m_bpf = 0;
  int64_t m_frames = 0;
  std::vector<unsigned char> m_buf;   // one encoded chunk
  std::vector<WavCarryChunk> m_carry;
};
