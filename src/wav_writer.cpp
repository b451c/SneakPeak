// wav_writer.cpp — chunked WAV encoder (see wav_writer.h)
#include "wav_writer.h"
#include "audio_engine.h"
#include "wav_smpl.h"
#include "debug.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// PCM encode = round to nearest on the decoder's grid (k / 32768, k / 8388608)
// and clamp at the ends: a load -> save round trip is the identity (audit
// A10.3; the old `v * 32767` truncation moved 65535 of the 65536 16-bit values).
inline int16_t doubleToS16(double v)
{
  const double s = (double)std::lrint(v * 32768.0);
  return (int16_t)std::max(-32768.0, std::min(32767.0, s));
}

inline void doubleToS24(double v, unsigned char* out)
{
  const double s = (double)std::lrint(v * 8388608.0);
  const int32_t i = (int32_t)std::max(-8388608.0, std::min(8388607.0, s));
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
      bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) {
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
  } else if (m_bits == 32) {   // 32-bit integer PCM (field recorders): kept, never folded to 16
    for (size_t i = 0; i < total; i++) {
      const int32_t v = (int32_t)std::max(-2147483648.0, std::min(2147483647.0, std::nearbyint(samples[i] * 2147483648.0)));
      memcpy(p + i * 4, &v, 4);
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
  const int64_t dataBytes = m_frames * m_bpf;
  // Loop points ride along only when they form a valid in-range region.
  const bool writeLoop = loopStartFrame >= 0 && loopEndFrame > loopStartFrame &&
                         (int64_t)loopEndFrame <= m_frames;
  // RIFF size (= file size - 8): every chunk pads to an even size (A10.5) and
  // the 32-bit field guards the whole image, not the audio alone.
  int64_t riffSize = 36 + dataBytes + (dataBytes & 1) + (writeLoop ? kSmplChunkBytes : 0);
  for (const auto& c : m_carry) riffSize += 8 + (int64_t)c.payload.size() + (c.payload.size() & 1);
  bool ok = riffSize <= (int64_t)UINT32_MAX;
  if (!ok) DBG("[WavWriter] RIFF size %lld bytes exceeds 4 GB\n", (long long)riffSize);
  if (ok && (dataBytes & 1)) ok = fputc(0, m_f) != EOF;   // pad byte: the next chunk starts even
  if (ok && writeLoop) {   // one forward sustain loop, infinite (INC-A4)
    unsigned char smpl[kSmplChunkBytes];
    BuildSmplChunk(m_sr, loopStartFrame, loopEndFrame, smpl);
    ok = fwrite(smpl, 1, sizeof(smpl), m_f) == sizeof(smpl);
  }
  for (size_t i = 0; ok && i < m_carry.size(); i++) {   // the original's metadata, verbatim
    const WavCarryChunk& c = m_carry[i];
    ok = fwrite(c.id, 4, 1, m_f) == 1 && PutU32(m_f, (uint32_t)c.payload.size()) &&
         (c.payload.empty() || fwrite(c.payload.data(), 1, c.payload.size(), m_f) == c.payload.size()) &&
         (!(c.payload.size() & 1) || fputc(0, m_f) != EOF);
  }
  ok = ok && fseek(m_f, (long)kRiffSizeOffset, SEEK_SET) == 0 && PutU32(m_f, (uint32_t)riffSize) &&
       fseek(m_f, (long)kDataSizeOffset, SEEK_SET) == 0 && PutU32(m_f, (uint32_t)dataBytes);
  ok = (fclose(m_f) == 0) && ok;
  m_f = nullptr;
  if (!ok) {
    DBG("[WavWriter] finalize failed: %s\n", m_tmp.c_str());
    AudioEngine::RemoveFile(m_tmp);
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
  AudioEngine::RemoveFile(m_tmp);
  DBG("[WavWriter] Wrote WAV: %s (%lld frames, %dch, %dHz, %dbit)\n",
      m_path.c_str(), (long long)m_frames, m_nch, m_sr, m_bits);
  return true;
}

void WavWriter::Abort()
{
  m_carry.clear();
  if (!m_f) return;
  fclose(m_f);
  m_f = nullptr;
  AudioEngine::RemoveFile(m_tmp);
}
