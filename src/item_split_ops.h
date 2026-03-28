// item_split_ops.h - Split-at-selection-edges + D_VOL application helpers
// Extracts the common pattern used in gain-with-selection across all view modes.
#pragma once

#include "globals.h"
#include <vector>

struct SplitGainParams {
  double absStart = 0.0;      // absolute timeline start of selection
  double absEnd = 0.0;        // absolute timeline end of selection
  double gainFactor = 1.0;    // linear gain multiplier (pow(10, dB/20))
  double edgeEps = 0.001;     // tolerance for edge alignment detection
  double crossfadeSec = 0.0;  // crossfade overlap at split points (0 = none)
};

// Find overlapping items on track, split at selection edges, apply D_VOL.
// Returns list of target items that received the gain change.
std::vector<MediaItem*> SplitAndApplyGain(MediaTrack* track, const SplitGainParams& p);

// Single-item variant: splits one item at selection edges, applies D_VOL.
// Returns the target item (the selection portion), or nullptr on failure.
MediaItem* SplitAndApplyGainSingle(MediaItem* item, const SplitGainParams& p);
