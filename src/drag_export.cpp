// ============================================================================
// drag_export.cpp — Drag & drop WAV export for SneakPeak
//
// Drag files/selections from SneakPeak to REAPER timeline or desktop.
// Handles clean files (drag original), dirty files (auto-save first),
// and selection exports (temp WAV with fade baking).
//
// Part of the SneakPeak class — methods defined here, class in edit_view.h.
// ============================================================================

#include "edit_view.h"
#include "audio_engine.h"
#include "audio_ops.h"
#include "debug.h"
#include "reaper_plugin.h"

#include <cmath>
#include <algorithm>

// --- Drag & Drop Export ---

void SneakPeak::CleanupDragTemp()
{
  if (!m_dragTempPath.empty() && !m_dragIsOriginal) {
    remove(m_dragTempPath.c_str());
  }
  m_dragTempPath.clear();
  m_dragIsOriginal = false;
}

void SneakPeak::InitiateDragExport()
{
  if (!m_waveform.HasItem()) return;
  CleanupDragTemp();

  bool isStandalone = m_waveform.IsStandaloneMode();
  bool hasSelection = m_waveform.HasSelection();

  // Standalone full-file drag (no selection)
  if (isStandalone && !hasSelection) {
    if (!m_dirty && !m_waveform.HasStandaloneFade()) {
      // Clean file — drag saved path or original
      std::string dragPath = m_savedPath.empty()
          ? m_waveform.GetStandaloneFilePath() : m_savedPath;
      if (!dragPath.empty()) {
        m_dragTempPath = dragPath;
        m_dragIsOriginal = true;
        DBG("[SneakPeak] DragExport: clean file: %s\n", dragPath.c_str());
      }
    } else {
      // Dirty — auto-save first, then drag the saved file
      SaveStandaloneFile();
      if (!m_dirty && !m_savedPath.empty()) {
        m_dragTempPath = m_savedPath;
        m_dragIsOriginal = true;
        DBG("[SneakPeak] DragExport: auto-saved: %s\n", m_savedPath.c_str());
      }
    }
  }

  // Selection export (standalone or REAPER) — create temp WAV
  if (m_dragTempPath.empty()) {
    int startF, endF;
    if (hasSelection) {
      GetSelectionSampleRange(startF, endF);
    } else {
      startF = 0;
      endF = m_waveform.GetAudioSampleCount();
    }
    int nch = m_waveform.GetNumChannels();
    int sr = m_waveform.GetSampleRate();
    int selFrames = endF - startF;
    if (selFrames <= 0 || nch <= 0) return;

    const auto& data = m_waveform.GetAudioData();
    size_t offset = (size_t)startF * (size_t)nch;
    size_t needed = offset + (size_t)selFrames * (size_t)nch;
    if (needed > data.size()) return;

    std::vector<double> exportBuf(data.begin() + offset,
                                   data.begin() + offset + (size_t)selFrames * nch);

    // Bake fades into export copy
    if (isStandalone) {
      // Standalone: use pending fade params
      auto sf = m_waveform.GetStandaloneFade();
      int totalFrames = m_waveform.GetAudioSampleCount();
      if (sf.fadeInLen >= 0.001) {
        int fadeFrames = std::min((int)(sf.fadeInLen * sr), totalFrames);
        if (startF < fadeFrames) {
          int overlap = std::min(fadeFrames - startF, selFrames);
          for (int i = 0; i < overlap; i++) {
            double t = (double)(startF + i) / (double)fadeFrames;
            double gain = ApplyFadeShape(t, sf.fadeInShape, -sf.fadeInDir);
            for (int ch = 0; ch < nch; ch++)
              exportBuf[i * nch + ch] *= gain;
          }
        }
      }
      if (sf.fadeOutLen >= 0.001) {
        int fadeFrames = std::min((int)(sf.fadeOutLen * sr), totalFrames);
        int fadeStart = totalFrames - fadeFrames;
        int overlapStart = std::max(startF, fadeStart);
        int overlapEnd = std::min(endF, totalFrames);
        for (int i = overlapStart; i < overlapEnd; i++) {
          double t = (double)(i - fadeStart) / (double)fadeFrames;
          double gain = ApplyFadeShape(1.0 - t, sf.fadeOutShape, sf.fadeOutDir);
          int bufIdx = i - startF;
          for (int ch = 0; ch < nch; ch++)
            exportBuf[bufIdx * nch + ch] *= gain;
        }
      }
    } else if (g_GetMediaItemInfo_Value) {
      // REAPER mode: bake item fades from REAPER metadata
      // D_VOL is already baked into m_audioData at load time.
      // Fades are visual-only overlays - must bake here for export.
      const auto& segs = m_waveform.GetSegments();
      if (segs.empty() && m_waveform.GetItem()) {
        // Single item - apply its fades to the full export range
        MediaItem* item = m_waveform.GetItem();
        double fadeInLen = g_GetMediaItemInfo_Value(item, "D_FADEINLEN");
        double fadeInDir = g_GetMediaItemInfo_Value(item, "D_FADEINDIR");
        double fadeOutLen = g_GetMediaItemInfo_Value(item, "D_FADEOUTLEN");
        double fadeOutDir = g_GetMediaItemInfo_Value(item, "D_FADEOUTDIR");
        int totalFrames = m_waveform.GetAudioSampleCount();

        if (fadeInLen >= 0.001) {
          int fadeFrames = std::min((int)(fadeInLen * sr), totalFrames);
          if (startF < fadeFrames) {
            int overlap = std::min(fadeFrames - startF, selFrames);
            for (int i = 0; i < overlap; i++) {
              double t = (double)(startF + i) / (double)fadeFrames;
              double gain = ApplyFadeShape(t, 0, -fadeInDir);
              for (int ch = 0; ch < nch; ch++)
                exportBuf[i * nch + ch] *= gain;
            }
          }
        }
        if (fadeOutLen >= 0.001) {
          int fadeFrames = std::min((int)(fadeOutLen * sr), totalFrames);
          int fadeStart = totalFrames - fadeFrames;
          int overlapStart = std::max(startF, fadeStart);
          int overlapEnd = std::min(endF, totalFrames);
          for (int i = overlapStart; i < overlapEnd; i++) {
            double t = (double)(i - fadeStart) / (double)fadeFrames;
            double gain = ApplyFadeShape(1.0 - t, 0, fadeOutDir);
            int bufIdx = i - startF;
            for (int ch = 0; ch < nch; ch++)
              exportBuf[bufIdx * nch + ch] *= gain;
          }
        }
      } else {
        // Timeline/SET: apply per-segment fades
        for (const auto& seg : segs) {
          if (!seg.item) continue;
          double fadeInLen = g_GetMediaItemInfo_Value(seg.item, "D_FADEINLEN");
          double fadeInDir = g_GetMediaItemInfo_Value(seg.item, "D_FADEINDIR");
          double fadeOutLen = g_GetMediaItemInfo_Value(seg.item, "D_FADEOUTLEN");
          double fadeOutDir = g_GetMediaItemInfo_Value(seg.item, "D_FADEOUTDIR");

          // Segment frame range in buffer
          int segStart = seg.audioStartFrame;
          int segEnd = segStart + seg.audioFrameCount;

          // Fade-in for this segment
          if (fadeInLen >= 0.001) {
            int fadeFrames = std::min((int)(fadeInLen * sr), seg.audioFrameCount);
            int applyStart = std::max(startF, segStart);
            int applyEnd = std::min(endF, segStart + fadeFrames);
            for (int i = applyStart; i < applyEnd; i++) {
              double t = (double)(i - segStart) / (double)fadeFrames;
              double gain = ApplyFadeShape(t, 0, -fadeInDir);
              int bufIdx = i - startF;
              for (int ch = 0; ch < nch; ch++)
                exportBuf[bufIdx * nch + ch] *= gain;
            }
          }
          // Fade-out for this segment
          if (fadeOutLen >= 0.001) {
            int fadeFrames = std::min((int)(fadeOutLen * sr), seg.audioFrameCount);
            int fadeStart = segEnd - fadeFrames;
            int applyStart = std::max(startF, fadeStart);
            int applyEnd = std::min(endF, segEnd);
            for (int i = applyStart; i < applyEnd; i++) {
              double t = (double)(i - fadeStart) / (double)fadeFrames;
              double gain = ApplyFadeShape(1.0 - t, 0, fadeOutDir);
              int bufIdx = i - startF;
              for (int ch = 0; ch < nch; ch++)
                exportBuf[bufIdx * nch + ch] *= gain;
            }
          }
        }
      }
    }

    const char* srcPath = isStandalone
        ? m_waveform.GetStandaloneFilePath().c_str() : nullptr;
    m_dragTempPath = AudioEngine::WriteExportWav(exportBuf.data(), selFrames, nch,
                                                  sr, m_wavBitsPerSample, m_wavAudioFormat,
                                                  srcPath);
    if (m_dragTempPath.empty()) return;
    DBG("[SneakPeak] DragExport: temp WAV: %s\n", m_dragTempPath.c_str());
  }

  // Initiate drag
#ifndef _WIN32
  RECT dragRect = { m_dragStartX - 5, m_dragStartY - 5,
                    m_dragStartX + 5, m_dragStartY + 5 };
  const char* files[] = { m_dragTempPath.c_str() };
  SWELL_InitiateDragDropOfFileList(m_hwnd, &dragRect, files, 1, nullptr);
#endif
}
