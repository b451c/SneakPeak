// wav_inplace.cpp — in-place WAV sample edits (see wav_inplace.h)
#include "platform.h"   // fopen -> fopenUTF8 on Windows (non-ASCII source paths, audit A3.1)
#include "wav_inplace.h"
#include "debug.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

namespace {

constexpr int64_t kChunkFrames = 1 << 18;   // 256k frames: 1.5 MB per buffer at 24-bit stereo

struct WavFile {
  FILE* f = nullptr;
  int64_t dataOffset = 0;   // first byte of sample data
  int64_t frames = 0;       // frames in the data chunk
  int nch = 0, bits = 0, fmt = 0, bpf = 0;
  ~WavFile() { if (f) fclose(f); }
};

bool SeekTo(FILE* f, int64_t pos)
{
#ifdef _WIN32
  return _fseeki64(f, pos, SEEK_SET) == 0;
#else
  return fseeko(f, (off_t)pos, SEEK_SET) == 0;
#endif
}

int64_t TellAt(FILE* f)
{
#ifdef _WIN32
  return _ftelli64(f);
#else
  return (int64_t)ftello(f);
#endif
}

// Open for update and walk the RIFF chunks to the data chunk.
bool Open(const std::string& path, WavFile& w)
{
  w.f = fopen(path.c_str(), "r+b");
  if (!w.f) { DBG("[WavInplace] cannot open for update: %s\n", path.c_str()); return false; }
  char id[4];
  uint32_t size;
  if (fread(id, 4, 1, w.f) != 1 || memcmp(id, "RIFF", 4) != 0) return false;
  if (fread(&size, 4, 1, w.f) != 1) return false;
  if (fread(id, 4, 1, w.f) != 1 || memcmp(id, "WAVE", 4) != 0) return false;
  bool haveFmt = false;
  for (;;) {
    if (fread(id, 4, 1, w.f) != 1 || fread(&size, 4, 1, w.f) != 1) return false;
    const int64_t next = TellAt(w.f) + (int64_t)size + (size & 1);
    if (memcmp(id, "fmt ", 4) == 0) {
      uint16_t fmt = 0, nch = 0, bits = 0, align = 0;
      uint32_t rate = 0, byteRate = 0;
      if (size < 16 || fread(&fmt, 2, 1, w.f) != 1 || fread(&nch, 2, 1, w.f) != 1 ||
          fread(&rate, 4, 1, w.f) != 1 || fread(&byteRate, 4, 1, w.f) != 1 ||
          fread(&align, 2, 1, w.f) != 1 || fread(&bits, 2, 1, w.f) != 1)
        return false;
      w.fmt = fmt; w.nch = nch; w.bits = bits;
      haveFmt = true;
    } else if (memcmp(id, "data", 4) == 0) {
      const bool pcm = w.fmt == 1 && (w.bits == 16 || w.bits == 24);
      const bool f32 = w.fmt == 3 && w.bits == 32;
      w.bpf = (w.bits / 8) * w.nch;
      if (!haveFmt || !(pcm || f32) || w.bpf <= 0) {
        DBG("[WavInplace] unsupported format fmt=%d bits=%d nch=%d\n", w.fmt, w.bits, w.nch);
        return false;
      }
      w.dataOffset = TellAt(w.f);
      w.frames = (int64_t)size / w.bpf;
      return true;
    }
    if (!SeekTo(w.f, next)) return false;
  }
}

bool ReadFrames(WavFile& w, int64_t frame, int64_t n, uint8_t* out)
{
  return SeekTo(w.f, w.dataOffset + frame * w.bpf) &&
         fread(out, (size_t)w.bpf, (size_t)n, w.f) == (size_t)n;
}

bool WriteFrames(WavFile& w, int64_t frame, int64_t n, const uint8_t* in)
{
  return SeekTo(w.f, w.dataOffset + frame * w.bpf) &&
         fwrite(in, (size_t)w.bpf, (size_t)n, w.f) == (size_t)n;
}

void Clamp(const WavFile& w, int64_t& s0, int64_t& s1)
{
  s0 = std::max<int64_t>(0, std::min(s0, w.frames));
  s1 = std::max(s0, std::min(s1, w.frames));
}

// Interleaved samples <-> file bytes (same scaling as audio_engine.cpp).
void Decode(const WavFile& w, const uint8_t* in, size_t n, double* out)
{
  if (w.fmt == 3) {
    for (size_t i = 0; i < n; i++) { float v; memcpy(&v, in + i * 4, 4); out[i] = (double)v; }
  } else if (w.bits == 16) {
    for (size_t i = 0; i < n; i++) { int16_t v; memcpy(&v, in + i * 2, 2); out[i] = (double)v / 32768.0; }
  } else {
    for (size_t i = 0; i < n; i++) {
      int32_t v = (int32_t)in[i * 3] | ((int32_t)in[i * 3 + 1] << 8) | ((int32_t)in[i * 3 + 2] << 16);
      if (v & 0x800000) v |= (int32_t)0xFF000000;
      out[i] = (double)v / 8388608.0;
    }
  }
}

void Encode(const WavFile& w, const double* in, size_t n, uint8_t* out)
{
  if (w.fmt == 3) {
    for (size_t i = 0; i < n; i++) { float v = (float)in[i]; memcpy(out + i * 4, &v, 4); }
    return;
  }
  for (size_t i = 0; i < n; i++) {
    const double v = std::max(-1.0, std::min(1.0, in[i]));
    if (w.bits == 16) {
      const int16_t s = (int16_t)(v * 32767.0);
      memcpy(out + i * 2, &s, 2);
    } else {
      const int32_t s = (int32_t)(v * 8388607.0);
      out[i * 3] = (uint8_t)(s & 0xFF);
      out[i * 3 + 1] = (uint8_t)((s >> 8) & 0xFF);
      out[i * 3 + 2] = (uint8_t)((s >> 16) & 0xFF);
    }
  }
}

void ReverseFrames(uint8_t* buf, int64_t n, int bpf)
{
  for (int64_t i = 0, j = n - 1; i < j; i++, j--)
    std::swap_ranges(buf + i * bpf, buf + (i + 1) * bpf, buf + j * bpf);
}

} // namespace

namespace WavInplace {

bool Reverse(const std::string& path, int64_t s0, int64_t s1)
{
  WavFile w;
  if (!Open(path, w)) return false;
  Clamp(w, s0, s1);
  // Two cursors closing in: front block A and back block B swap places, each
  // reversed; blocks never overlap (n <= half of what remains).
  std::vector<uint8_t> a((size_t)(kChunkFrames * w.bpf)), b(a.size());
  int64_t lo = s0, hi = s1;
  while (hi - lo > 1) {
    const int64_t n = std::min(kChunkFrames, (hi - lo) / 2);
    if (!ReadFrames(w, lo, n, a.data()) || !ReadFrames(w, hi - n, n, b.data())) return false;
    ReverseFrames(a.data(), n, w.bpf);
    ReverseFrames(b.data(), n, w.bpf);
    if (!WriteFrames(w, lo, n, b.data()) || !WriteFrames(w, hi - n, n, a.data())) return false;
    lo += n;
    hi -= n;
  }
  return fflush(w.f) == 0;
}

bool Gain(const std::string& path, int64_t s0, int64_t s1, double factor, int fadeFrames)
{
  WavFile w;
  if (!Open(path, w)) return false;
  Clamp(w, s0, s1);
  const int64_t total = s1 - s0;
  const int64_t fade = std::min<int64_t>(fadeFrames, total / 2);
  std::vector<uint8_t> raw((size_t)(kChunkFrames * w.bpf));
  std::vector<double> smp((size_t)(kChunkFrames * w.nch));
  for (int64_t pos = s0; pos < s1;) {
    const int64_t n = std::min(kChunkFrames, s1 - pos);
    if (!ReadFrames(w, pos, n, raw.data())) return false;
    Decode(w, raw.data(), (size_t)(n * w.nch), smp.data());
    for (int64_t f = 0; f < n; f++) {
      const int64_t i = pos - s0 + f;
      double g = factor;
      if (fade > 1) {
        if (i < fade) g = 1.0 + ((double)i / (double)fade) * (factor - 1.0);
        else if (i >= total - fade) g = 1.0 + ((double)(total - 1 - i) / (double)fade) * (factor - 1.0);
      }
      for (int ch = 0; ch < w.nch; ch++) smp[(size_t)(f * w.nch + ch)] *= g;
    }
    Encode(w, smp.data(), (size_t)(n * w.nch), raw.data());
    if (!WriteFrames(w, pos, n, raw.data())) return false;
    pos += n;
  }
  return fflush(w.f) == 0;
}

bool DCRemove(const std::string& path, int64_t s0, int64_t s1)
{
  WavFile w;
  if (!Open(path, w)) return false;
  Clamp(w, s0, s1);
  if (s1 <= s0) return true;
  std::vector<uint8_t> raw((size_t)(kChunkFrames * w.bpf));
  std::vector<double> smp((size_t)(kChunkFrames * w.nch));
  std::vector<double> mean((size_t)w.nch, 0.0);
  for (int64_t pos = s0; pos < s1;) {   // pass 1: per-channel mean
    const int64_t n = std::min(kChunkFrames, s1 - pos);
    if (!ReadFrames(w, pos, n, raw.data())) return false;
    Decode(w, raw.data(), (size_t)(n * w.nch), smp.data());
    for (int64_t f = 0; f < n; f++)
      for (int ch = 0; ch < w.nch; ch++) mean[(size_t)ch] += smp[(size_t)(f * w.nch + ch)];
    pos += n;
  }
  for (double& m : mean) m /= (double)(s1 - s0);
  for (int64_t pos = s0; pos < s1;) {   // pass 2: subtract
    const int64_t n = std::min(kChunkFrames, s1 - pos);
    if (!ReadFrames(w, pos, n, raw.data())) return false;
    Decode(w, raw.data(), (size_t)(n * w.nch), smp.data());
    for (int64_t f = 0; f < n; f++)
      for (int ch = 0; ch < w.nch; ch++) smp[(size_t)(f * w.nch + ch)] -= mean[(size_t)ch];
    Encode(w, smp.data(), (size_t)(n * w.nch), raw.data());
    if (!WriteFrames(w, pos, n, raw.data())) return false;
    pos += n;
  }
  return fflush(w.f) == 0;
}

} // namespace WavInplace
