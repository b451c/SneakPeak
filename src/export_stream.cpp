// export_stream.cpp — exports that read the item at FULL rate through
// AudioStream instead of the working buffer (v2.5 8e, finding F11: long items
// are held downsampled, so every buffer-fed export wrote 22050/8000 Hz files).
// Edit Copy is an OnTimer pump (STA-1/loader pattern): ~15 ms of read+encode
// per tick, progress in the title, aborted by any item change.
#include "edit_view.h"
#include "debug.h"
#include <algorithm>

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
