-- Debug\DebugScreen.lua, after the game's: the developers' debug race setup, opened with Y on the
-- title screen while Game Adjustments' "Debug menu" is on (GothamSystem.IsFinalBuild then
-- answers false, src/ui/pgr4_settings.cpp).
--
-- The shipped game lacks UIEnums.DebugRaceModeNames and UIEnums.DebugRaceModeIDs, which the
-- screen's Init counts: the count of nil stopped the screen and it froze. They are made here
-- from the race modes this build has, before Init runs. From the PGR4 debug menu kit ("debug menu
-- access": patches/DebugScreen_diff.txt), which injects the same into the script's bytecode.
--
-- Start a race from the car screen (RT, then A there): A on this screen sends no cars, and the
-- car spawns without its mass (the kit's TECHNICAL_DETAILS.md, section 3).

if UIEnums.DebugRaceModeNames == nil or UIEnums.DebugRaceModeIDs == nil then
  local names = {
    "STREET_RACE", "ELIMINATION", "HOTLAP", "CAT_AND_MOUSE", "SURVIVOR", "CAPTURE_THE_FLAG",
    "SUPERSTAR", "OVERTAKE", "SPEED_CHALLENGE", "TIMED_RUN", "TIME_VS_KUDOS", "KUDOS_CHALLENGE",
    "ONE_ON_ONE", "CONE_CHALLENGE", "CONE_ATTACK", "GATE_CHASE", "BREAKTHROUGH", "AUTOCROSS",
    "DRIFT_CHALLENGE", "TIMEATTACK", "TESTTRACK_TIME", "TESTTRACK_SPEED", "TESTTRACK_SKID",
    "NET_STREET_RACE", "NET_ELIMINATION", "NET_SURVIVOR", "NET_CAT_AND_MOUSE",
    "NET_CAPTURE_THE_FLAG", "NET_HOT_LAP", "NET_OVERTAKE", "NET_SPEED_CHALLENGE", "NET_AUTOCROSS",
    "NET_SUPERSTAR", "NET_BREAKTHROUGH", "NET_CONE_CHALLENGE", "NET_DRIFT_CHALLENGE",
    "NET_TOURIST", "NET_CAT_AND_MOUSE_FREE_ROAM",
  }
  local mode_names, mode_ids = {}, {}
  for _, name in ipairs(names) do
    local id = UIEnums.RaceMode[name]
    if id ~= nil then
      mode_names[#mode_names + 1] = name
      mode_ids[#mode_ids + 1] = id
    end
  end
  UIEnums.DebugRaceModeNames = mode_names
  UIEnums.DebugRaceModeIDs = mode_ids
end
