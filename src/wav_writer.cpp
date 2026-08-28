// wav_writer.cpp — chunked WAV encoder (see wav_writer.h)
#include "wav_writer.h"
#include "audio_engine.h"
#include "wav_smpl.h"
#include "debug.h"
#include <algorithm>
#include <cstring>

namespace {

inline int16_t doubleToS16(double v)
{
  v = std::max(-1.0, std::min(1.0, v));
  return (int16_t)(v * 32767.0);
}

inline void doubleToS24(double v, unsigned char* out)
{
  v = std::max(-1.0, std::min(1.0, v));
  int32_t i = (int32_t)(v * 8388607.0);
  out[0] = (unsigned char)(i & 0xFF);
  out[1] = (unsigned char)((i >> 8) & 0xFF);
  out[2] = (unsigned char)((i >> 16) & 0xFF);
}

bool PutU16(FILE* f, uint16_t v) { return fwrite(&v, 2, 1, f) == 1; }
bool PutU32(FILE* f, uint32_t v) { return fwrite(&v, 4, 1, f) == 1; }

constexpr int64_t kRiffSizeOffset = 4;    // RIFF fileSize field
constexpr int64_t kDataSizeOffset = 40;   // data chunk size field (44-byte header)

} // namespace

WavWriter::~WavWriter()
{
  Abort();
}

bool WavWriter::Begin(const std::string& path, int numChannels, int sampleRate,
                      int bitsPerSample, int audioFormat)
{
  Abort();
  if (numChannels <= 0 || sampleRate <= 0) return false;
  // Force unsupported bit depths to 16-bit PCM for consistency
  if (!(audioFormat == 3 && bitsPerSample == 32) &&
      bitsPerSample != 16 && bitsPerSample != 24) {
    bitsPerSample = 16;
    audioFormat = 1;
  }
  m_tmp = path + ".sneakpeak.tmp";
  m_f = fopen(m_tmp.c_str(), "wb");
  if (!m_f) {
    DBG("[WavWriter] Failed to open tmp file for writing: %s\n", m_tmp.c_str());
    return false;
  }
  m_path = path;
  m_nch = numChannels; m_sr = sampleRate; m_bits = bitsPerSample; m_fmt = audioFormat;
  m_bpf = (bitsPerSample / 8) * numChannels;
  m_frames = 0;

  // 44-byte header with the sizes left at 0; End() patches them.
  bool ok = fwrite("RIFF", 4, 1, m_f) == 1 && PutU32(m_f, 0) &&
            fwrite("WAVE", 4, 1, m_f) == 1 &&
            fwrite("fmt ", 4, 1, m_f) == 1 && PutU32(m_f, 16) &&
            PutU16(m_f, (uint16_t)audioFormat) && PutU16(m_f, (uint16_t)numChannels) &&
            PutU32(m_f, (uint32_t)sampleRate) && PutU32(m_f, (uint32_t)(sampleRate * m_bpf)) &&
            PutU16(m_f, (uint16_t)m_bpf) && PutU16(m_f, (uint16_t)bitsPerSample) &&
            fwrite("data", 4, 1, m_f) == 1 && PutU32(m_f, 0);
  if (!ok) Abort();
  return ok;
}

bool WavWriter::Write(const double* samples, int numFrames)
{
  if (!m_f) return false;
  if (numFrames <= 0) return true;
  const int64_t bytes = (int64_t)numFrames * m_bpf;
  if ((m_frames + numFrames) * (int64_t)m_bpf > (int64_t)UINT32_MAX) {
    DBG("[WavWriter] WAV size overflow at %lld frames\n", (long long)(m_frames + numFrames));
    Abort();
    return false;
  }
  m_buf.resize((size_t)bytes);
  const size_t total = (size_t)numFrames * (size_t)m_nch;
  unsigned char* p = m_buf.data();
  if (m_fmt == 3 && m_bits == 32) {
    for (size_t i = 0; i < total; i++) {
      const float v = (float)samples[i];
      memcpy(p + i * 4, &v, 4);
    }
  } else if (m_bits == 16) {
    for (size_t i = 0; i < total; i++) {
      const int16_t v = doubleToS16(samples[i]);
      memcpy(p + i * 2, &v, 2);
    }
  } else {   // 24
    for (size_t i = 0; i < total; i++) doubleToS24(samples[i], p + i * 3);
  }
  if (fwrite(m_buf.data(), 1, (size_t)bytes, m_f) != (size_t)bytes) {
    DBG("[WavWriter] short write on %s\n", m_tmp.c_str());
    Abort();
    return false;
  }
  m_frames += numFrames;
  return true;
}

bool WavWriter::End(int loopStartFrame, int loopEndFrame)
{
  if (!m_f) return false;
  const uint32_t dataSize = (uint32_t)(m_frames * m_bpf);
  // Loop points ride along only when they form a valid in-range region.
  const bool writeLoop = loopStartFrame >= 0 && loopEndFrame > loopStartFrame &&
                         (int64_t)loopEndFrame <= m_frames;
  bool ok = true;
  if (writeLoop) {   // one forward sustain loop, infinite (INC-A4)
    unsigned char smpl[kSmplChunkBytes];
    BuildSmplChunk(m_sr, loopStartFrame, loopEndFrame, smpl);
    ok = fwrite(smpl, 1, sizeof(smpl), m_f) == sizeof(smpl);
  }
  const uint32_t fileSize = 36 + dataSize + (writeLoop ? (uint32_t)kSmplChunkBytes : 0);
  ok = ok && fseek(m_f, (long)kRiffSizeOffset, SEEK_SET) == 0 && PutU32(m_f, fileSize) &&
       fseek(m_f, (long)kDataSizeOffset, SEEK_SET) == 0 && PutU32(m_f, dataSize);
  ok = (fclose(m_f) == 0) && ok;
  m_f = nullptr;
  if (!ok) {
    DBG("[WavWriter] finalize failed: %s\n", m_tmp.c_str());
    remove(m_tmp.c_str());
    return false;
  }

  // Overwrite the ORIGINAL file in place (same inode). REAPER pools decoders per
  // path: a rename() over the path leaves every already-open decoder on the old
  // inode, still serving the pre-edit audio (F7, forum #47). The tmp file is a
  // complete image, so the original is only touched once the encode succeeded;
  // if the copy fails the tmp is kept as the recovery copy.
  if (!AudioEngine::CopyFileInto(m_tmp, m_path)) {
    DBG("[WavWriter] in-place overwrite failed, tmp kept: %s\n", m_tmp.c_str());
    return false;
  }
  remove(m_tmp.c_str());
  DBG("[WavWriter] Wrote WAV: %s (%lld frames, %dch, %dHz, %dbit)\n",
      m_path.c_str(), (long long)m_frames, m_nch, m_sr, m_bits);
  return true;
}

void WavWriter::Abort()
{
  if (!m_f) return;
  fclose(m_f);
  m_f = nullptr;
  remove(m_tmp.c_str());
}
