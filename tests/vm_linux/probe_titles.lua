local function title(h) local _, t = reaper.BR_Win32_GetWindowText(h) return t or "" end
local tops = {}
local w = reaper.BR_Win32_GetWindow(reaper.BR_Win32_GetMainHwnd(), 0)
local n = 0
while w and n < 40 do
  if reaper.BR_Win32_IsWindowVisible(w) then tops[#tops+1] = title(w) end
  w = reaper.BR_Win32_GetWindow(w, 2) n = n + 1
end
return tops
