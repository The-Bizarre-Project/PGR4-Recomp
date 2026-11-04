-- Screens\GUIBank.lua, after the game's: the HUD's viewport rectangles for 3 players to match
-- the port's 3-player layout (four_players.cpp: player 1 across the top half, players 2 and 3
-- side by side below), where the game's are quadrants with the bottom-right one empty.

local game_get_viewport_details = GetViewportDetails

function GetViewportDetails(viewport, ...)
  local details = game_get_viewport_details(viewport, ...)
  if Pgr4.Split3Wide() and GothamSystem.GetNumberOfViewports() == 3 and details then
    local s = UIGlobals.screen
    if viewport == 0 then
      details.topLeftX, details.topLeftY, details.sizeX, details.sizeY = s.left, s.top, s.width, s.height / 2
    elseif viewport == 1 then
      details.topLeftX, details.topLeftY, details.sizeX, details.sizeY = s.left, s.cy, s.width / 2, s.height / 2
    elseif viewport == 2 then
      details.topLeftX, details.topLeftY, details.sizeX, details.sizeY = s.cx, s.cy, s.width / 2, s.height / 2
    end
  end
  return details
end
