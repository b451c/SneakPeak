// multi_item_lanes.cpp — Lanes (per Track) for the Multi-item view: one band per
// track, stacked like channels, each with its own vertical zoom (row 15 #2, #68).
#include "multi_item_view.h"
#include "waveform_view.h"  // for WaveformSelection
#include "reaper_plugin.h"
#include "config.h"
#include "theme.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

// IP_TRACKNUMBER: GetSetMediaTrackInfo returns the 1-based number itself (API doc), not a pointer
int MultiItemView::TrackNumber(MediaTrack* tr)
{
  if (!tr || !g_GetSetMediaTrackInfo) return 0;
  return (int)(intptr_t)g_GetSetMediaTrackInfo(tr, "IP_TRACKNUMBER", nullptr);
}

std::string MultiItemView::TrackLabel(MediaTrack* tr)
{
  char name[256] = { 0 };
  if (tr && g_GetSetMediaTrackInfo_String)
    g_GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
  if (name[0]) return name;
  int num = TrackNumber(tr);
  if (num <= 0) return "Track";
  char buf[32];
  snprintf(buf, sizeof(buf), "Track %d", num);
  return buf;
}

// Same closed form as the channel split: N lanes share the height with a
// separator between them; a lane tall enough splits a stereo view into L/R
// bands, otherwise it folds both channels into one band.
bool MultiItemView::LaneGeometry(RECT rect, int lane, RECT& outLane, int& outBands, int& outBandH) const
{
  int lanes = LaneCount();
  if (lane < 0 || lane >= lanes) return false;
  int sep = SP(CHANNEL_SEPARATOR_HEIGHT);
  int laneH = (rect.bottom - rect.top - sep * (lanes - 1)) / lanes;
  if (laneH < 1) return false;
  outLane = rect;
  outLane.top = rect.top + lane * (laneH + sep);
  outLane.bottom = outLane.top + laneH;
  outBands = (m_channels == 2 && laneH >= SP(MIN_WAVEFORM_HEIGHT)) ? 2 : 1;
  outBandH = (laneH - sep * (outBands - 1)) / outBands;
  return true;
}

int MultiItemView::LaneAtY(RECT rect, int y) const
{
  if (m_mode != MultiItemMode::LANES) return -1;
  for (int lane = 0; lane < LaneCount(); lane++) {
    RECT lr; int bands, bandH;
    if (LaneGeometry(rect, lane, lr, bands, bandH) && y >= lr.top && y < lr.bottom) return lane;
  }
  return -1;
}

void MultiItemView::ZoomLane(int lane, float factor)
{
  if (lane < 0 || lane >= LaneCount()) return;
  float z = m_laneZoom[lane] * factor;
  m_laneZoom[lane] = std::max(MIN_VERTICAL_ZOOM, std::min(MAX_VERTICAL_ZOOM, z));
}

void MultiItemView::DrawLanes(HDC hdc, RECT rect, int numChannels, double viewStart, double viewDur,
                              float verticalZoom, const WaveformSelection& selection, double gainOffset)
{
  int w = rect.right - rect.left - SP(DB_SCALE_WIDTH);
  if (w < 1) w = 1;
  int nch = numChannels;
  int lanes = LaneCount();
  if (nch < 1 || lanes < 1) return;

  bool hasSel = selection.active && selection.startTime != selection.endTime;
  int selX1 = 0, selX2 = 0;
  if (hasSel) {
    double s = std::min(selection.startTime, selection.endTime);
    double e = std::max(selection.startTime, selection.endTime);
    double pixPerSec = (viewDur > 0.0) ? (double)w / viewDur : 1.0;
    selX1 = rect.left + (int)((s - viewStart) * pixPerSec);
    selX2 = rect.left + (int)((e - viewStart) * pixPerSec);
  }

  COLORREF bgColor = g_theme.waveformBg;
  double sr = (double)m_sampleRate;
  double timePerPixel = viewDur / (double)w;
  int sep = SP(CHANNEL_SEPARATOR_HEIGHT);

  HPEN centerPen = CreatePen(PS_SOLID, 1, g_theme.centerLine);
  HFONT oldFont = (HFONT)SelectObject(hdc, g_fonts.normal11);
  SetBkMode(hdc, TRANSPARENT);

  for (int lane = 0; lane < lanes; lane++) {
    RECT lr; int bands, bandH;
    if (!LaneGeometry(rect, lane, lr, bands, bandH)) continue;
    float zoom = verticalZoom * m_laneZoom[lane];
    COLORREF base = kLayerColors[lane % kNumLayerColors];
    HPEN peakPen    = CreatePen(PS_SOLID, 1, BlendColor(base, bgColor, 0.7f));
    HPEN rmsPen     = CreatePen(PS_SOLID, 1, BlendColor(base, bgColor, 0.9f));
    HPEN peakSelPen = CreatePen(PS_SOLID, 1, BlendColor(base, g_theme.waveformSelBg, 0.7f));
    HPEN rmsSelPen  = CreatePen(PS_SOLID, 1, BlendColor(base, g_theme.waveformSelBg, 0.9f));

    for (int b = 0; b < bands; b++) {
      int bandTop = lr.top + b * (bandH + sep);
      int centerY = bandTop + bandH / 2;
      float halfH = (float)(bandH / 2) * zoom;

      HPEN oldPen = (HPEN)SelectObject(hdc, centerPen);
      MoveToEx(hdc, rect.left, centerY, nullptr);
      LineTo(hdc, rect.left + w, centerY);
      HPEN curPen = centerPen;

      for (const auto& layer : m_layers) {
        if (layer.trackColorIndex != lane || layer.peakMax.empty()) continue;
        // Visible columns: the planned length while the layer still draws from .reapeaks
        int frames = layer.audioFrameCount > 0 ? layer.audioFrameCount : layer.plannedFrames;
        double layerRelStart = (double)layer.audioStartFrame / sr;
        double layerRelEnd = layerRelStart + (double)frames / sr;
        int colStart = std::max(0, (int)((layerRelStart - viewStart) / timePerPixel));
        int colEnd = std::min(w, (int)((layerRelEnd - viewStart) / timePerPixel) + 1);

        for (int col = colStart; col < colEnd; col++) {
          // One band per channel when the lane splits, else both channels folded
          double maxVal = -1.0, minVal = 1.0, rmsVal = 0.0;
          for (int ch = 0; ch < nch; ch++) {
            if (bands == 2 && ch != b) continue;
            size_t idx = (size_t)(col * nch + ch);
            if (idx >= layer.peakMax.size()) break;
            maxVal = std::max(maxVal, layer.peakMax[idx]);
            minVal = std::min(minVal, layer.peakMin[idx]);
            rmsVal = std::max(rmsVal, layer.peakRMS[idx]);
          }
          maxVal = std::max(-1.0, std::min(1.0, maxVal * gainOffset));
          minVal = std::max(-1.0, std::min(1.0, minVal * gainOffset));
          rmsVal = std::min(1.0, rmsVal * gainOffset);

          int x = rect.left + col;
          bool inSel = hasSel && x >= selX1 && x < selX2;
          HPEN wantPen = inSel ? peakSelPen : peakPen;
          if (wantPen != curPen) { SelectObject(hdc, wantPen); curPen = wantPen; }
          int yMax = centerY - (int)(maxVal * (double)halfH);
          int yMin = centerY - (int)(minVal * (double)halfH);
          yMax = std::max(bandTop, std::min(bandTop + bandH - 1, yMax));
          yMin = std::max(bandTop, std::min(bandTop + bandH - 1, yMin));
          if (yMax > yMin) std::swap(yMax, yMin);
          MoveToEx(hdc, x, yMax, nullptr);
          LineTo(hdc, x, yMin + 1);

          wantPen = inSel ? rmsSelPen : rmsPen;
          if (wantPen != curPen) { SelectObject(hdc, wantPen); curPen = wantPen; }
          int yRmsTop = std::max(bandTop, std::min(bandTop + bandH - 1, centerY - (int)(rmsVal * (double)halfH)));
          int yRmsBot = std::max(bandTop, std::min(bandTop + bandH - 1, centerY + (int)(rmsVal * (double)halfH)));
          MoveToEx(hdc, x, yRmsTop, nullptr);
          LineTo(hdc, x, yRmsBot + 1);
        }
      }
      SelectObject(hdc, oldPen);
    }

    // Lane label in the lane colour: the track, plus its zoom when not 1:1 (the
    // only indicator on a lane too short for a dB scale)
    char label[300];
    if (fabsf(m_laneZoom[lane] - 1.0f) > 0.001f)
      snprintf(label, sizeof(label), "%s  x%.1f", m_laneNames[lane].c_str(), m_laneZoom[lane]);
    else
      snprintf(label, sizeof(label), "%s", m_laneNames[lane].c_str());
    SetTextColor(hdc, base);
    RECT tr = { lr.left + SP(4), lr.top + SP(2), lr.left + w - SP(4), lr.top + SP(16) };
    DrawTextUTF8(hdc, label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    DeleteObject(peakPen);
    DeleteObject(rmsPen);
    DeleteObject(peakSelPen);
    DeleteObject(rmsSelPen);
  }

  SelectObject(hdc, oldFont);
  DeleteObject(centerPen);
}
