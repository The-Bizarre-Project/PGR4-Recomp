-- Pgr4\Settings.lua: a page of the port's settings (Pgr4.Page, src/ui/pgr4_settings.cpp), made
-- as the game's own option screens are (Options\General.lua, and the scrolling of
-- ContentServer\ReplaySearchVars.lua): a spinner per setting in a frame, up and down to choose
-- one, left and right to change it, A to apply and save the page, B to leave it unchanged. The
-- page is Pgr4.page and the menu that opened the screen Pgr4.page_from, which it goes back to:
-- the Options menu (OptionsMainMenu.lua), or the pause menu (PauseMenu.lua), over whose race it
-- is drawn as Ingame\IngameOptions.lua is.
--
-- Ported from pgr4-native (games/pgr4/ui/Screens/Pgr4/Settings.fnl), written there in Fennel;
-- here in plain Lua, which the game's Lua 5.1 compiles when it loads the screen.

local titles = {adjustments = "GAME ADJUSTMENTS"}
local menus = {options = "Options\\MainMenu.lua", pause = "Ingame\\PauseMenu.lua"}

-- Seconds the frame takes to slide in over a race (SetupIngameSlideGroup).
local slide_seconds = 0.4

-- Where a hidden row is put: inactive, a spinner still draws its label, but the frame clips it.
local parked_y = 10000

GUI = {finished = false, started = false, optionChosen = 0, offset = 0, target_offset = 0,
       max_offset = 0,
       scroll_group = {type = UIEnums.ButtonTypes.GROUPSTART, group_type = UIEnums.GroupType.OFFSET,
                       x = 0, y = 0}}

local function shown(option)
  local condition = option.shown_with
  if condition == nil then
    return true
  end
  local by = GUI.by_id[condition.id]
  return shown(by) and condition.choices[UIButtons.GetSelection(by.ID)] == true
end

-- Marks the chosen row and scrolls it into the frame's upper half where the page allows. Every
-- row is unmarked before the chosen one is marked, as General.lua's ActivateOptions does.
local function activate_options()
  for _, option in ipairs(GUI.options) do
    UIButtons.SetSelected(option.ID, false)
  end
  -- Over a race the frame slides in and out, but the chosen row's red bar does not slide with
  -- it (the clip stays at the frame's place): no row is marked while it slides.
  if GUI.sliding then
    return
  end
  UIButtons.SetSelected(GUI.rows[GUI.optionChosen + 1].ID, true)
  local offset = 100 - UIGlobals.options_cr * GUI.optionChosen
  GUI.target_offset = math.max(-GUI.max_offset, math.min(0, offset))
end

-- Shows the rows whose conditions hold, one below the other, and keeps the chosen one chosen.
local function lay_out()
  local chosen = GUI.rows[GUI.optionChosen + 1]
  GUI.rows = {}
  for _, option in ipairs(GUI.options) do
    local is_shown = shown(option)
    UIButtons.SetActive(option.ID, is_shown)
    UIButtons.ChangePosition(option.ID, GUI.x,
                             is_shown and (GUI.top + #GUI.rows * UIGlobals.options_cr) or parked_y, 0)
    if is_shown then
      if option == chosen then
        GUI.optionChosen = #GUI.rows
      end
      GUI.rows[#GUI.rows + 1] = option
    end
  end
  GUI.optionChosen = math.min(GUI.optionChosen, #GUI.rows - 1)
  GUI.max_offset = math.max(0, UIGlobals.options_cr * #GUI.rows - GUI.visible)
  for _, id in ipairs(GUI.scroll_bar) do
    UIButtons.SetActive(id, GUI.max_offset > 0)
  end
  for _, option in ipairs(GUI.options) do
    option.shown_selection = UIButtons.GetSelection(option.ID)
  end
  activate_options()
end

-- Whether a row another depends on shows another choice than at the last lay-out.
local function conditions_changed()
  for _, option in ipairs(GUI.options) do
    if option.controls and option.shown_selection ~= UIButtons.GetSelection(option.ID) then
      return true
    end
  end
  return false
end

local function unmark_rows()
  GUI.sliding = GUI.ingame
  for _, option in ipairs(GUI.options) do
    UIButtons.SetSelected(option.ID, false)
  end
end

local function leave()
  unmark_rows()
  UIScreen.SetNextScreen(menus[GUI.from])
end

-- A start-up option takes its choice at the next start (the rows' " *"): the game's own popup
-- (Popups\TheCustomPopup.lua) says so, and A on it goes back to the menu.
local function leave_for_restart()
  CustomPopup.Data.pgr4_restart = {
    darken = true,
    title_text_ID = Pgr4.Text("RESTART REQUIRED"),
    message_text_ID = Pgr4.Text("The choices marked * take effect the next time the game starts."),
    next_text_ID = UIText.IDS_CONTINUE_A,
    no_back = true,
    next_screen = menus[GUI.from],
  }
  unmark_rows()
  SetupCustomPopup("pgr4_restart")
end

function Init()
  GUI.from = Pgr4.page_from or "options"
  GUI.ingame = GUI.from == "pause"
  GUI.sliding = GUI.ingame
  GUI.slide_time = 0
  -- The Options menu's fade panel, which its rows fade with; a race has none.
  GUI.panel = (not GUI.ingame) and 1 or nil
  if not GUI.ingame then
    SetupFadePanel()
  end
  SetupScreenTimers()
  SetupInfoText(UIText.IDS_CANCEL_B_SAVE_A, 1, false)
  SetupSlideMenuInfo()
  if GUI.ingame then
    SetupFadedBackground(UIEnums.Layer.BACKGROUND)
    SetupIngameSlideGroup()
  end
  GUI.page = Pgr4.page or "adjustments"
  GUI.options = Pgr4.Page(GUI.page)
  GUI.by_id = {}
  for _, option in ipairs(GUI.options) do
    GUI.by_id[option.id] = option
  end
  for _, option in ipairs(GUI.options) do
    if option.shown_with then
      GUI.by_id[option.shown_with.id].controls = true
    end
  end
  GUI.rows = {}
  local frame = GetDefaultFrame(113.33333333333334, UIGlobals.screen.safe_top, 453.3333333333333,
                                UIGlobals.screen.safe_bottom - 30,
                                GUI.ingame and GUIBank.df_alternate_style_pu or nil)
  local width = 320
  local rows = UIGlobals.options_cr * #GUI.options
  frame.panel = GUI.panel
  if GUI.ingame then
    frame.background_layer = UIEnums.Layer.LAYER_1
    frame.foreground_layer = UIEnums.Layer.LAYER_2
  end
  UIButtons.AddButton(frame)
  SetupFrameTitle(frame, Pgr4.Text(titles[GUI.page] or GUI.page))
  GUI.x = 350 - width * 0.5 - 16
  GUI.top = frame.y + UIGlobals.frame_mainbox_offset_y + 12
  GUI.visible = frame.height - UIGlobals.frame_mainbox_offset_y - 28
  -- Sized for every row; hidden while the rows shown fit the frame.
  if rows > GUI.visible then
    GUI.scroll_bar = SetupFrameScrollBar(frame, true, (GUI.visible / rows) * GUI.visible, 0,
                                         UIEnums.Layer.LAYER_2)
  else
    GUI.scroll_bar = {}
  end
  -- Clipped to the frame, over a race too: a clip the UI's width (pgr4-native's, so the rows
  -- slide with the frame) showed the chosen row's red bars past the frame's sides.
  SetClipToFrame(GUIBank.variable_spinner, frame)
  GUI.scroll_group.ID = UIButtons.AddButton(GUI.scroll_group)
  GUIBank.spinner_y = UIGlobals.screen.safe_top + 12
  GUIBank.spinner_cr = 40
  for row, option in ipairs(GUI.options) do
    option.ID = SetupVariableSpinner(option.label, GUI.top + (row - 1) * UIGlobals.options_cr, width,
                                     GUI.x, GUI.panel)
    -- The spinner lists its items last added first.
    for index = #option.choices, 1, -1 do
      UIButtons.AddItem(option.ID, index - 1, option.choices[index], false)
    end
    UIButtons.SetSelection(option.ID, option.value)
  end
  SetupGroupEnd()
  if GUI.ingame then
    SetupGroupEnd()
  end
  local spinner = GUIBank.variable_spinner
  spinner.clipx = nil
  spinner.clipsizex = nil
  spinner.clipy = nil
  spinner.clipsizey = nil
  lay_out()
end

function StartLoop(_delta)
  return true
end

-- A move between rows is the screen's own (true), as in ReplaySearchVars.lua.
function MessageUpdate(message)
  if GUI.finished then
    return nil
  end
  if message == "BUTTON_UP" then
    PlayMenuMove()
    GUI.optionChosen = GUI.optionChosen > 0 and GUI.optionChosen - 1 or #GUI.rows - 1
    activate_options()
    return true
  elseif message == "BUTTON_DOWN" then
    PlayMenuMove()
    GUI.optionChosen = GUI.optionChosen < #GUI.rows - 1 and GUI.optionChosen + 1 or 0
    activate_options()
    return true
  elseif message == "BUTTON_NEXT" then
    GUI.finished = true
    PlayMenuNext()
    local changed = {}
    for _, option in ipairs(GUI.options) do
      local selection = UIButtons.GetSelection(option.ID)
      if selection ~= option.value then
        changed[option.id] = selection
      end
    end
    if Pgr4.Apply(GUI.page, changed) then
      leave_for_restart()
    else
      leave()
    end
    return false
  elseif message == "BUTTON_BACK" then
    GUI.finished = true
    PlayMenuBack()
    leave()
    return false
  end
  return false
end

-- The UI marks a row of its own once Init has run, so the chosen one is marked again on the
-- first frame. The rows follow the choices they depend on, and the frame eases to the chosen
-- row's offset, as ReplaySearchVars' does.
function FrameUpdate(delta)
  if GUI.ingame and not GUI.finished and GothamSystem.MustLeavePauseMode() then
    GUI.finished = true
    leave()
  end
  if GUI.finished then
    return
  end
  if GUI.sliding then
    GUI.slide_time = GUI.slide_time + delta
    if GUI.slide_time >= slide_seconds then
      GUI.sliding = false
      activate_options()
    end
  end
  if not GUI.started then
    GUI.started = true
    activate_options()
  end
  if conditions_changed() then
    lay_out()
  end
  if GUI.offset ~= GUI.target_offset then
    GUI.offset = GUI.offset - (GUI.offset - GUI.target_offset) * math.min(1, delta * 4)
    if math.abs(GUI.offset - GUI.target_offset) < 0.5 then
      GUI.offset = GUI.target_offset
    end
    UIButtons.ChangePosition(GUI.scroll_group.ID, 0, GUI.offset)
    if GUI.max_offset > 0 then
      MoveScrollBarV(-GUI.offset / GUI.max_offset)
    end
  end
end

function Render()
end

function End()
end

function EndLoop(_delta)
  return true
end
