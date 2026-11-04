-- Ingame\PauseMenu.lua, after the game's: the port's settings, Game Adjustments, after Sound in
-- each of the menu's lists, as the Options menu has it (OptionsMainMenu.lua). It opens
-- Pgr4\Settings.lua, which comes back here.
--
-- Ported from pgr4-native (games/pgr4/ui/patches/Screens/InGame/PauseMenu.fnl).

local pages = {{op = 100, page = "adjustments", text = "GAME ADJUSTMENTS"}}

local by_op = {}
for _, p in ipairs(pages) do
  by_op[p.op] = p.page
end

-- The lists the game's Init chooses from by race mode and build; the frame and the menu are
-- sized from the list chosen.
for _, name in ipairs({"timeAttackUI", "TournamentQualifyUI", "defaultUI",
                       "defaultUIwithoutPhotoMode", "careerUI", "networkUI"}) do
  local items = GUI[name]
  if items then
    local sound = nil
    for at, item in ipairs(items) do
      if item.op == GUI.OP_SOUND then
        sound = at
      end
    end
    if sound then
      for i, p in ipairs(pages) do
        table.insert(items, sound + i, {op = p.op, text = Pgr4.Text(p.text)})
      end
    end
    GUI.right_menu.num_rows = math.max(GUI.right_menu.num_rows, #items)
  end
end

local game_message_update = MessageUpdate

function MessageUpdate(message, ...)
  local page = message == "BUTTON_NEXT" and not GUI.finished and
               by_op[UIButtons.GetSelection(GUI.pause_menu_ID)]
  if page then
    GUI.finished = true
    PlayMenuNext()
    Pgr4.page = page
    Pgr4.page_from = "pause"
    UIScreen.SetNextScreen("Pgr4\\Settings.lua")
    return false
  end
  return game_message_update(message, ...)
end
