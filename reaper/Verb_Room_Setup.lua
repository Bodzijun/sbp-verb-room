-- Verb Room: create (or reuse) the "Room" track and send the selected tracks into it.
-- Each selected track becomes one mono send into its own input channel of the Room track:
-- track 1 -> input 1, track 2 -> input 2, ... which matches the plugin's default "Add source"
-- order (each new source takes the next free input channel). Dry signal stays on the track's
-- normal output; mute the track's master/parent send yourself if you want only the room.
--
-- Usage: select the tracks that carry the sounds, run this action. Running it again adds
-- sends for newly selected tracks and leaves existing ones alone.

local ROOM_NAME = "Room"
local FX_NAME = "Verb Room"

local function find_room()
  for i = 0, reaper.CountTracks(0) - 1 do
    local tr = reaper.GetTrack(0, i)
    local _, name = reaper.GetSetMediaTrackInfo_String(tr, "P_NAME", "", false)
    if name == ROOM_NAME then return tr end
  end
  return nil
end

local function has_send(src, dst)
  for s = 0, reaper.GetTrackNumSends(src, 0) - 1 do
    if reaper.GetTrackSendInfo_Value(src, 0, s, "P_DESTTRACK") == dst then return true end
  end
  return false
end

reaper.Undo_BeginBlock()
reaper.PreventUIRefresh(1)

local selected = {}
for i = 0, reaper.CountSelectedTracks(0) - 1 do
  local tr = reaper.GetSelectedTrack(0, i)
  selected[#selected + 1] = tr
end

local room = find_room()
if room then
  -- never send the room into itself
  for i = #selected, 1, -1 do
    if selected[i] == room then table.remove(selected, i) end
  end
else
  local idx = reaper.CountTracks(0)
  reaper.InsertTrackAtIndex(idx, true)
  room = reaper.GetTrack(0, idx)
  reaper.GetSetMediaTrackInfo_String(room, "P_NAME", ROOM_NAME, true)
  if reaper.TrackFX_AddByName(room, FX_NAME, false, -1) < 0 then
    reaper.ShowMessageBox("The plugin \"" .. FX_NAME .. "\" was not found. Rescan the VST folders in REAPER (Preferences > Plug-ins > VST).", "Verb Room", 0)
  end
end

-- the room track must carry as many channels as there are sources (even number, at least 2)
local nch = math.max(2, #selected)
if nch % 2 == 1 then nch = nch + 1 end
if nch > 64 then nch = 64 end
if reaper.GetMediaTrackInfo_Value(room, "I_NCHAN") < nch then
  reaper.SetMediaTrackInfo_Value(room, "I_NCHAN", nch)
end

-- first free input channel: after the ones already fed by sends
local used = 0
for i = 0, reaper.CountTracks(0) - 1 do
  local tr = reaper.GetTrack(0, i)
  for s = 0, reaper.GetTrackNumSends(tr, 0) - 1 do
    if reaper.GetTrackSendInfo_Value(tr, 0, s, "P_DESTTRACK") == room then
      local dst = reaper.GetTrackSendInfo_Value(tr, 0, s, "I_DSTCHAN")
      used = math.max(used, (math.floor(dst) % 1024) + 1)
    end
  end
end

local added = 0
for _, tr in ipairs(selected) do
  if not has_send(tr, room) then
    if used + 1 > reaper.GetMediaTrackInfo_Value(room, "I_NCHAN") then
      reaper.SetMediaTrackInfo_Value(room, "I_NCHAN", math.min(64, used + 2))
    end
    local s = reaper.CreateTrackSend(tr, room)
    reaper.SetTrackSendInfo_Value(tr, 0, s, "I_SRCCHAN", 1024)       -- mono: first channel of the track
    reaper.SetTrackSendInfo_Value(tr, 0, s, "I_DSTCHAN", 1024 + used) -- mono into the next room input
    used = used + 1
    added = added + 1
  end
end

reaper.PreventUIRefresh(-1)
reaper.TrackList_AdjustWindows(false)
reaper.UpdateArrange()
reaper.Undo_EndBlock("Verb Room: set up sends", -1)

reaper.ShowMessageBox(
  added .. " send(s) created into \"" .. ROOM_NAME .. "\" (inputs 1-" .. used .. ").\n\n" ..
  "In the plugin, press \"Add source\" until there is one source per send (each takes the next input channel), " ..
  "and give them the Mono format.", "Verb Room", 0)
