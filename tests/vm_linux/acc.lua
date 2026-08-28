local tr = reaper.GetTrack(0, 0)
local acc = reaper.CreateTrackAudioAccessor(tr)
local sr, nch = 44100, 2
local function meanabs(t0, secs)
  local n = math.floor(secs * sr)
  local buf = reaper.new_array(n * nch)
  buf.clear()
  reaper.GetAudioAccessorSamples(acc, sr, nch, t0, n, buf)
  local s = 0
  local tb = buf.table()
  for i = 1, n * nch do s = s + math.abs(tb[i]) end
  return s / (n * nch)
end
local len = reaper.GetMediaItemInfo_Value(reaper.GetTrackMediaItem(tr, 0), "D_LENGTH")
local r = { head = meanabs(0.2, 1.0), tail = meanabs(len - 1.5, 1.0), len = len }
reaper.DestroyAudioAccessor(acc)
return r
