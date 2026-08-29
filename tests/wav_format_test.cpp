// wav_format_test.cpp — WAV format robustness (audit A10; offline, no REAPER)
//
// Hand-built files exercise the three WAV code paths SneakPeak owns:
//   ReadWavHeader (format probe for exports / Standalone write-back),
//   WavInplace   (destructive ITEM edits streamed through the file itself),
//   WavWriter    (every file SneakPeak writes).
// Every assertion is exact (byte or integer compare) and deterministic.
//   A10.1 WAVE_FORMAT_EXTENSIBLE (0xFFFE) resolves to the real tag (1 / 3)
//   A10.2 data size 0 / 0xFFFFFFFF (streamed WAVs) = frames from the file size
//   A10.3 PCM encode is lrint + clamp: load -> save is the identity on the grid
//   A10.4 metadata chunks of the original ride along on a Standalone overwrite
//   A10.5 pad byte after an odd data chunk; RIFF size counts every chunk
#include "audio_engine.h"
#include "wav_inplace.h"
#include "wav_smpl.h"
#include "wav_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failed = 0, g_checks = 0;

#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failed++; \
  printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

using Bytes = std::vector<unsigned char>;

void Put16(Bytes& b, uint16_t v) { b.push_back((unsigned char)(v & 0xFF)); b.push_back((unsigned char)(v >> 8)); }
void Put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((unsigned char)((v >> (8 * i)) & 0xFF)); }
void PutId(Bytes& b, const char* id) { b.insert(b.end(), id, id + 4); }
uint16_t Get16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Get32(const unsigned char* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

std::string TempPath(const char* name)
{
  return AudioEngine::TempDir() + "/sneakpeak_wavtest_" + std::to_string(AudioEngine::ProcessId()) + "_" + name;
}

bool WriteBytes(const std::string& path, const Bytes& b)
{
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = fwrite(b.data(), 1, b.size(), f) == b.size();
  return (fclose(f) == 0) && ok;
}

Bytes ReadBytes(const std::string& path)
{
  Bytes b;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return b;
  unsigned char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
  fclose(f);
  return b;
}

struct Chunk { std::string id; size_t offset; Bytes payload; };   // offset = of the id

// Strict RIFF walk: every chunk starts on an even offset, the RIFF size is the
// file size - 8, and the walk ends exactly at the end of the file.
bool WalkRiff(const Bytes& f, std::vector<Chunk>& out, std::string* why)
{
  out.clear();
  if (f.size() < 12 || memcmp(f.data(), "RIFF", 4) != 0 || memcmp(f.data() + 8, "WAVE", 4) != 0) { *why = "not RIFF/WAVE"; return false; }
  if (Get32(f.data() + 4) != f.size() - 8) { *why = "RIFF size " + std::to_string(Get32(f.data() + 4)) + " != file size - 8 (" + std::to_string(f.size() - 8) + ")"; return false; }
  size_t pos = 12;
  while (pos < f.size()) {
    if (pos & 1) { *why = "chunk at odd offset " + std::to_string(pos); return false; }
    if (pos + 8 > f.size()) { *why = "truncated chunk header"; return false; }
    Chunk c;
    c.id.assign((const char*)f.data() + pos, 4);
    const uint32_t size = Get32(f.data() + pos + 4);
    if (pos + 8 + size > f.size()) { *why = "chunk " + c.id + " runs past the file"; return false; }
    c.offset = pos;
    c.payload.assign(f.begin() + (long)(pos + 8), f.begin() + (long)(pos + 8 + size));
    out.push_back(c);
    pos += 8 + size + (size & 1);
  }
  return true;
}

const Chunk* Find(const std::vector<Chunk>& cs, const char* id)
{
  for (const auto& c : cs) if (c.id == id) return &c;
  return nullptr;
}

// A WAV image: fmt (plain or EXTENSIBLE), the given data bytes (dataSizeField
// overrides the declared size when >= 0), extra chunks before the data.
Bytes BuildWav(int fmtTag, int nch, int rate, int bits, const Bytes& data, bool extensible,
               int64_t dataSizeField = -1, const std::vector<Chunk>& before = {},
               const std::vector<Chunk>& after = {})
{
  Bytes b;
  PutId(b, "RIFF"); Put32(b, 0); PutId(b, "WAVE");
  PutId(b, "fmt "); Put32(b, extensible ? 40 : 16);
  Put16(b, extensible ? 0xFFFE : (uint16_t)fmtTag); Put16(b, (uint16_t)nch); Put32(b, (uint32_t)rate);
  const int bpf = bits / 8 * nch;
  Put32(b, (uint32_t)(rate * bpf)); Put16(b, (uint16_t)bpf); Put16(b, (uint16_t)bits);
  if (extensible) {
    Put16(b, 22); Put16(b, (uint16_t)bits); Put32(b, nch == 2 ? 3u : 4u);   // cbSize, validBits, channel mask
    Put16(b, (uint16_t)fmtTag);                                               // SubFormat GUID: tag + KSDATAFORMAT_SUBTYPE base
    const unsigned char guidTail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
    b.insert(b.end(), guidTail, guidTail + 14);
  }
  for (const auto& c : before) {
    PutId(b, c.id.c_str()); Put32(b, (uint32_t)c.payload.size());
    b.insert(b.end(), c.payload.begin(), c.payload.end());
    if (c.payload.size() & 1) b.push_back(0);
  }
  PutId(b, "data"); Put32(b, dataSizeField >= 0 ? (uint32_t)dataSizeField : (uint32_t)data.size());
  b.insert(b.end(), data.begin(), data.end());
  if (data.size() & 1) b.push_back(0);
  for (const auto& c : after) {
    PutId(b, c.id.c_str()); Put32(b, (uint32_t)c.payload.size());
    b.insert(b.end(), c.payload.begin(), c.payload.end());
    if (c.payload.size() & 1) b.push_back(0);
  }
  const uint32_t riff = (uint32_t)(b.size() - 8);
  for (int i = 0; i < 4; i++) b[4 + (size_t)i] = (unsigned char)((riff >> (8 * i)) & 0xFF);
  return b;
}

Bytes Pcm16(const std::vector<int16_t>& v) { Bytes b; for (int16_t s : v) Put16(b, (uint16_t)s); return b; }
Bytes Pcm24(const std::vector<int32_t>& v)
{
  Bytes b;
  for (int32_t s : v) { b.push_back((unsigned char)(s & 0xFF)); b.push_back((unsigned char)((s >> 8) & 0xFF)); b.push_back((unsigned char)((s >> 16) & 0xFF)); }
  return b;
}
Bytes F32(const std::vector<float>& v) { Bytes b; for (float s : v) { unsigned char t[4]; memcpy(t, &s, 4); b.insert(b.end(), t, t + 4); } return b; }

std::vector<int16_t> DecodePcm16(const Bytes& d) { std::vector<int16_t> v; for (size_t i = 0; i + 1 < d.size(); i += 2) v.push_back((int16_t)Get16(d.data() + i)); return v; }
std::vector<int32_t> DecodePcm24(const Bytes& d)
{
  std::vector<int32_t> v;
  for (size_t i = 0; i + 2 < d.size(); i += 3) {
    int32_t s = (int32_t)d[i] | ((int32_t)d[i + 1] << 8) | ((int32_t)d[i + 2] << 16);
    if (s & 0x800000) s |= (int32_t)0xFF000000;
    v.push_back(s);
  }
  return v;
}

// ---------------------------------------------------------------------------
// A10.1 EXTENSIBLE headers resolve to the real tag; in-place edits accept them
// ---------------------------------------------------------------------------
void TestExtensible()
{
  printf("A10.1 WAVE_FORMAT_EXTENSIBLE\n");
  struct Case { int tag, bits; Bytes data; int frames; const char* name; } cases[] = {
    { 1, 16, Pcm16({ 1000, -1000, 2000, -2000, 3000, -3000 }), 3, "ext16.wav" },
    { 1, 24, Pcm24({ 100000, -100000, 200000, -200000 }), 2, "ext24.wav" },
    { 3, 32, F32({ 0.25f, -0.25f, 0.5f, -0.5f, 0.75f, -0.75f, 1.0f, -1.0f }), 4, "ext32f.wav" },
  };
  for (const auto& c : cases) {
    const std::string path = TempPath(c.name);
    CHECK(WriteBytes(path, BuildWav(c.tag, 2, 48000, c.bits, c.data, true)), "write %s", c.name);
    WavInfo info;
    const bool ok = AudioEngine::ReadWavHeader(path, info);
    CHECK(ok, "%s: ReadWavHeader refused an EXTENSIBLE header", c.name);
    CHECK(info.audioFormat == c.tag, "%s: audioFormat %d (want %d = the SubFormat tag, not 0xFFFE)", c.name, info.audioFormat, c.tag);
    CHECK(info.bitsPerSample == c.bits && info.numChannels == 2 && info.sampleRate == 48000, "%s: %d bit %d ch %d Hz", c.name, info.bitsPerSample, info.numChannels, info.sampleRate);
    CHECK(info.numFrames == c.frames, "%s: %d frames (want %d)", c.name, info.numFrames, c.frames);
    remove(path.c_str());
  }
  // In-place Reverse on an EXTENSIBLE 24-bit file: the frames swap, nothing else moves.
  {
    const std::string path = TempPath("ext24_reverse.wav");
    const std::vector<int32_t> pcm = { 11, 12, 21, 22, 31, 32, 41, 42 };   // 4 stereo frames
    const Bytes original = BuildWav(1, 2, 44100, 24, Pcm24(pcm), true);
    CHECK(WriteBytes(path, original), "write ext24_reverse");
    CHECK(WavInplace::Reverse(path, 0, 4), "Reverse refused an EXTENSIBLE 24-bit file");
    std::vector<Chunk> cs; std::string why;
    const Bytes after = ReadBytes(path);
    CHECK(WalkRiff(after, cs, &why), "reversed file: %s", why.c_str());
    const Chunk* data = Find(cs, "data");
    CHECK(data != nullptr, "no data chunk after Reverse");
    if (data) {
      const std::vector<int32_t> got = DecodePcm24(data->payload);
      const std::vector<int32_t> want = { 41, 42, 31, 32, 21, 22, 11, 12 };
      CHECK(got == want, "Reverse did not reverse the EXTENSIBLE frames (got %d %d %d %d ...)", got.size() > 3 ? got[0] : -1, got.size() > 3 ? got[1] : -1, got.size() > 3 ? got[2] : -1, got.size() > 3 ? got[3] : -1);
    }
    CHECK(WavInplace::Reverse(path, 0, 4), "second Reverse");
    CHECK(ReadBytes(path) == original, "Reverse x2 must restore the original bytes exactly");
    remove(path.c_str());
  }
}

// ---------------------------------------------------------------------------
// A10.2 data size 0 / 0xFFFFFFFF: frames derived from the file size
// ---------------------------------------------------------------------------
void TestStreamedDataSize()
{
  printf("A10.2 data chunk size 0 / 0xFFFFFFFF\n");
  const int64_t sizes[] = { 0, (int64_t)0xFFFFFFFF };
  for (int64_t sz : sizes) {
    const std::string path = TempPath(sz == 0 ? "data0.wav" : "dataFF.wav");
    const std::vector<int16_t> pcm = { 1, 2, 3, 4, 5, 6 };   // 6 mono frames
    CHECK(WriteBytes(path, BuildWav(1, 1, 44100, 16, Pcm16(pcm), false, sz)), "write data=%lld", (long long)sz);
    WavInfo info;
    CHECK(AudioEngine::ReadWavHeader(path, info), "ReadWavHeader on data=%lld", (long long)sz);
    CHECK(info.numFrames == 6, "data=%lld: %d frames (want 6 = (file size - data offset) / 2)", (long long)sz, info.numFrames);
    CHECK(WavInplace::Reverse(path, 0, 6), "Reverse on data=%lld", (long long)sz);
    const Bytes after = ReadBytes(path);
    const std::vector<int16_t> got = DecodePcm16(Bytes(after.begin() + 44, after.end()));
    const std::vector<int16_t> want = { 6, 5, 4, 3, 2, 1 };
    CHECK(got == want, "data=%lld: Reverse reported success but the samples are unchanged", (long long)sz);
    remove(path.c_str());
  }
}

// ---------------------------------------------------------------------------
// A10.3 PCM encode = lrint + clamp: load -> save is the identity on the grid
// ---------------------------------------------------------------------------
void TestEncodeIdentity()
{
  printf("A10.3 PCM encode identity\n");
  // 16-bit: every value once, through WavWriter.
  {
    std::vector<double> in;
    std::vector<int16_t> want;
    for (int k = -32768; k <= 32767; k++) { in.push_back((double)k / 32768.0); want.push_back((int16_t)k); }
    const std::string path = TempPath("identity16.wav");
    CHECK(AudioEngine::WriteWavFile(path, in.data(), (int)in.size(), 1, 44100, 16, 1), "WriteWavFile 16-bit");
    const Bytes f = ReadBytes(path);
    std::vector<Chunk> cs; std::string why;
    CHECK(WalkRiff(f, cs, &why), "identity16: %s", why.c_str());
    const Chunk* data = Find(cs, "data");
    int bad = 0, first = 0;
    if (data) {
      const std::vector<int16_t> got = DecodePcm16(data->payload);
      CHECK(got.size() == want.size(), "identity16: %zu samples", got.size());
      for (size_t i = 0; i < got.size() && i < want.size(); i++)
        if (got[i] != want[i]) { if (!bad) first = (int)i; bad++; }
    }
    CHECK(bad == 0, "16-bit round trip changed %d of 65536 values (first: k=%d wrote %d)", bad, first - 32768,
          data ? DecodePcm16(data->payload)[(size_t)first] : 0);
    remove(path.c_str());
  }
  // 24-bit: the grid around zero, the extremes and every 4097th value.
  {
    std::vector<double> in;
    std::vector<int32_t> want;
    auto add = [&](int32_t k) { in.push_back((double)k / 8388608.0); want.push_back(k); };
    for (int32_t k = -8388608; k <= 8388607; k += 4097) add(k);
    for (int32_t k = -300; k <= 300; k++) add(k);
    add(8388607); add(-8388608);
    const std::string path = TempPath("identity24.wav");
    CHECK(AudioEngine::WriteWavFile(path, in.data(), (int)in.size(), 1, 44100, 24, 1), "WriteWavFile 24-bit");
    const Bytes f = ReadBytes(path);
    std::vector<Chunk> cs; std::string why;
    CHECK(WalkRiff(f, cs, &why), "identity24: %s", why.c_str());
    const Chunk* data = Find(cs, "data");
    int bad = 0;
    if (data) {
      const std::vector<int32_t> got = DecodePcm24(data->payload);
      CHECK(got.size() == want.size(), "identity24: %zu samples", got.size());
      for (size_t i = 0; i < got.size() && i < want.size(); i++) if (got[i] != want[i]) bad++;
    }
    CHECK(bad == 0, "24-bit round trip changed %d values", bad);
    remove(path.c_str());
  }
  // Clamp: +1.0 and beyond hit the top of the grid, -1.0 the bottom, no wrap.
  {
    const double in[] = { 1.0, 2.0, -1.0, -2.0, 0.999999 };
    const std::string path = TempPath("clamp16.wav");
    CHECK(AudioEngine::WriteWavFile(path, in, 5, 1, 44100, 16, 1), "WriteWavFile clamp");
    const Bytes f = ReadBytes(path);
    const std::vector<int16_t> got = DecodePcm16(Bytes(f.begin() + 44, f.end()));
    const std::vector<int16_t> want = { 32767, 32767, -32768, -32768, 32767 };
    CHECK(got == want, "clamp: got %d %d %d %d %d", got.size() > 4 ? got[0] : 0, got.size() > 4 ? got[1] : 0, got.size() > 4 ? got[2] : 0, got.size() > 4 ? got[3] : 0, got.size() > 4 ? got[4] : 0);
    remove(path.c_str());
  }
  // In-place Gain x1.0 must leave a 16-bit file byte-identical (same encoder).
  {
    std::vector<int16_t> pcm;
    for (int k = -32768; k <= 32767; k += 7) pcm.push_back((int16_t)k);
    const std::string path = TempPath("gain1_16.wav");
    const Bytes original = BuildWav(1, 1, 44100, 16, Pcm16(pcm), false);
    CHECK(WriteBytes(path, original), "write gain1_16");
    CHECK(WavInplace::Gain(path, 0, (int64_t)pcm.size(), 1.0, 0), "Gain 1.0");
    CHECK(ReadBytes(path) == original, "in-place Gain x1.0 changed the 16-bit samples (encoder is not the identity)");
    remove(path.c_str());
  }
  {
    std::vector<int32_t> pcm;
    for (int32_t k = -8388608; k <= 8388607; k += 997) pcm.push_back(k);
    const std::string path = TempPath("gain1_24.wav");
    const Bytes original = BuildWav(1, 1, 44100, 24, Pcm24(pcm), false);
    CHECK(WriteBytes(path, original), "write gain1_24");
    CHECK(WavInplace::Gain(path, 0, (int64_t)pcm.size(), 1.0, 0), "Gain 1.0 (24)");
    CHECK(ReadBytes(path) == original, "in-place Gain x1.0 changed the 24-bit samples (encoder is not the identity)");
    remove(path.c_str());
  }
}

// ---------------------------------------------------------------------------
// A10.5 odd data chunk + smpl: pad byte, even offsets, RIFF size = file - 8
// ---------------------------------------------------------------------------
void TestOddDataChunkWithLoop()
{
  printf("A10.5 odd data chunk + smpl\n");
  std::vector<double> in;
  for (int i = 0; i < 5; i++) in.push_back(0.1 * (double)(i + 1));   // 5 mono 24-bit frames = 15 bytes
  const std::string path = TempPath("odd24_loop.wav");
  CHECK(AudioEngine::WriteWavFile(path, in.data(), 5, 1, 44100, 24, 1, 1, 4), "WriteWavFile odd + loop");
  const Bytes f = ReadBytes(path);
  std::vector<Chunk> cs; std::string why;
  CHECK(WalkRiff(f, cs, &why), "odd data + smpl: %s", why.c_str());
  const Chunk* data = Find(cs, "data");
  const Chunk* smpl = Find(cs, "smpl");
  CHECK(data && data->payload.size() == 15, "data chunk of 15 bytes");
  CHECK(smpl != nullptr, "smpl chunk present");
  if (smpl) CHECK((smpl->offset & 1) == 0, "smpl chunk at odd offset %zu (missing pad byte after the odd data chunk)", smpl->offset);
  int s = -1, e = -1;
  CHECK(ParseWavSmplFile(path.c_str(), &s, &e) && s == 1 && e == 4, "loop read back as %d..%d (want 1..4)", s, e);
  remove(path.c_str());
}

// ---------------------------------------------------------------------------
// A10.4 the original's metadata chunks ride along on a save, verbatim
// ---------------------------------------------------------------------------
void TestCarryChunks()
{
  printf("A10.4 metadata carry-over\n");
  Chunk bext; bext.id = "bext"; bext.payload.assign(602, 0); memcpy(bext.payload.data(), "SneakPeak desc", 14);
  Chunk ixml; ixml.id = "iXML"; const char* x = "<BWFXML><IXML_VERSION>1.5</IXML_VERSION></BWFXML>"; ixml.payload.assign(x, x + strlen(x));   // odd length
  Chunk list; list.id = "LIST"; const char* l = "INFOISFT\x0a\0\0\0SneakPeak\0"; list.payload.assign(l, l + 22);
  Chunk cue;  cue.id = "cue "; Put32(cue.payload, 1); for (int i = 0; i < 6; i++) Put32(cue.payload, (uint32_t)(i * 7));
  CHECK((ixml.payload.size() & 1) == 1, "precondition: the iXML payload is odd (%zu)", ixml.payload.size());
  unsigned char smplBytes[kSmplChunkBytes];
  BuildSmplChunk(44100, 2, 5, smplBytes);
  Chunk smpl; smpl.id = "smpl"; smpl.payload.assign(smplBytes + 8, smplBytes + kSmplChunkBytes);
  const std::vector<int32_t> pcm = { 1000, 2000, 3000, 4000, 5000, 6000 };   // 6 mono frames
  const std::string orig = TempPath("bwf_original.wav");
  CHECK(WriteBytes(orig, BuildWav(1, 1, 44100, 24, Pcm24(pcm), false, -1, { bext, ixml }, { list, cue, smpl })), "write bwf original");

  std::vector<WavCarryChunk> carry;
  CHECK(AudioEngine::CollectWavCarryChunks(orig, carry), "CollectWavCarryChunks");
  CHECK(carry.size() == 4, "collected %zu chunks (want 4: bext iXML LIST cue - never fmt/data/smpl)", carry.size());
  for (const auto& c : carry)
    CHECK(memcmp(c.id, "fmt ", 4) != 0 && memcmp(c.id, "data", 4) != 0 && memcmp(c.id, "smpl", 4) != 0, "carried an owned chunk %.4s", c.id);

  // The "save": edited audio (reversed), a new loop, the carries appended.
  std::vector<double> edited;
  for (int i = 5; i >= 0; i--) edited.push_back((double)pcm[(size_t)i] / 8388608.0);
  const std::string saved = TempPath("bwf_saved.wav");
  CHECK(AudioEngine::WriteWavFile(saved, edited.data(), 6, 1, 44100, 24, 1, 1, 4, &carry), "WriteWavFile with carry");
  const Bytes f = ReadBytes(saved);
  std::vector<Chunk> cs; std::string why;
  CHECK(WalkRiff(f, cs, &why), "saved BWF: %s", why.c_str());
  for (const Chunk* want : { &bext, &ixml, &list, &cue }) {
    const Chunk* got = Find(cs, want->id.c_str());
    CHECK(got != nullptr, "chunk %s missing from the saved file", want->id.c_str());
    if (got) CHECK(got->payload == want->payload, "chunk %s not byte-identical", want->id.c_str());
  }
  const Chunk* data = Find(cs, "data");
  CHECK(data && DecodePcm24(data->payload) == std::vector<int32_t>({ 6000, 5000, 4000, 3000, 2000, 1000 }), "the audio is the edited one");
  int s = -1, e = -1;
  CHECK(ParseWavSmplFile(saved.c_str(), &s, &e) && s == 1 && e == 4, "the NEW loop is written (%d..%d), the original smpl was not copied", s, e);
  CHECK(cs.size() == 7, "%zu chunks in the saved file (want fmt data smpl bext iXML LIST cue)", cs.size());
  WavInfo info;
  CHECK(AudioEngine::ReadWavHeader(saved, info) && info.numFrames == 6 && info.bitsPerSample == 24, "saved file still parses as 6 x 24-bit");
  remove(orig.c_str());
  remove(saved.c_str());
}

// ---------------------------------------------------------------------------
// F3 (v2.5 UX audit) WavInplace::Limit: the range is limited in place, the
// rest of the file byte-identical; nothing above the ceiling = nothing written
// ---------------------------------------------------------------------------
void TestInplaceLimit()
{
  printf("F3 in-place Limit\n");
  const int rate = 44100, total = rate * 5 / 2;   // 2.5 s mono 16-bit
  std::vector<int16_t> pcm((size_t)total);
  for (int i = 0; i < total; i++) {
    const double t = (double)i / rate;
    const double amp = (t >= 1.0 && t < 1.5) ? 0.9 : 0.01;   // one loud burst at 1.0-1.5 s
    pcm[(size_t)i] = (int16_t)std::lrint(amp * sin(2.0 * 3.14159265358979 * 220.0 * t) * 32767.0);
  }
  const Bytes original = BuildWav(1, 1, rate, 16, Pcm16(pcm), false);
  const std::string path = TempPath("limit16.wav");
  CHECK(WriteBytes(path, original), "write limit16");

  LimiterParams p;           // Game Asset defaults: -1 dBTP, TP on
  p.gainDb = 6.0;            // 0.9 -> 1.8 must come down to the ceiling
  const int64_t s0 = rate / 2, s1 = rate * 2;   // 0.5..2.0 s: a window of the file, ramps apply
  const int ramp = rate / 50;                   // 20 ms
  LimiterResult res;
  std::vector<double> processed;
  CHECK(WavInplace::Limit(path, s0, s1, p, ramp, res, &processed), "Limit on a window");
  CHECK(res.ok && res.maxGainReductionDb > 5.0, "max GR %.2f dB (want > 5: +6 dB into -1 dBTP on a 0.9 burst)", res.maxGainReductionDb);
  CHECK(processed.size() == (size_t)(s1 - s0), "processed carries the range (%zu frames, want %lld)", processed.size(), (long long)(s1 - s0));
  const Bytes after = ReadBytes(path);
  CHECK(after.size() == original.size(), "file size unchanged");
  std::vector<Chunk> cs; std::string why;
  CHECK(WalkRiff(after, cs, &why), "limited file: %s", why.c_str());
  const Chunk* data = Find(cs, "data");
  CHECK(data != nullptr, "no data chunk after Limit");
  if (data) {
    const std::vector<int16_t> got = DecodePcm16(data->payload);
    bool outsideSame = true;
    int16_t peakIn = 0;
    for (int i = 0; i < total; i++) {
      if (i < s0 || i >= s1) { if (got[(size_t)i] != pcm[(size_t)i]) outsideSame = false; }
      else if (i >= s0 + ramp && i < s1 - ramp) peakIn = std::max(peakIn, (int16_t)std::abs((int)got[(size_t)i]));
    }
    CHECK(outsideSame, "samples outside the range changed");
    const double peakDb = 20.0 * log10((double)peakIn / 32768.0);
    CHECK(peakDb <= -0.95, "sample peak inside the range %.2f dBFS (ceiling -1 dBTP)", peakDb);
    CHECK(peakDb > -1.6, "sample peak inside the range %.2f dBFS: the burst was not pushed into the ceiling", peakDb);
  }

  // Nothing above the ceiling with 0 dB gain (a file at -40 dBFS): the file
  // is left alone, byte for byte.
  std::vector<int16_t> quietPcm((size_t)total);
  for (int i = 0; i < total; i++)
    quietPcm[(size_t)i] = (int16_t)std::lrint(0.01 * sin(2.0 * 3.14159265358979 * 220.0 * (double)i / rate) * 32767.0);
  const Bytes quietFile = BuildWav(1, 1, rate, 16, Pcm16(quietPcm), false);
  CHECK(WriteBytes(path, quietFile), "write quiet16");
  p.gainDb = 0.0;
  LimiterResult quiet;
  CHECK(WavInplace::Limit(path, 0, total, p, 0, quiet, nullptr), "Limit with nothing to do");
  CHECK(quiet.ok && quiet.maxGainReductionDb == 0.0, "quiet file: max GR %.3f dB (want 0)", quiet.maxGainReductionDb);
  CHECK(ReadBytes(path) == quietFile, "a no-op Limit must not touch the file");
  remove(path.c_str());
}

} // namespace

int main()
{
  TestExtensible();
  TestStreamedDataSize();
  TestEncodeIdentity();
  TestCarryChunks();
  TestOddDataChunkWithLoop();
  TestInplaceLimit();
  printf("%s: %d checks, %d failed\n", g_failed ? "FAILED" : "PASS", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
