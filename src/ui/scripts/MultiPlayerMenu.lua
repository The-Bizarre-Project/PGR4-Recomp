-- Intro\MultiPlayerMenu.lua, after the game's: split screen for 3 and 4 players.
--
-- The game's MessageUpdate starts split screen only when Profile.NumActive() is exactly 2
-- (TOO_FEW_PROFILES otherwise), and adds the two players to the lobby with
-- Lobby.AddPrimaryPlayer and Lobby.AddCoopPlayer. With 3 or 4 signed-in profiles it runs here
-- with NumActive answering 2, and after its co-op player each further active profile is added
-- the same way (Pgr4.SetCoopProfile points AddCoopPlayer at it).

local game_message_update = MessageUpdate

function MessageUpdate(message, ...)
  if message ~= "BUTTON_NEXT" or GUI.finished or Profile.NumActive() <= 2 or
     UIButtons.GetSelection(GUI.menu_ID) ~= 0 then
    return game_message_update(message, ...)
  end
  local num_active = Profile.NumActive
  local add_coop = Lobby.AddCoopPlayer
  Profile.NumActive = function()
    return 2
  end
  Lobby.AddCoopPlayer = function(...)
    add_coop(...)
    for _, profile in ipairs(Pgr4.ExtraLocalProfiles()) do
      local previous = Pgr4.SetCoopProfile(profile)
      add_coop()
      Pgr4.SetCoopProfile(previous)
    end
  end
  local ok, err = pcall(game_message_update, message, ...)
  Profile.NumActive = num_active
  Lobby.AddCoopPlayer = add_coop
  if not ok then
    error(err)
  end
end

-- The game's UpdateButtons enables "Play on this console" (item 0) only for exactly 2 profiles.
local game_update_buttons = UpdateButtons

function UpdateButtons(...)
  game_update_buttons(...)
  local n = Profile.NumActive()
  if n == 3 or n == 4 then
    UIButtons.SetMenuItemActive(GUI.menu_ID, 0, true)
  end
end
