// wav_smpl.cpp — WAV `smpl` chunk build/parse (v2.4 INC-A4)
#include "wav_smpl.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

void PutU32(unsigned char* p, uint32_t v)
{
  p[0] = (unsigned char)(v & 0xFF);
  p[1] = (unsigned char)((v >> 8) & 0xFF);
  p[2] = (unsigned char)((v >> 16) & 0xFF);
  p[3] = (unsigned char)((v >> 24) & 0xFF);
}

uint32_t GetU32(const unsigned char* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

}  // namespace

void BuildSmplChunk(int sampleRate, int loopStartFrame, int loopEndFrameExcl,
                    unsigned char out[kSmplChunkBytes])
{
  memset(out, 0, kSmplChunkBytes);
  memcpy(out, "smpl", 4);
  PutU32(out + 4, 60);                      // payload: 36 header + 24 loop
  unsigned char* p = out + 8;
  PutU32(p + 0, 0);                         // Manufacturer
  PutU32(p + 4, 0);                         // Product
  PutU32(p + 8, sampleRate > 0
                    ? (uint32_t)std::lround(1e9 / (double)sampleRate)
                    : 0);                   // SamplePeriod (ns)
  PutU32(p + 12, 60);                       // MIDIUnityNote (middle C)
  PutU32(p + 16, 0);                        // MIDIPitchFraction
  PutU32(p + 20, 0);                        // SMPTEFormat
  PutU32(p + 24, 0);                        // SMPTEOffset
  PutU32(p + 28, 1);                        // NumSampleLoops
  PutU32(p + 32, 0);                        // SamplerData
  // Loop record. dwEnd is INCLUSIVE on disk: write endExcl - 1.
  PutU32(p + 36, 0);                        // CuePointID
  PutU32(p + 40, 0);                        // Type 0 = forward
  PutU32(p + 44, (uint32_t)(loopStartFrame > 0 ? loopStartFrame : 0));
  PutU32(p + 48, (uint32_t)(loopEndFrameExcl > 1 ? loopEndFrameExcl - 1 : 0));
  PutU32(p + 52, 0);                        // Fraction
  PutU32(p + 56, 0);                        // PlayCount 0 = infinite
}

bool ParseWavSmplFile(const char* path, int* loopStartFrame,
                      int* loopEndFrameExcl)
{
  if (!path || !loopStartFrame || !loopEndFrameExcl) return false;
  FILE* f = fopen(path, "rb");
  if (!f) return false;

  unsigned char hdr[12];
  if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
      memcmp(hdr + 8, "WAVE", 4) != 0) {
    fclose(f);
    return false;
  }

  bool found = false;
  unsigned char ck[8];
  while (fread(ck, 1, 8, f) == 8) {
    const uint32_t size = GetU32(ck + 4);
    if (memcmp(ck, "smpl", 4) == 0 && size >= 60) {
      unsigned char payload[60];
      if (fread(payload, 1, 60, f) == 60 && GetU32(payload + 28) >= 1) {
        const uint32_t start = GetU32(payload + 44);
        const uint32_t endIncl = GetU32(payload + 48);
        if (endIncl >= start && endIncl < 0x7FFFFFFEu) {
          *loopStartFrame = (int)start;
          *loopEndFrameExcl = (int)endIncl + 1;   // disk end is inclusive
          found = true;
        }
      }
      break;
    }
    // Skip this chunk (odd sizes are padded to even per RIFF).
    const long skip = (long)size + (long)(size & 1);
    if (fseek(f, skip, SEEK_CUR) != 0) break;
  }
  fclose(f);
  return found;
}
