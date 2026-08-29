// dynamics_pipeline.cpp — Dynamics analysis off the input path and off the buffer
//
// Phase 2b (v2.5): mouse-move only updates the panel + marks the params dirty;
// OnTimer runs Analyze + ComputeCompression on a worker thread with the LATEST
// params (intermediate positions are skipped) and swaps the finished engine in
// on the main thread; Live envelope writes are debounced (~150 ms after the
// last change) and flushed synchronously on mouse-up.
//
// 8f (design_dynamics_stream.md): item views no longer analyse the working
// buffer (downsampled on long items, absent after 8g). A DynTraceJob streams
// the view at full rate through AudioStream on its own thread into a DynTrace
// (the 1 ms detector lanes); the DynWorker then analyses that shared,
// immutable trace. Accessors are opened on the main thread before the job's
// thread starts and closed after the join; while the thread runs the main
// thread touches no accessor API (abort tests = load generation + take
// validity; AudioStream::Changed() once after the join). Detector-param
// changes (Peak/RMS, RMS window, de-ess mode/freq/Q) rebuild the trace;
// everything else re-runs only the trace -> results pass. Standalone keeps
// analysing its full-rate buffer (no accessor there).
#include "edit_view.h"
#include "denormals.h"
#include <algorithm>
#include <cmath>

static const DWORD LIVE_WRITE_DEBOUNCE_MS = 150;
static const int kTraceChunkFrames = 65536;

// Params the analysis should use: the panel's while it is open, else the
// engine's (the overlay curve without the panel).
const DynamicsParams& SneakPeak::CurrentDynParams() const
{
  return m_dynamicsPanel.IsVisible() ? m_dynamicsPanel.GetParams() : m_dynamics.GetParams();
}

static double ItemVolDb(const WaveformView& w)
{
  return w.IsStandaloneMode() ? 0.0 : 20.0 * log10(std::max(w.GetFadeCache().itemVol, 1e-12));
}

bool SneakPeak::DynamicsWanted() const
{
  return !m_waveform.IsStandaloneMode() && m_waveform.HasItem() && !m_waveform.IsMultiItemActive() &&
         (m_dynamicsVisible || m_dynamicsPanel.IsVisible() || m_dynApplyPending);
}

// The current trace serves these params on this view.
bool SneakPeak::DynTraceCurrent(const DynamicsParams& p) const
{
  if (!m_dynTrace || m_dynTraceGen != m_waveform.GetLoadGeneration()) return false;
  const DynTraceKey& k = m_dynTrace->key;
  return k == DynamicsEngine::TraceKeyFor(p, k.sampleRate, k.numChannels, k.numFrames);
}

void SneakPeak::StartDynTraceJob()
{
  DynTraceJob& J = m_dynTraceJob;
  AbortDynTraceJob();
  // Loader parity: the shared timeline/SET buffer bakes per-segment item
  // volume, a single item's does not (ivDb is the engine's argument).
  const bool applyItemVolume = m_waveform.GetSegments().size() > 1;
  if (!m_waveform.OpenStream(J.stream, 0.0, m_waveform.GetItemDuration(), applyItemVolume))
    return;   // no take / rate yet: the OnTimer tick retries
  J.key = DynamicsEngine::TraceKeyFor(CurrentDynParams(), J.stream.Rate(), J.stream.Channels(),
                                      J.stream.Frames());
  J.builder.Begin(J.key);
  J.generation = m_waveform.GetLoadGeneration();
  J.abort.store(false);
  J.done.store(false);
  J.framesDone.store(0);
  J.result.reset();
  J.lastPct = -1;
  J.active = true;
  J.thread = std::thread([&J]() {
    FlushDenormalsToZero();   // this thread filters long silences (denormals.h)
    std::vector<double> chunk((size_t)kTraceChunkFrames * (size_t)J.stream.Channels());
    bool ok = true;
    while (J.stream.Remaining() > 0 && !J.abort.load()) {
      const int n = (int)std::min<int64_t>(kTraceChunkFrames, J.stream.Remaining());
      if (!J.stream.Read(chunk.data(), n)) { ok = false; break; }
      J.builder.Feed(chunk.data(), n);
      J.framesDone.store(J.stream.Frames() - J.stream.Remaining());
    }
    std::shared_ptr<const DynTrace> trace = J.builder.Finish();
    if (ok && !J.abort.load() && J.stream.Remaining() == 0) J.result = std::move(trace);
    J.done.store(true);
  });
}

// MAIN THREAD: stop the job (one chunk of latency), join, close the accessors.
void SneakPeak::AbortDynTraceJob()
{
  DynTraceJob& J = m_dynTraceJob;
  if (J.thread.joinable()) {
    J.abort.store(true);
    J.thread.join();
  }
  J.stream.Close();
  J.result.reset();
  const bool retitle = J.active && J.lastPct >= 0;
  J.active = false;
  if (retitle) UpdateTitle();
}

// OnTimer: abort a job the view outgrew, land a finished trace, keep the
// title honest, and (self-healing, loader pattern) start the job any wanted
// view is missing - no call site can forget it.
void SneakPeak::StepDynTraceJob()
{
  DynTraceJob& J = m_dynTraceJob;
  const bool wanted = DynamicsWanted();
  if (J.active) {
    MediaItem_Take* take = m_waveform.GetTake();
    const bool stale = !wanted || J.generation != m_waveform.GetLoadGeneration() ||
        (take && g_ValidatePtr2 && !g_ValidatePtr2(nullptr, (void*)take, "MediaItem_Take*")) ||
        !(J.key == DynamicsEngine::TraceKeyFor(CurrentDynParams(), J.key.sampleRate,
                                               J.key.numChannels, J.key.numFrames));
    if (stale) {
      AbortDynTraceJob();
    } else if (J.done.load()) {
      J.thread.join();
      const bool changed = J.stream.Changed();   // main thread, worker finished
      std::shared_ptr<const DynTrace> result = std::move(J.result);
      J.stream.Close();
      J.active = false;
      if (J.lastPct >= 0) UpdateTitle();
      if (result && !changed) {
        m_dynTrace = std::move(result);
        m_dynTraceGen = J.generation;
        m_dynParamsDirty = true;   // the worker analyses it on this tick
      } else if (!changed) {
        m_dynTraceFailedGen = J.generation;   // accessor error: no retry loop
      }
    } else if (m_hwnd && !m_itemLoad.active && !m_exportPump.active && J.stream.Frames() > 0) {
      const int pct = (int)(100.0 * (double)J.framesDone.load() / (double)J.stream.Frames());
      if (pct != J.lastPct) {   // title writes are not free - only on change
        J.lastPct = pct;
        char title[128];
        snprintf(title, sizeof(title), "SneakPeak: Analyzing dynamics... %d%%", pct);
        SetWindowText(m_hwnd, title);
      }
    }
  }
  if (!J.active && wanted && !DynTraceCurrent(CurrentDynParams()) &&
      m_dynTraceFailedGen != m_waveform.GetLoadGeneration())
    StartDynTraceJob();
}

// One entry for "the analysis must be (re)done". Item views: mark the params
// dirty and let the pipeline deliver (trace job -> worker -> swap; Live write
// debounced; a pending Apply fires when the result lands). Standalone:
// analyse the full-rate buffer here, as every caller used to.
void SneakPeak::RequestDynamicsAnalysis()
{
  if (m_waveform.IsStandaloneMode()) {
    if (m_waveform.GetAudioSampleCount() <= 0) return;
    const DynamicsParams params = CurrentDynParams();
    m_dynamics.SetParams(params);
    m_dynamics.Analyze(m_waveform.GetAudioData().data(), m_waveform.GetAudioSampleCount(),
                       m_waveform.GetNumChannels(), m_waveform.GetSampleRate(), 0.0, params);
    m_dynamics.ComputeCompression();
    m_dynamicsPanel.SetAvgGainReduction(m_dynamics.GetAvgGainReduction());
    return;
  }
  m_dynParamsDirty = true;
  StepDynTraceJob();
}

// discardResult = the audio or the view is changing: nothing computed so far
// may survive (trace job aborted + accessors closed, trace dropped, results
// cleared). false = input-event sites: only wait for the fast worker.
void SneakPeak::JoinDynamicsWorker(bool discardResult)
{
  DynWorker& W = m_dynWorker;
  if (W.thread.joinable()) W.thread.join();
  if (!discardResult) return;
  W.hasResult.store(false);
  W.engine.Clear();
  AbortDynTraceJob();
  m_dynTrace.reset();
  m_dynamics.Clear();
  m_dynApplyPending = false;
  m_dynLiveWriteDue = 0;   // a debounced Live write of the discarded result must not fire (A7.5)
}

// Swap a finished worker engine in (main thread). Returns true when a result landed.
bool SneakPeak::TakeDynamicsResult()
{
  DynWorker& W = m_dynWorker;
  if (W.busy.load() || !W.hasResult.load()) return false;
  if (W.thread.joinable()) W.thread.join();
  W.hasResult.store(false);
  std::swap(m_dynamics, W.engine);
  m_dynamicsPanel.SetAvgGainReduction(m_dynamics.GetAvgGainReduction());
  // The operating point arrives with the result: a threshold still on the
  // provisional one follows it (and the curve is recomputed for it).
  if (m_dynamicsPanel.SetAvgPeakDb(m_dynamics.GetAveragePeakDb())) m_dynParamsDirty = true;
  if (m_dynamicsPanel.IsLive()) {
    DWORD due = GetTickCount() + (m_dynamicsPanel.IsDragging() ? LIVE_WRITE_DEBOUNCE_MS : 0);
    m_dynLiveWriteDue = due ? due : 1;
  }
  if (m_dynApplyPending && !m_dynParamsDirty) {
    m_dynApplyPending = false;
    ApplyDynamicsToEnvelope();
  }
  if (m_hwnd) Invalidate();
  return true;
}

void SneakPeak::LiveWriteEnvelope()
{
  bool alreadyOpen = m_dynamicsPanel.LiveUndoOpen();
  if (!alreadyOpen && g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
  m_dynamicsPanel.SetLiveUndoOpen(true);
  ApplyDynamicsToEnvelope();
  // A drag keeps its block open until mouse-up (one undo step per gesture);
  // outside a drag the write is its own step.
  if (!alreadyOpen && !m_dynamicsPanel.IsDragging()) {
    m_dynamicsPanel.SetLiveUndoOpen(false);
    if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, "SneakPeak: Live Dynamics", -1);
  }
}

void SneakPeak::StepDynamicsPipeline()
{
  DynWorker& W = m_dynWorker;
  TakeDynamicsResult();

  if (m_dynParamsDirty && !W.busy.load()) {
    const DynamicsParams params = CurrentDynParams();
    const bool standalone = m_waveform.IsStandaloneMode();
    // Item views wait for their trace (StepDynTraceJob re-marks dirty when it
    // lands); Standalone needs its buffer.
    const bool ready = standalone ? m_waveform.GetAudioSampleCount() > 0 : DynTraceCurrent(params);
    if (ready) {
      m_dynParamsDirty = false;
      m_dynamics.SetParams(params);   // readouts/overlays follow the knob immediately
      const std::shared_ptr<const DynTrace> trace = standalone ? nullptr : m_dynTrace;
      const double* data = m_waveform.GetAudioData().data();
      const int frames = m_waveform.GetAudioSampleCount();
      const int nch = m_waveform.GetNumChannels();
      const int sr = m_waveform.GetSampleRate();
      const double ivDb = ItemVolDb(m_waveform);
      if (W.thread.joinable()) W.thread.join();
      W.engine.SetParams(params);
      W.hasResult.store(false);
      W.busy.store(true);
      W.thread = std::thread([&W, trace, data, frames, nch, sr, ivDb, params]() {
        if (trace) W.engine.Analyze(trace, ivDb, params);
        else W.engine.Analyze(data, frames, nch, sr, ivDb, params);
        W.engine.BuildEnvelopeCurve(W.engine.ComputeCompression());   // Apply's curve, off the main thread
        W.hasResult.store(true);
        W.busy.store(false);
      });
    }
  }

  if (m_dynLiveWriteDue && !W.busy.load() && !m_dynParamsDirty &&
      (int)(GetTickCount() - m_dynLiveWriteDue) >= 0) {
    m_dynLiveWriteDue = 0;
    if (m_dynamicsPanel.IsLive() && m_dynamics.HasResults()) LiveWriteEnvelope();
  }
}

// Mouse-up: finish whatever is in flight and write the final position now
// (a view whose trace is still streaming stays dirty: the pipeline finishes
// the write when the trace lands).
void SneakPeak::FlushDynamicsPipeline()
{
  DynWorker& W = m_dynWorker;
  if (W.thread.joinable()) W.thread.join();
  TakeDynamicsResult();
  const bool standalone = m_waveform.IsStandaloneMode();
  const DynamicsParams params = CurrentDynParams();
  const bool ready = standalone ? m_waveform.GetAudioSampleCount() > 0 : DynTraceCurrent(params);
  if (m_dynParamsDirty && ready) {
    m_dynParamsDirty = false;
    m_dynamics.SetParams(params);
    const double ivDb = ItemVolDb(m_waveform);
    if (standalone)
      m_dynamics.Analyze(m_waveform.GetAudioData().data(), m_waveform.GetAudioSampleCount(),
                         m_waveform.GetNumChannels(), m_waveform.GetSampleRate(), ivDb, params);
    else
      m_dynamics.Analyze(m_dynTrace, ivDb, params);
    m_dynamics.ComputeCompression();
    m_dynamicsPanel.SetAvgGainReduction(m_dynamics.GetAvgGainReduction());
    if (m_dynamicsPanel.IsLive()) m_dynLiveWriteDue = 1;
  }
  if (m_dynLiveWriteDue && !m_dynParamsDirty) {
    m_dynLiveWriteDue = 0;
    if (m_dynamicsPanel.IsLive() && m_dynamics.HasResults()) LiveWriteEnvelope();
  }
}
