// ============================================================================
// destructive_job.cpp — destructive ITEM edits on a worker (v2.5 F5)
//
// Reverse / Gain (selection) / DC Remove rewrite the source file in place
// (wav_inplace.cpp). They used to run on the main thread after the confirm:
// a 20-min Reverse froze REAPER for tens of seconds, and the pre-edit file
// copy alone costs ~0.25 s per 200 MB. Everything file-sized now runs on one
// worker: the snapshot copy, the op, and the rollback copy on failure or
// cancel. The main thread opens the write (BeginDestructiveWrite, incl. the
// Windows offline bracket), shows progress in the title from OnTimer and
// finalizes on the tick the worker finishes: undo bookkeeping, the display
// edit, REAPER's refresh + undo point (FinishDestructiveWrite). The view is
// pinned to the item until then (LoadSelectedItem returns early) and every
// command that reads or rewrites the file refuses through DestructiveJobBusy.
// Design: .harness/design_f5_background_destructive.md
//
// Part of the SneakPeak class — methods defined here, class in edit_view.h.
// ============================================================================

#include "edit_view.h"
#include "audio_engine.h"
#include "wav_inplace.h"
#include "debug.h"
#include "reaper_plugin.h"

#include <cstdio>

bool SneakPeak::DestructiveJobBusy()
{
  if (!m_destructiveJob.active) return false;
  char msg[96];
  snprintf(msg, sizeof(msg), "%s in progress - wait or press Esc", m_destructiveJob.verb.c_str());
  ShowToast(msg);
  return true;
}

void SneakPeak::StartDestructiveJob(const char* verb, const char* doing, const char* undoLabel,
                                    DestructiveOp op, std::function<void()> displayEdit)
{
  DestructiveJob& J = m_destructiveJob;
  if (J.active) return;   // the command refused through DestructiveJobBusy before the confirm
  std::string path;
  if (!BeginDestructiveWrite(path)) return;
  J.path = path;
  J.snapshot = UndoSnapshotPath();
  GetSelectionSourceRange(J.s0, J.s1);
  J.verb = verb;
  J.doing = doing;
  J.undoLabel = undoLabel;
  J.op = std::move(op);
  J.displayEdit = std::move(displayEdit);
  J.selCount = g_CountSelectedMediaItems ? g_CountSelectedMediaItems(nullptr) : 0;
  J.selItem = J.selCount > 0 && g_GetSelectedMediaItem ? g_GetSelectedMediaItem(nullptr, 0) : nullptr;
  J.cancel.store(false);
  J.done.store(false);
  J.phase.store(0);
  J.pct.store(0);
  J.snapshotOk = J.ok = J.restored = false;
  J.lastTitle.clear();
  J.active = true;
  J.thread = std::thread(&SneakPeak::DestructiveJobThread, this);
}

// Worker: pure file I/O, no REAPER calls.
void SneakPeak::DestructiveJobThread()
{
  DestructiveJob& J = m_destructiveJob;
  J.snapshotOk = AudioEngine::CopyFileInto(J.path, J.snapshot);
  if (J.snapshotOk) {
    J.phase.store(1);
    WavInplace::Progress prog;
    prog.user = &J;
    prog.fn = [](void* user, double frac) -> bool {
      DestructiveJob* j = (DestructiveJob*)user;
      j->pct.store((int)(frac * 100.0 + 0.5));
      return !j->cancel.load();
    };
    J.ok = J.op(J.path, J.s0, J.s1, &prog);
    // Rollback (audit A1.4) on the worker too: a failed or cancelled edit
    // leaves the chunks already written; the snapshot goes back over them.
    if (!J.ok) J.restored = AudioEngine::CopyFileInto(J.snapshot, J.path);
  }
  J.done.store(true);
}

// OnTimer: progress in the title while the worker runs; finalize on completion.
void SneakPeak::StepDestructiveJob()
{
  DestructiveJob& J = m_destructiveJob;
  if (!J.active) return;
  if (!J.done.load()) {
    if (!m_hwnd) return;
    char title[128];
    if (J.phase.load() == 0)
      snprintf(title, sizeof(title), "SneakPeak: %s... saving the pre-edit copy", J.doing.c_str());
    else
      snprintf(title, sizeof(title), "SneakPeak: %s... %d%% (Esc cancels)", J.doing.c_str(), J.pct.load());
    if (J.lastTitle != title) {   // title writes are not free - only on change
      J.lastTitle = title;
      SetWindowText(m_hwnd, title);
    }
    return;
  }
  J.thread.join();
  J.active = false;
  FinalizeDestructiveJob();
}

// Main thread, after the join: what the synchronous path did around the op.
void SneakPeak::FinalizeDestructiveJob()
{
  DestructiveJob& J = m_destructiveJob;
  DBG("[SneakPeak] destructive job %s: snapshot=%d ok=%d restored=%d cancelled=%d\n",
      J.verb.c_str(), J.snapshotOk ? 1 : 0, J.ok ? 1 : 0, J.restored ? 1 : 0, J.cancel.load() ? 1 : 0);
  if (!J.snapshotOk) {
    // No pre-edit copy = no edit (A1.3): the file was never touched.
    AudioEngine::RemoveFile(J.snapshot);   // whatever a partial copy left behind
    BringOfflineItemsBackOnline();         // Windows: the items BeginDestructiveWrite took offline
    m_waveform.RecreateLiveAccessor();
    UpdateTitle();
    char msg[sizeof(m_toastText)];
    snprintf(msg, sizeof(msg), "Could not create the pre-edit copy in %s - the edit was cancelled",
             AudioEngine::TempDir().c_str());
    ShowToast(msg);
    return;
  }
  // The copy exists: the bookkeeping UndoSave did (the old snapshot is dropped
  // only now, the slot flips, the restore checks the path).
  DiscardItemUndo();
  m_itemUndoSlot ^= 1;
  m_itemUndoPath = J.path;
  m_itemUndoFile = J.snapshot;
  m_hasUndo = true;
  if (J.ok) {
    if (g_PreventUIRefresh) g_PreventUIRefresh(1);
    if (g_Undo_BeginBlock2) g_Undo_BeginBlock2(nullptr);
    if (J.displayEdit) J.displayEdit();
    FinishDestructiveWrite(true, false);
    if (g_Undo_EndBlock2) g_Undo_EndBlock2(nullptr, J.undoLabel.c_str(), -1);
    if (g_PreventUIRefresh) g_PreventUIRefresh(-1);
    Invalidate();
  } else {
    FinishDestructiveWrite(false, J.restored);
    if (J.restored && J.cancel.load()) {
      char msg[sizeof(m_toastText)];
      snprintf(msg, sizeof(msg), "%s cancelled - the file was restored from the pre-edit copy", J.verb.c_str());
      ShowToast(msg);
    }
  }
  // The view was pinned: if REAPER's selection moved meanwhile, main.cpp's
  // poll has already recorded it and will not fire again - follow it now.
  if (g_CountSelectedMediaItems && g_GetSelectedMediaItem) {
    const int n = g_CountSelectedMediaItems(nullptr);
    MediaItem* sel = n > 0 ? g_GetSelectedMediaItem(nullptr, 0) : nullptr;
    if (n != J.selCount || sel != J.selItem) LoadSelectedItem();
  }
}

// Window close: stop the worker (it rolls the file back), then finalize like
// a tick would - an edit that had just landed still gets its refresh.
void SneakPeak::AbortDestructiveJob()
{
  DestructiveJob& J = m_destructiveJob;
  if (!J.active) return;
  J.cancel.store(true);
  if (J.thread.joinable()) J.thread.join();
  J.active = false;
  FinalizeDestructiveJob();
}
