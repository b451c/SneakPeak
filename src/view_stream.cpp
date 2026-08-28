// view_stream.cpp — WaveformView::OpenStream: the view's segment table for an
// AudioStream (design_streaming_source.md 4.1). Single item = one segment at the
// view's source rate; timeline/SET = one per ItemSegment overlapping the range,
// silence in the gaps; multi-item = not a stream client (8e).
#include "waveform_view.h"
#include "audio_stream.h"
#include <algorithm>
#include <cmath>

bool WaveformView::OpenStream(AudioStream& stream, double t0, double t1,
                              bool applyItemVolume) const
{
  if (m_standaloneMode || m_multiItemActive || m_sourceRate <= 0) return false;
  t0 = std::max(0.0, t0);
  t1 = std::min(m_itemDuration, t1);
  const int64_t total = (int64_t)std::llround((t1 - t0) * m_sourceRate);
  if (total <= 0) return false;
  auto valid = [](MediaItem_Take* take) {
    return take && (!g_ValidatePtr2 || g_ValidatePtr2(nullptr, (void*)take, "MediaItem_Take*"));
  };

  std::vector<AudioStreamSegment> segs;
  int readNch = std::max(1, m_srcChannels), chanMode = 0;
  if (m_segments.size() > 1) {
    readNch = std::max(1, m_numChannels);   // shared-buffer layout (loader parity)
    for (const auto& seg : m_segments) {
      const double s = std::max(t0, seg.relativeOffset);
      const double e = std::min(t1, seg.relativeOffset + seg.duration);
      if (e <= s || !valid(seg.take)) continue;
      AudioStreamSegment j;
      j.item = seg.item; j.take = seg.take;
      j.takeStartSec = s - seg.relativeOffset;
      j.dstFrame = (int64_t)std::llround((s - t0) * m_sourceRate);
      j.frames = (int64_t)std::llround((e - t0) * m_sourceRate) - j.dstFrame;
      j.srcNch = readNch;
      j.volume = applyItemVolume ? ItemTakeVolume(seg.item, seg.take) : 1.0;
      if (j.frames > 0) segs.push_back(j);
    }
  } else {
    if (!valid(m_take)) return false;
    AudioStreamSegment j;
    j.item = m_item; j.take = m_take;
    j.takeStartSec = t0;
    j.frames = total;
    j.srcNch = readNch;
    j.volume = applyItemVolume ? ItemTakeVolume(m_item, m_take) : 1.0;
    segs.push_back(j);
    chanMode = TakeChanMode(m_take);
  }
  return stream.Open(std::move(segs), m_sourceRate, readNch, chanMode, total);
}
