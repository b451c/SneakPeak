// audio_stream.cpp — see audio_stream.h
#include "audio_stream.h"
#include "debug.h"
#include <algorithm>

namespace {
// One accessor call covers at most this many frames (~0.3 ms on a WAV, measured
// 2026-08-28); never below 1024 except the tail (forum t=248593: erratic results
// on tiny requests).
constexpr int kAccessorCallFrames = 65536;
}

double ItemTakeVolume(MediaItem* item, MediaItem_Take* take)
{
  double vol = (item && g_GetMediaItemInfo_Value) ? g_GetMediaItemInfo_Value(item, "D_VOL") : 1.0;
  if (take && g_GetSetMediaItemTakeInfo) {
    double* pv = (double*)g_GetSetMediaItemTakeInfo(take, "D_VOL", nullptr);
    if (pv) vol *= *pv;
  }
  return vol > 0.0 ? vol : 1.0;
}

int TakeChanMode(MediaItem_Take* take)
{
  if (!take || !g_GetSetMediaItemTakeInfo) return 0;
  int* p = (int*)g_GetSetMediaItemTakeInfo(take, "I_CHANMODE", nullptr);
  return p ? *p : 0;
}

int FoldedChannels(int srcNch, int chanMode)
{
  return (srcNch == 2 && chanMode >= 2 && chanMode <= 4) ? 1 : srcNch;
}

void FoldChanMode(double* frames, int64_t n, int chanMode)
{
  for (int64_t i = 0; i < n; i++) {
    const double l = frames[i * 2], r = frames[i * 2 + 1];
    frames[i] = (chanMode == 2) ? (l + r) * 0.5 : (chanMode == 4) ? r : l;
  }
}

std::mutex& AudioStream::ApiLock()
{
  static std::mutex m;
  return m;
}

AudioStream::~AudioStream()
{
  Close();
}

bool AudioStream::Open(std::vector<AudioStreamSegment> segs, int rate, int readNch,
                       int chanMode, int64_t totalFrames)
{
  Close();
  if (rate <= 0 || readNch <= 0 || totalFrames <= 0 || !g_GetAudioAccessorSamples ||
      !g_CreateTakeAudioAccessor || !g_DestroyAudioAccessor)
    return false;
  m_segs = std::move(segs);
  for (auto& s : m_segs) {
    std::lock_guard<std::mutex> lk(ApiLock());
    s.accessor = s.take ? g_CreateTakeAudioAccessor(s.take) : nullptr;
    if (!s.accessor) DBG("[AudioStream] no accessor for take %p - silence\n", (void*)s.take);
  }
  m_rate = rate;
  m_readNch = readNch;
  m_chanMode = chanMode;
  m_outNch = FoldedChannels(readNch, chanMode);
  m_total = totalFrames;
  m_cursor = 0;
  m_open = true;
  return true;
}

bool AudioStream::Read(double* out, int frames)
{
  if (!m_open || frames <= 0) return false;
  std::fill(out, out + (size_t)frames * (size_t)m_outNch, 0.0);
  const int64_t c0 = m_cursor, c1 = m_cursor + frames;
  for (const auto& seg : m_segs) {
    const int64_t s = std::max(c0, seg.dstFrame);
    const int64_t e = std::min(c1, seg.dstFrame + seg.frames);
    if (e <= s || !seg.accessor) continue;
    const int n = (int)(e - s);
    const int srcNch = std::max(1, std::min(m_readNch, seg.srcNch));
    m_tmp.resize((size_t)n * (size_t)m_readNch);
    // Accessor reads in bounded calls, mono sources duplicated across the read
    // channels (loader parity); ret 0 = no audio there, keep the zeros.
    for (int done = 0; done < n;) {
      const int call = std::min(kAccessorCallFrames, n - done);
      const double t = seg.takeStartSec + (double)(s - seg.dstFrame + done) / (double)m_rate;
      double* dst = m_tmp.data() + (size_t)done * (size_t)m_readNch;
      int ret;
      if (srcNch < m_readNch) {
        std::vector<double> mono((size_t)call * (size_t)srcNch, 0.0);
        { std::lock_guard<std::mutex> lk(ApiLock());
          ret = g_GetAudioAccessorSamples(seg.accessor, m_rate, srcNch, t, call, mono.data()); }
        for (int f = 0; f < call; f++)
          for (int ch = 0; ch < m_readNch; ch++)
            dst[(size_t)f * m_readNch + ch] = ret > 0 ? mono[(size_t)f * srcNch] : 0.0;
      } else {
        { std::lock_guard<std::mutex> lk(ApiLock());
          ret = g_GetAudioAccessorSamples(seg.accessor, m_rate, m_readNch, t, call, dst); }
        if (ret <= 0) std::fill(dst, dst + (size_t)call * (size_t)m_readNch, 0.0);
      }
      if (ret < 0) {
        DBG("[AudioStream] accessor error at %.3f s\n", t);
        return false;
      }
      done += call;
    }
    if (seg.volume != 1.0)
      for (size_t i = 0, cnt = (size_t)n * (size_t)m_readNch; i < cnt; i++) m_tmp[i] *= seg.volume;
    if (m_outNch != m_readNch) FoldChanMode(m_tmp.data(), n, m_chanMode);
    std::copy(m_tmp.begin(), m_tmp.begin() + (size_t)n * (size_t)m_outNch,
              out + (size_t)(s - c0) * (size_t)m_outNch);
  }
  m_cursor = c1;
  return true;
}

bool AudioStream::Changed() const
{
  if (!m_open || !g_AudioAccessorStateChanged) return false;
  std::lock_guard<std::mutex> lk(ApiLock());
  for (const auto& s : m_segs)
    if (s.accessor && g_AudioAccessorStateChanged(s.accessor)) return true;
  return false;
}

void AudioStream::Close()
{
  if (g_DestroyAudioAccessor)
    for (auto& s : m_segs)
      if (s.accessor) { std::lock_guard<std::mutex> lk(ApiLock()); g_DestroyAudioAccessor(s.accessor); }
  m_segs.clear();
  m_open = false;
  m_total = m_cursor = 0;
}
