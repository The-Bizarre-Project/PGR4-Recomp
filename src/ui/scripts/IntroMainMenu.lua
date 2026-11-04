-- Intro\MainMenu.lua, after the game's: Multiplayer with 3 or 4 signed-in profiles.
--
-- The game's MessageUpdate opens Multiplayer (selection 1) only while Profile.NumActive() is at
-- most 2 (TOO_MANY_PROFILES otherwise). For that entry NumActive answers at most 2 while it runs;
-- MultiPlayerMenu.lua then takes the 3rd and 4th players into split screen. The single-player
-- entries keep their own checks.

local game_message_update = MessageUpdate

function MessageUpdate(message, ...)
  if message ~= "BUTTON_NEXT" or GUI.finished or UIButtons.GetSelection(GUI.menu_ID) ~= 1 then
    return game_message_update(message, ...)
  end
  local num_active = Profile.NumActive
  Profile.NumActive = function(...)
    return math.min(num_active(...), 2)
  end
  local ok, err = pcall(game_message_update, message, ...)
  Profile.NumActive = num_active
  if not ok then
    error(err)
  end
end
