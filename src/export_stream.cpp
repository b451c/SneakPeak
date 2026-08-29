// export_stream.cpp — exports that read the item at FULL rate through
// AudioStream instead of the working buffer (v2.5 8e, finding F11: long items
// are held downsampled, so every buffer-fed export wrote 22050/8000 Hz files).
// Edit Copy is an OnTimer pump (STA-1/loader pattern): ~15 ms of read+encode
// per tick, progress in the title, aborted by any item change.
#include "edit_view.h"
#include "audio_ops.h"
#include "debug.h"
#include <algorithm>
#include <cmath>

static const int kExportChunkFrames = 65536;

void SneakPeak::StartEditCopyExport(const std::string& outPath)
{
  ExportPump& P = m_exportPump;
  AbortExportPump();
  if (!m_waveform.OpenStream(P.stream, 0.0, m_waveform.GetItemDuration(), true)) {
    ShowToast("Edit Copy needs a readable item");
    return;
  }
  // 32-bit float: lossless for what the accessor delivers (as the buffer path was).
  if (!P.writer.Begin(outPath, P.stream.Channels(), P.stream.Rate(), 32, 3)) {
    P.stream.Close();
    ShowToast("Write failed - check the source folder permissions");
    return;
  }
  P.outPath = outPath;
  P.generation = m_waveform.GetLoadGeneration();
  P.lastPct = -1;
  P.active = true;
}

void SneakPeak::StepExportPump()
{
  ExportPump& P = m_exportPump;
  if (!P.active) return;
  if (m_waveform.IsStandaloneMode() || P.generation != m_waveform.GetLoadGeneration() ||
      P.stream.Changed() ||
      (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, (void*)m_waveform.GetTake(), "MediaItem_Take*"))) {
    AbortExportPump();
    ShowToast("Edit Copy cancelled - the item changed");
    return;
  }

  const DWORD t0 = GetTickCount();
  P.chunk.resize((size_t)kExportChunkFrames * (size_t)P.stream.Channels());
  while (P.stream.Remaining() > 0 && GetTickCount() - t0 < 15) {
    const int n = (int)std::min<int64_t>(kExportChunkFrames, P.stream.Remaining());
    if (!P.stream.Read(P.chunk.data(), n) || !P.writer.Write(P.chunk.data(), n)) {
      AbortExportPump();
      ShowToast("Edit Copy failed - could not read or write the audio");
      return;
    }
  }
  if (P.stream.Remaining() > 0) {
    const int pct = (int)(100.0 * (double)P.writer.Frames() / (double)P.stream.Frames());
    if (pct != P.lastPct && m_hwnd) {   // title writes are not free - only on change
      P.lastPct = pct;
      char title[128];
      snprintf(title, sizeof(title), "SneakPeak: Edit Copy... %d%%", pct);
      SetWindowText(m_hwnd, title);
    }
    return;
  }

  const std::string path = P.outPath;
  const bool ok = P.writer.End();
  P.stream.Close();
  P.active = false;
  UpdateTitle();
  if (!ok) {
    ShowToast("Write failed - check the source folder permissions");
    return;
  }
  const size_t slash = path.find_last_of("/\\");
  char buf[160];
  snprintf(buf, sizeof(buf), "Edit copy: %s",
           slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1);
  ShowToast(buf);
  AddStandaloneFile(path.c_str());
}

void SneakPeak::AbortExportPump()
{
  ExportPump& P = m_exportPump;
  if (!P.active) return;
  P.writer.Abort();   // tmp removed, the destination never existed
  P.stream.Close();
  P.active = false;
  if (m_hwnd) UpdateTitle();
}

// Item fades are visual overlays in ITEM modes (D_VOL is what the stream
// bakes); an export must bake them. Same formulas as the buffer path had:
// linear shape, curvature from D_FADEINDIR/D_FADEOUTDIR, per segment in
// timeline/SET. viewFrame0 = the chunk's first frame in view time at `sr`.
void SneakPeak::BakeItemFades(double* chunk, int64_t viewFrame0, int n, int nch, int sr) const
{
  if (!g_GetMediaItemInfo_Value) return;
  const int64_t c0 = viewFrame0, c1 = viewFrame0 + n;
  auto scale = [&](int64_t i, double gain) {
    double* f = chunk + (size_t)(i - c0) * (size_t)nch;
    for (int ch = 0; ch < nch; ch++) f[ch] *= gain;
  };
  auto bake = [&](MediaItem* item, int64_t segStart, int64_t segEnd) {
    const double fadeInLen = g_GetMediaItemInfo_Value(item, "D_FADEINLEN");
    const double fadeInDir = g_GetMediaItemInfo_Value(item, "D_FADEINDIR");
    const double fadeOutLen = g_GetMediaItemInfo_Value(item, "D_FADEOUTLEN");
    const double fadeOutDir = g_GetMediaItemInfo_Value(item, "D_FADEOUTDIR");
    const int64_t segFrames = segEnd - segStart;
    if (fadeInLen >= 0.001) {
      const int64_t fadeFrames = std::min((int64_t)(fadeInLen * sr), segFrames);
      for (int64_t i = std::max(c0, segStart), e = std::min(c1, segStart + fadeFrames); i < e; i++)
        scale(i, ApplyFadeShape((double)(i - segStart) / (double)fadeFrames, 0, -fadeInDir));
    }
    if (fadeOutLen >= 0.001) {
      const int64_t fadeFrames = std::min((int64_t)(fadeOutLen * sr), segFrames);
      const int64_t fadeStart = segEnd - fadeFrames;
      for (int64_t i = std::max(c0, fadeStart), e = std::min(c1, segEnd); i < e; i++)
        scale(i, ApplyFadeShape(1.0 - (double)(i - fadeStart) / (double)fadeFrames, 0, fadeOutDir));
    }
  };
  const auto& segs = m_waveform.GetSegments();
  if (segs.empty()) {
    if (m_waveform.GetItem())
      bake(m_waveform.GetItem(), 0, (int64_t)std::llround(m_waveform.GetItemDuration() * sr));
    return;
  }
  for (const auto& seg : segs) {
    if (!seg.item) continue;
    const int64_t segStart = (int64_t)std::llround(seg.relativeOffset * sr);
    bake(seg.item, segStart, segStart + (int64_t)std::llround(seg.duration * sr));
  }
}

// Synchronous: the OS drag needs the gesture in flight, and the accessor
// streams a 5-min WAV window in ~60 ms (design 2; AAC ~0.9 s - progress in
// the title). Format = the source's own (24-bit PCM for non-WAV sources),
// never the standalone-tracked m_wavBitsPerSample. Empty on failure.
std::string SneakPeak::ExportItemRangeToWav(double t0, double t1)
{
  if (DestructiveJobBusy()) return {};   // F5: the file is being rewritten
  AudioStream stream;
  if (!m_waveform.OpenStream(stream, t0, t1, true)) return {};
  MediaItem_Take* take = m_waveform.GetTake();
  const auto& segs = m_waveform.GetSegments();
  if (!take && !segs.empty()) take = segs[0].take;
  int bits = 24, fmt = 1;
  WavInfo info;
  if (take && AudioEngine::ReadWavHeader(AudioEngine::GetSourceFilePath(take), info) &&
      ((info.audioFormat == 3 && info.bitsPerSample == 32) ||
       (info.audioFormat == 1 && (info.bitsPerSample == 16 || info.bitsPerSample == 24)))) {
    bits = info.bitsPerSample;
    fmt = info.audioFormat;
  }
  // RIFF holds 4 GB: refuse a longer selection up front, like Copy does (A10.5).
  const int64_t bytesPerFrame = (int64_t)(bits / 8) * stream.Channels();
  if (stream.Frames() * bytesPerFrame > (int64_t)UINT32_MAX - 4096) {
    const int maxMin = (int)(((int64_t)UINT32_MAX - 4096) / bytesPerFrame / stream.Rate() / 60);
    char msg[128];
    snprintf(msg, sizeof(msg), "Selection too long to export as WAV (about %d min max at this rate)", maxMin);
    ShowToast(msg);
    return {};
  }
  const std::string path = AudioEngine::ExportWavPath(nullptr);
  WavWriter writer;
  if (!writer.Begin(path, stream.Channels(), stream.Rate(), bits, fmt)) return {};

  const int64_t viewFrame0 = (int64_t)std::llround(t0 * stream.Rate());
  std::vector<double> chunk((size_t)kExportChunkFrames * (size_t)stream.Channels());
  int lastPct = -1;
  while (stream.Remaining() > 0) {
    const int n = (int)std::min<int64_t>(kExportChunkFrames, stream.Remaining());
    const int64_t at = viewFrame0 + writer.Frames();
    if (!stream.Read(chunk.data(), n)) return {};
    BakeItemFades(chunk.data(), at, n, stream.Channels(), stream.Rate());
    if (!writer.Write(chunk.data(), n)) return {};
    const int pct = (int)(100.0 * (double)writer.Frames() / (double)stream.Frames());
    if (pct != lastPct && m_hwnd && stream.Remaining() > 0) {
      lastPct = pct;
      char title[128];
      snprintf(title, sizeof(title), "SneakPeak: Exporting... %d%%", pct);
      SetWindowText(m_hwnd, title);
    }
  }
  if (lastPct >= 0) UpdateTitle();
  if (!writer.End()) return {};
  DBG("[SneakPeak] ExportItemRangeToWav: %s (%.3f-%.3f s, %d-bit)\n", path.c_str(), t0, t1, bits);
  return path;
}

bool SneakPeak::SliceSamples(double t0, double t1, std::vector<double>& out, int* nch, int* sr)
{
  out.clear();
  if (m_waveform.IsStandaloneMode()) {   // full-rate buffer, no accessor to stream
    const auto& data = m_waveform.GetAudioData();
    const int n = m_waveform.GetNumChannels(), rate = m_waveform.GetSampleRate();
    const int s0 = std::max(0, (int)(t0 * rate + 0.5));
    const int s1 = std::min(m_waveform.GetAudioSampleCount(), (int)(t1 * rate + 0.5));
    if (n <= 0 || rate <= 0 || s1 <= s0) return false;
    out.assign(data.begin() + (size_t)s0 * n, data.begin() + (size_t)s1 * n);
    *nch = n; *sr = rate;
    return true;
  }
  AudioStream stream;
  if (!m_waveform.OpenStream(stream, t0, t1, true)) return false;
  out.resize((size_t)stream.Frames() * (size_t)stream.Channels());
  double* dst = out.data();
  while (stream.Remaining() > 0) {
    const int n = (int)std::min<int64_t>(kExportChunkFrames, stream.Remaining());
    if (!stream.Read(dst, n)) { out.clear(); return false; }
    dst += (size_t)n * (size_t)stream.Channels();
  }
  *nch = stream.Channels(); *sr = stream.Rate();
  return true;
}
