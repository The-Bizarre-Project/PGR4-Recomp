-- Intro\StartScreen.lua, after the game's: up to 4 signed-in profiles may press Start.
--
-- The game's MessageUpdate stops at TOO_MANY_PROFILES_TO_PLAY_PGR4 when Profile.NumProfiles()
-- is above 2. While it runs, NumProfiles answers at most 2; Profile.SetupGamerProfiles, which it
-- then calls, sets up all four profiles anyway (the 3- and 4-player split screen,
-- MultiPlayerMenu.lua).

local game_message_update = MessageUpdate

function MessageUpdate(...)
  local num_profiles = Profile.NumProfiles
  Profile.NumProfiles = function(...)
    return math.min(num_profiles(...), 2)
  end
  local ok, err = pcall(game_message_update, ...)
  Profile.NumProfiles = num_profiles
  if not ok then
    error(err)
  end
end
