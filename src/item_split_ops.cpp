// item_split_ops.cpp - Split-at-selection-edges + D_VOL application
#include "item_split_ops.h"
#include "config.h"
#include <cmath>
#include <algorithm>

std::vector<MediaItem*> SplitAndApplyGain(MediaTrack* track, const SplitGainParams& p)
{
  std::vector<MediaItem*> targets;
  if (!track || !g_GetTrackNumMediaItems || !g_GetTrackMediaItem) return targets;
  if (!g_GetMediaItemInfo_Value || !g_SetMediaItemInfo_Value || !g_SplitMediaItem) return targets;

  double eps = p.edgeEps;
  int count = g_GetTrackNumMediaItems(track);

  // Collect items overlapping [absStart, absEnd]
  std::vector<MediaItem*> overlap;
  for (int i = 0; i < count; i++) {
    MediaItem* mi = g_GetTrackMediaItem(track, i);
    if (!mi) continue;
    if (g_ValidatePtr2 && !g_ValidatePtr2(nullptr, mi, "MediaItem*")) continue;
    double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
    double len = g_GetMediaItemInfo_Value(mi, "D_LENGTH");
    if (pos + len > p.absStart && pos < p.absEnd)
      overlap.push_back(mi);
  }

  for (MediaItem* mi : overlap) {
    double pos = g_GetMediaItemInfo_Value(mi, "D_POSITION");
    double end = pos + g_GetMediaItemInfo_Value(mi, "D_LENGTH");

    MediaItem* target = mi;
    bool didSplitStart = false, didSplitEnd = false;

    if (pos >= p.absStart - eps && end <= p.absEnd + eps) {
      target = mi; // fully inside selection
    } else if (pos < p.absStart - eps && end > p.absEnd + eps) {
      g_SplitMediaItem(mi, p.absEnd); didSplitEnd = true;
      target = g_SplitMediaItem(mi, p.absStart); didSplitStart = true;
    } else if (pos < p.absStart - eps) {
      target = g_SplitMediaItem(mi, p.absStart); didSplitStart = true;
    } else {
      g_SplitMediaItem(mi, p.absEnd); didSplitEnd = true;
      target = mi;
    }

    if (!target) continue;

    // Apply gain
    double v = g_GetMediaItemInfo_Value(target, "D_VOL");
    g_SetMediaItemInfo_Value(target, "D_VOL", v * p.gainFactor);

    // Crossfade overlap at fresh split points (SET mode only)
    if (p.crossfadeSec > 0.0) {
      double tpos = g_GetMediaItemInfo_Value(target, "D_POSITION");
      double tlen = g_GetMediaItemInfo_Value(target, "D_LENGTH");
      double xf = std::min(p.crossfadeSec, tlen * 0.2);
      MediaItem_Take* take = g_GetActiveTake ? g_GetActiveTake(target) : nullptr;

      if (didSplitStart && take && g_GetSetMediaItemTakeInfo && xf > 0.0) {
        double* pOff = (double*)g_GetSetMediaItemTakeInfo(take, "D_STARTOFFS", nullptr);
        double soff = pOff ? *pOff : 0.0;
        double lxf = std::min(xf, soff);
        if (lxf > 0.0) {
          double newOff = soff - lxf;
          g_GetSetMediaItemTakeInfo(take, "D_STARTOFFS", &newOff);
          g_SetMediaItemInfo_Value(target, "D_POSITION", tpos - lxf);
          g_SetMediaItemInfo_Value(target, "D_LENGTH", tlen + lxf);
        }
      }
      if (didSplitEnd) {
        double curLen = g_GetMediaItemInfo_Value(target, "D_LENGTH");
        g_SetMediaItemInfo_Value(target, "D_LENGTH", curLen + xf);
      }
    }

    targets.push_back(target);
  }
  return targets;
}

MediaItem* SplitAndApplyGainSingle(MediaItem* item, const SplitGainParams& p)
{
  if (!item || !g_GetMediaItemInfo_Value || !g_SetMediaItemInfo_Value || !g_SplitMediaItem) return nullptr;

  double eps = p.edgeEps;
  double itemPos = g_GetMediaItemInfo_Value(item, "D_POSITION");
  double itemEnd = itemPos + g_GetMediaItemInfo_Value(item, "D_LENGTH");
  bool startAtEdge = std::abs(p.absStart - itemPos) < eps;
  bool endAtEdge = std::abs(p.absEnd - itemEnd) < eps;

  MediaItem* target = item;
  if (startAtEdge && endAtEdge) {
    // Selection = entire item, no split needed
  } else if (startAtEdge) {
    g_SplitMediaItem(item, p.absEnd);
  } else if (endAtEdge) {
    target = g_SplitMediaItem(item, p.absStart);
  } else {
    g_SplitMediaItem(item, p.absEnd);
    target = g_SplitMediaItem(item, p.absStart);
  }

  if (target) {
    double v = g_GetMediaItemInfo_Value(target, "D_VOL");
    g_SetMediaItemInfo_Value(target, "D_VOL", v * p.gainFactor);
  }
  return target;
}
