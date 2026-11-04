-- Options\MainMenu.lua, after the game's: the port's settings, Game Adjustments, before Game
-- Credits. It opens Pgr4\Settings.lua (Settings.lua) with its page.
--
-- Ported from pgr4-native (games/pgr4/ui/patches/Screens/Options/MainMenu.fnl).

local adjustments = 100
local pages = {[adjustments] = "adjustments"}
local credits = 7 -- the game's item id of Game Credits

local game_init = Init
local game_message_update = MessageUpdate

-- The game's Init with the items added before the credits' as its own are: an item, its help
-- line, its sticker.
function Init()
  local add_item = UIButtons.AddItem
  UIButtons.AddItem = function(menu, id, text, stored)
    if menu == GUI.menu_ID and id == credits then
      add_item(menu, adjustments, Pgr4.Text("GAME ADJUSTMENTS"), false)
      UIButtons.AddItemHelp(menu, Pgr4.Text("Display, graphics, controller and racing wheel settings"))
      GUI.sticker_types[adjustments] = UIEnums.Stickers.OP_GRAPHICS
    end
    return add_item(menu, id, text, stored)
  end
  local ok, message = pcall(game_init)
  UIButtons.AddItem = add_item
  if not ok then
    error(message, 0)
  end
end

function MessageUpdate(message, ...)
  local page = message == "BUTTON_NEXT" and not GUI.finished and
               pages[UIButtons.GetSelection(GUI.menu_ID)]
  if page then
    GUI.finished = true
    PlayMenuNext()
    StoreMenuSelection(GUI.menu_ID)
    GothamSystem.SetMusicState(22)
    Pgr4.page = page
    Pgr4.page_from = "options"
    UIScreen.SetNextScreen("Pgr4\\Settings.lua")
    return false
  end
  return game_message_update(message, ...)
end
