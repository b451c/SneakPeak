// dynamics_pipeline.cpp — Dynamics knob-drag pipeline off the input path (phase 2b)
//
// v2.4.0 ran Analyze -> ComputeCompression -> (Live) SimplifyCurve + envelope
// writes inline on EVERY mouse-move, so on long items the slider updated at
// 3-4 Hz (profile_2026-07-09_longfile.md: ~250-400 ms per tick). Now:
//  - mouse-move only updates the panel + marks the params dirty;
//  - OnTimer runs Analyze + ComputeCompression on a worker thread with the
//    LATEST params (intermediate positions are skipped) and swaps the finished
//    engine in on the main thread - the engine is pure computation, no REAPER
//    API, and the sample buffer is immutable while a job runs (see
//    JoinDynamicsWorker at every buffer-changing entry);
//  - Live envelope writes are debounced (~150 ms after the last change) and
//    flushed synchronously on mouse-up, so the written points are identical
//    to the old per-tick path for the final knob position.
#include "edit_view.h"
#include <algorithm>
#include <cmath>

static const DWORD LIVE_WRITE_DEBOUNCE_MS = 150;

void SneakPeak::JoinDynamicsWorker(bool discardResult)
{
  DynWorker& W = m_dynWorker;
  if (W.thread.joinable()) W.thread.join();
  if (discardResult) W.hasResult.store(false);
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
  if (m_dynamicsPanel.IsLive()) {
    DWORD due = GetTickCount() + (m_dynamicsPanel.IsDragging() ? LIVE_WRITE_DEBOUNCE_MS : 0);
    m_dynLiveWriteDue = due ? due : 1;
  }
  if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
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

  if (m_dynParamsDirty && !W.busy.load() && m_waveform.GetAudioSampleCount() > 0) {
    m_dynParamsDirty = false;
    const DynamicsParams params = m_dynamicsPanel.GetParams();
    m_dynamics.SetParams(params);   // readouts/overlays follow the knob immediately
    const double* data = m_waveform.GetAudioData().data();
    const int frames = m_waveform.GetAudioSampleCount();
    const int nch = m_waveform.GetNumChannels();
    const int sr = m_waveform.GetSampleRate();
    const double ivDb = m_waveform.IsStandaloneMode()
        ? 0.0 : 20.0 * log10(std::max(m_waveform.GetFadeCache().itemVol, 1e-12));
    if (W.thread.joinable()) W.thread.join();
    W.engine.SetParams(params);
    W.hasResult.store(false);
    W.busy.store(true);
    W.thread = std::thread([&W, data, frames, nch, sr, ivDb, params]() {
      W.engine.Analyze(data, frames, nch, sr, ivDb, params);
      W.engine.ComputeCompression();
      W.hasResult.store(true);
      W.busy.store(false);
    });
  }

  if (m_dynLiveWriteDue && !W.busy.load() && !m_dynParamsDirty &&
      (int)(GetTickCount() - m_dynLiveWriteDue) >= 0) {
    m_dynLiveWriteDue = 0;
    if (m_dynamicsPanel.IsLive() && m_dynamics.HasResults()) LiveWriteEnvelope();
  }
}

// Mouse-up: finish whatever is in flight and write the final position now.
void SneakPeak::FlushDynamicsPipeline()
{
  DynWorker& W = m_dynWorker;
  if (W.thread.joinable()) W.thread.join();
  TakeDynamicsResult();
  if (m_dynParamsDirty && m_waveform.GetAudioSampleCount() > 0) {
    m_dynParamsDirty = false;
    const DynamicsParams params = m_dynamicsPanel.GetParams();
    m_dynamics.SetParams(params);
    const double ivDb = m_waveform.IsStandaloneMode()
        ? 0.0 : 20.0 * log10(std::max(m_waveform.GetFadeCache().itemVol, 1e-12));
    m_dynamics.Analyze(m_waveform.GetAudioData().data(), m_waveform.GetAudioSampleCount(),
                       m_waveform.GetNumChannels(), m_waveform.GetSampleRate(), ivDb, params);
    m_dynamics.ComputeCompression();
    m_dynamicsPanel.SetAvgGainReduction(m_dynamics.GetAvgGainReduction());
    if (m_dynamicsPanel.IsLive()) m_dynLiveWriteDue = 1;
  }
  if (m_dynLiveWriteDue) {
    m_dynLiveWriteDue = 0;
    if (m_dynamicsPanel.IsLive() && m_dynamics.HasResults()) LiveWriteEnvelope();
  }
}
