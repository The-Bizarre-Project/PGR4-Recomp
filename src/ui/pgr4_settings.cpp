// The port's settings in the game's Options and pause menus (src/ui/scripts/: Settings.lua,
// OptionsMainMenu.lua, PauseMenu.lua): each a spinner of choices over the cvars that give it.
// A setting that can change while the game runs is applied at once; one taken at its start
// (marked " *") waits for the next one. Both are saved into the configuration file beside the
// executable, pgr4_recompiled.toml, which the runtime loads at start.
//
// One page, Game Adjustments ("adjustments"), whose first row picks the section shown:
//   Display        HDR output and FXAA (video.cpp), field of view (fov.cpp) and the frame rate limit
//                  (renderer.cpp)
//   Controller buttons  what each controller button gives (hid_pad_buttons)
//   Wheel          racing wheel axes and pedals (rexglue-sdk src/input/sdl/wheel_input.cpp)
//   Wheel buttons  which wheel button gives each pad button
//   Developer      the game's debug menu (Y on the title screen)
//
// After pgr4-native's games/pgr4/src/pgr4_settings.cpp, with its pages replaced.
#include "ui/pgr4_lua.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, split3_wide);
REXCVAR_DEFINE_BOOL(game_debug_menus, false, "PGR4",
                    "The game's debug menu: Y on the title screen opens the developers' race setup "
                    "(GothamSystem.IsFinalBuild answers false, and menus that test it show their "
                    "debug entries)");

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pgr4::settings {
namespace {

struct Choice {
  std::string label;
  std::string value;
};

// One cvar's value for a choice; `text` values are TOML strings in the configuration file.
struct Assignment {
  std::string cvar;
  std::string value;
  bool text = false;
};
using Assignments = std::vector<Assignment>;

struct Option {
  std::string page, id, label;
  std::vector<Choice> choices;
  bool restart = false;                                  // taken at start-up only
  std::string (*read)(const Option&) = nullptr;          // the choice value running now
  Assignments (*assign)(const Option&, const std::string&) = nullptr;
  std::string cvar;   // the cvar of a plain option (read and assign by default)
  bool text = false;  // the cvar is a string
  std::string pad;    // a wheel button row: the pad button it gives (hid_wheel_buttons)
  std::string section;  // shown while the Category row shows it (kSections)
};

// The Category row's choices: the page's sections.
const std::vector<Choice> kSections = {
    {"Display", "display"}, {"Controller buttons", "controller"}, {"Wheel", "wheel"},
    {"Wheel buttons", "buttons"}, {"Developer", "developer"}};
constexpr std::string_view kCategory = "category";
// The section last shown, which the page opens on again (not saved).
std::string g_section = "display";

std::string Flag(std::string_view cvar) { return rex::cvar::GetFlagByName(cvar); }

// The choice whose value is nearest the number the cvar holds: a configuration file or the
// command line may have set one between the choices.
std::string Nearest(const Option& o, const std::string& v) {
  const double n = std::atof(v.c_str());
  const auto d = [n](const Choice& c) { return std::abs(std::atof(c.value.c_str()) - n); };
  return std::ranges::min(o.choices, {}, d).value;
}

std::string ReadPlain(const Option& o) {
  const std::string v = Flag(o.cvar);
  for (const Choice& c : o.choices) {
    if (c.value == v) return v;
  }
  return o.text ? o.choices.front().value : Nearest(o, v);
}

Assignments AssignPlain(const Option& o, const std::string& v) { return {{o.cvar, v, o.text}}; }

// hid_wheel_buttons: the pad button of each wheel button, in order ("A,B,-,LB,...").
std::vector<std::string> WheelButtons() {
  std::vector<std::string> out;
  const std::string spec = Flag("hid_wheel_buttons");
  size_t pos = 0;
  while (pos <= spec.size()) {
    size_t end = spec.find(',', pos);
    if (end == std::string::npos) end = spec.size();
    std::string item = spec.substr(pos, end - pos);
    std::erase(item, ' ');
    std::ranges::transform(item, item.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    out.push_back(item.empty() ? "-" : item);
    pos = end + 1;
  }
  return out;
}

// A wheel button row: which wheel button gives the pad button (the first, if several do).
std::string ReadButton(const Option& o) {
  const std::vector<std::string> map = WheelButtons();
  for (size_t i = 0; i < map.size(); ++i) {
    if (map[i] == o.pad) return std::to_string(i);
  }
  return "none";
}

// The pad button moves to wheel button `v`, which gives up what it gave before; no other wheel
// button gives it any more.
Assignments AssignButton(const Option& o, const std::string& v) {
  std::vector<std::string> map = WheelButtons();
  for (std::string& b : map) {
    if (b == o.pad) b = "-";
  }
  if (v != "none") {
    const size_t at = size_t(std::atoi(v.c_str()));
    if (map.size() <= at) map.resize(at + 1, "-");
    map[at] = o.pad;
  }
  while (!map.empty() && map.back() == "-") map.pop_back();
  std::string spec;
  for (size_t i = 0; i < map.size(); ++i) spec += (i ? "," : "") + map[i];
  return {{"hid_wheel_buttons", spec, true}};
}

// hid_pad_buttons (rexglue-sdk wheel_input.cpp): what each controller button gives, in this
// order; empty when every button gives its own.
constexpr const char* kPadOrder[] = {"A",    "B",     "X",  "Y",  "LB", "RB",   "LT",   "RT",
                                     "BACK", "START", "LS", "RS", "UP", "DOWN", "LEFT", "RIGHT"};
constexpr size_t kPadButtons = std::size(kPadOrder);

std::vector<std::string> PadButtons() {
  std::vector<std::string> out;
  const std::string spec = Flag("hid_pad_buttons");
  size_t pos = 0;
  while (!spec.empty() && pos <= spec.size() && out.size() < kPadButtons) {
    size_t end = spec.find(',', pos);
    if (end == std::string::npos) end = spec.size();
    std::string item = spec.substr(pos, end - pos);
    std::erase(item, ' ');
    std::ranges::transform(item, item.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    out.push_back(item.empty() ? "-" : item);
    pos = end + 1;
  }
  while (out.size() < kPadButtons) out.push_back(kPadOrder[out.size()]);
  return out;
}

size_t PadIndex(const std::string& name) {
  for (size_t i = 0; i < kPadButtons; ++i) {
    if (name == kPadOrder[i]) return i;
  }
  return 0;
}

std::string ReadPadButton(const Option& o) { return PadButtons()[PadIndex(o.pad)]; }

Assignments AssignPadButton(const Option& o, const std::string& v) {
  std::vector<std::string> map = PadButtons();
  map[PadIndex(o.pad)] = v;
  bool identity = true;
  std::string spec;
  for (size_t i = 0; i < kPadButtons; ++i) {
    identity = identity && map[i] == kPadOrder[i];
    spec += (i ? "," : "") + map[i];
  }
  return {{"hid_pad_buttons", identity ? std::string() : spec, true}};
}

constexpr int kWheelAxes = 24;     // the wheel's parts' axes together (wheel, pedals, shifter)
constexpr int kWheelButtons = 48;  // likewise buttons

std::vector<Option> MakeOptions() {
  const std::vector<Choice> on_off = {{"Off", "false"}, {"On", "true"}};
  std::vector<Choice> axes, axes_auto = {{"Automatic", "-1"}};
  for (int i = 0; i < kWheelAxes; ++i) {
    axes.push_back({"Axis " + std::to_string(i), std::to_string(i)});
    axes_auto.push_back(axes.back());
  }
  std::vector<Choice> buttons = {{"None", "none"}};
  for (int i = 0; i < kWheelButtons; ++i) {
    buttons.push_back({"Button " + std::to_string(i), std::to_string(i)});
  }
  std::vector<Option> o;
  {
    Option category;
    category.page = "adjustments";
    category.id = std::string(kCategory);
    category.label = "Category";
    category.choices = kSections;
    category.read = [](const Option&) { return g_section; };
    category.assign = [](const Option&, const std::string& v) {
      g_section = v;
      return Assignments{};
    };
    o.push_back(std::move(category));
  }
  auto plain = [&o](std::string section, std::string id, std::string label, std::vector<Choice> choices,
                    std::string cvar, bool text = false, bool restart = false) {
    Option opt;
    opt.page = "adjustments";
    opt.section = std::move(section);
    opt.id = std::move(id);
    opt.label = std::move(label);
    opt.choices = std::move(choices);
    opt.cvar = std::move(cvar);
    opt.text = text;
    opt.restart = restart;
    opt.read = ReadPlain;
    opt.assign = AssignPlain;
    o.push_back(std::move(opt));
  };

  // Wheel. Numbers count across the wheel's parts (base first, then pedals and shifter), as the
  // log's "SDL wheel part" lines list them; --hid_wheel_log_axes logs each axis that moves.
  plain("wheel", "steer_axis", "Steering axis", axes, "hid_wheel_steer_axis");
  plain("wheel", "throttle_axis", "Accelerator axis", axes_auto, "hid_wheel_throttle_axis");
  plain("wheel", "brake_axis", "Brake axis", axes_auto, "hid_wheel_brake_axis");
  plain("wheel", "pedals_rest", "Pedals at rest",
        {{"Automatic", "auto"}, {"High", "high"}, {"Low", "low"}}, "hid_wheel_pedals_rest", true);
  plain("wheel", "steer_scale", "Steering sensitivity",
        {{"50%", "0.5"}, {"75%", "0.75"}, {"100%", "1.0"}, {"125%", "1.25"}, {"150%", "1.5"},
         {"200%", "2.0"}, {"250%", "2.5"}, {"300%", "3.0"}, {"400%", "4.0"}},
        "hid_wheel_steer_scale");
  plain("wheel", "deadzone", "Steering dead zone",
        {{"None", "0"}, {"Small", "500"}, {"Medium", "1500"}, {"Large", "4000"}},
        "hid_wheel_deadzone");
  plain("wheel", "invert_steering", "Reverse steering", on_off, "hid_wheel_invert_steering");
  plain("wheel", "as_360_wheel", "Shown to the game as",
        {{"Gamepad", "false"}, {"Xbox 360 wheel", "true"}}, "hid_wheel_as_360_wheel");
  struct Pad {
    const char* name;
    const char* label;
  };
  const Pad kPads[] = {Pad{"A", "A button"}, Pad{"B", "B button"}, Pad{"X", "X button"},
                      Pad{"Y", "Y button"}, Pad{"LB", "Left bumper"}, Pad{"RB", "Right bumper"},
                      Pad{"LT", "Left trigger"}, Pad{"RT", "Right trigger"}, Pad{"BACK", "Back"},
                      Pad{"START", "Start"}, Pad{"LS", "Left stick click"},
                      Pad{"RS", "Right stick click"}, Pad{"UP", "D-pad up"},
                      Pad{"DOWN", "D-pad down"}, Pad{"LEFT", "D-pad left"},
                      Pad{"RIGHT", "D-pad right"}};
  for (const Pad p : kPads) {
    Option opt;
    opt.page = "adjustments";
    opt.section = "buttons";
    opt.id = std::string("button_") + p.name;
    opt.label = p.label;
    opt.choices = buttons;
    opt.pad = p.name;
    opt.read = ReadButton;
    opt.assign = AssignButton;
    o.push_back(std::move(opt));
  }

  // Controller buttons: what each button of a controller gives.
  std::vector<Choice> pad_choices;
  for (const char* name : kPadOrder) {
    for (const Pad p : kPads) {
      if (std::string_view(p.name) == name) pad_choices.push_back({p.label, name});
    }
  }
  pad_choices.push_back({"None", "-"});
  for (const char* name : kPadOrder) {
    for (const Pad p : kPads) {
      if (std::string_view(p.name) != name) continue;
      Option opt;
      opt.page = "adjustments";
      opt.section = "controller";
      opt.id = std::string("pad_") + name;
      opt.label = p.label;
      opt.choices = pad_choices;
      opt.pad = name;
      opt.read = ReadPadButton;
      opt.assign = AssignPadButton;
      o.push_back(std::move(opt));
    }
  }

  // Display.
  plain("display", "hdr", "HDR output", {{"Automatic", "auto"}, {"On", "on"}, {"Off", "off"}},
        "hdr_output", true, true);
  plain("display", "paper_white", "HDR brightness",
        {{"80 nits", "80"}, {"100 nits", "100"}, {"120 nits", "120"}, {"150 nits", "150"},
         {"200 nits", "200"}, {"250 nits", "250"}, {"300 nits", "300"}, {"400 nits", "400"}},
        "hdr_paper_white");
  plain("display", "highlights", "HDR highlights",
        {{"Off", "0"}, {"Low", "0.5"}, {"Full", "1"}}, "hdr_highlights");
  {
    // 50% - 300% in 5% steps.
    std::vector<Choice> fov;
    for (int pct = 50; pct <= 300; pct += 5) {
      char value[16];
      std::snprintf(value, sizeof value, "%.2f", pct / 100.0);
      fov.push_back({pct == 100 ? "Default" : std::to_string(pct) + "%", value});
    }
    plain("display", "fov", "Field of view", std::move(fov), "fov_scale");
  }
  {
    // 25% - 300% of the car's own chase camera height.
    std::vector<Choice> height;
    for (int pct = 25; pct <= 300; pct += 25) {
      char value[16];
      std::snprintf(value, sizeof value, "%.2f", pct / 100.0);
      height.push_back({pct == 100 ? "Default" : std::to_string(pct) + "%", value});
    }
    plain("display", "camera_height", "Chase camera height", std::move(height), "camera_height");
  }
  // Supersampling: the scene renders at this height and is scaled to the window, the sharpest
  // anti-aliasing (renderer.cpp internal_resolution, read at start-up).
  plain("display", "resolution", "Rendering resolution",
        {{"720p (native)", "720"}, {"1080p", "1080"}, {"1440p", "1440"}, {"4K", "2160"}},
        "internal_resolution", false, true);
  plain("display", "msaa", "Multisampling (MSAA)",
        {{"Off", "1"}, {"2x", "2"}, {"4x", "4"}, {"8x", "8"}}, "msaa", false, true);
  plain("display", "fxaa", "Anti-aliasing", {{"Off", "false"}, {"FXAA", "true"}}, "fxaa");
  plain("display", "speedo", "Speedometer (in-car view)", on_off, "speedo");
  plain("developer", "debug_menu", "Debug menu (Y on the title screen)", on_off,
        "game_debug_menus");
  plain("display", "frame_rate", "Frame rate limit",
        {{"30", "30"}, {"60", "60"}, {"120", "120"}, {"144", "144"}, {"165", "165"},
         {"240", "240"}, {"Unlimited", "0"}},
        "frame_rate_limit");
  return o;
}

const std::vector<Option>& Options() {
  static const std::vector<Option> options = MakeOptions();
  return options;
}

// Start-up options chosen since the start, by id: their cvars keep the running values.
std::map<std::string, std::string> g_pending;
std::mutex g_mutex;  // the configuration file

std::filesystem::path ConfigFile() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return std::filesystem::path(exe).parent_path() / "pgr4_recompiled.toml";
}

std::string Value(const Option& o) {
  if (const auto it = g_pending.find(o.id); it != g_pending.end()) return it->second;
  return o.read(o);
}

void Set(const Option& o, const std::string& v) {
  if (o.restart) {
    if (v == o.read(o))
      g_pending.erase(o.id);
    else
      g_pending[o.id] = v;
    return;
  }
  for (const Assignment& a : o.assign(o, v)) {
    if (!rex::cvar::SetFlagByName(a.cvar, a.value)) {
      REXLOG_ERROR("PGR4 settings: cannot set {} to {}", a.cvar, a.value);
    }
  }
}

std::string_view Trim(std::string_view s) {
  const size_t first = s.find_first_not_of(" \t");
  if (first == std::string_view::npos) return {};
  return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

// The `# comment` ending a `name = value` line from `from` on, outside a string, with the blanks
// before it; empty without one.
std::string_view Comment(std::string_view line, size_t from) {
  char quote = 0;
  for (size_t i = from; i < line.size(); ++i) {
    const char c = line[i];
    if (quote) {
      if (c == quote && !(quote == '"' && line[i - 1] == '\\')) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '#') {
      const size_t text = line.substr(0, i).find_last_not_of(" \t");
      return line.substr(text == std::string_view::npos ? 0 : text + 1);
    }
  }
  return {};
}

struct Entry {
  std::string_view name, value;
};
std::optional<Entry> Parse(std::string_view line) {
  line = Trim(line);
  const size_t eq = line.find('=');
  if (line.starts_with('#') || line.starts_with('[') || eq == std::string_view::npos) return {};
  const Entry e{Trim(line.substr(0, eq)),
                Trim(line.substr(eq + 1, line.size() - eq - 1 - Comment(line, eq + 1).size()))};
  if (e.name.empty() || e.value.empty()) return {};
  return e;
}

using Lines = std::vector<std::pair<std::string, std::string>>;

// `set` written into the configuration file in place: the line of a cvar takes its value,
// keeping its comment, and a cvar the file has no line for gets one under the menu's comment.
// The file's other lines stay. (pgr4-native's Write.)
bool Write(const Lines& set) {
  std::lock_guard lock(g_mutex);
  const std::filesystem::path file = ConfigFile();
  std::vector<std::string> lines;
  bool crlf = false;
  {
    std::ifstream f(file, std::ios::binary);
    for (std::string line; std::getline(f, line);) {
      if (line.ends_with('\r')) {
        line.pop_back();
        crlf = true;
      }
      lines.push_back(std::move(line));
    }
  }
  // A cvar's line, and where new ones go: before the first table, whose keys are not cvars.
  std::map<std::string, size_t, std::less<>> at;
  size_t end = lines.size();
  for (size_t i = 0; i < lines.size(); ++i) {
    if (Trim(lines[i]).starts_with('[')) end = std::min(end, i);
    if (const std::optional<Entry> e = Parse(lines[i]); e && i < end) at.emplace(e->name, i);
  }
  constexpr std::string_view kAdded = "# Set in the game's Options menu.";
  bool header = std::ranges::find(lines, kAdded) != lines.end();
  for (const auto& [cvar, value] : set) {
    const std::string line = cvar + " = " + value;
    if (const auto it = at.find(cvar); it != at.end()) {
      std::string& old = lines[it->second];
      old = line + std::string(Comment(old, old.find('=') + 1));
      continue;
    }
    if (!header) {
      header = true;
      lines.insert(lines.begin() + std::ptrdiff_t(end), {"", std::string(kAdded)});
      end += 2;
    }
    lines.insert(lines.begin() + std::ptrdiff_t(end), line);
    at.emplace(cvar, end++);
  }
  std::string toml;
  for (const std::string& line : lines) toml += line + (crlf ? "\r\n" : "\n");
  const std::filesystem::path temp = std::filesystem::path(file) += ".new";
  if (std::ofstream f(temp, std::ios::binary | std::ios::trunc); !(f << toml)) {
    REXLOG_ERROR("PGR4 settings: cannot write {}", temp.string());
    return false;
  }
  std::error_code ec;
  std::filesystem::rename(temp, file, ec);
  if (ec) {
    REXLOG_ERROR("PGR4 settings: cannot write {}: {}", file.string(), ec.message());
    return false;
  }
  REXLOG_INFO("PGR4 settings: saved {} setting(s) to {}", set.size(), file.string());
  return true;
}

// Lua: Pgr4.Page(page) -> {{id, label, choices = {label...}, value = shown choice from 0,
// shown_with = {id, choices = {[choice from 0] = true}} or nil}...}, the labels string IDs
// (Lua::Text). A row with shown_with is shown while the Category row shows its section. A start-up option's choices other than the running one are
// marked " *".
void LuaPage(PPCContext& ctx, uint8_t* base) {
  pgr4::lua::Lua lua(ctx, base, ctx.r3.u32);
  const std::string page = lua.ToString(1);
  lua.CreateTable(0, 0);
  int row = 0;
  for (const Option& o : Options()) {
    if (o.page != page) continue;
    lua.CreateTable(0, 4);
    lua.PushString(o.id);
    lua.SetField(-2, "id");
    lua.PushNumber(lua.Text(o.label));
    lua.SetField(-2, "label");
    const std::string value = Value(o), running = o.read(o);
    lua.CreateTable(int(o.choices.size()), 0);
    int shown = 0;
    for (size_t i = 0; i < o.choices.size(); ++i) {
      const Choice& c = o.choices[i];
      if (c.value == value) shown = int(i);
      lua.PushNumber(lua.Text(o.restart && c.value != running ? c.label + " *" : c.label));
      lua.RawSetI(-2, int(i) + 1);
    }
    lua.SetField(-2, "choices");
    lua.PushNumber(shown);
    lua.SetField(-2, "value");
    if (!o.section.empty()) {
      // shown_with = {id = "category", choices = {[section's choice from 0] = true}}
      lua.CreateTable(0, 2);
      lua.PushString(kCategory);
      lua.SetField(-2, "id");
      lua.CreateTable(0, 1);
      for (size_t i = 0; i < kSections.size(); ++i) {
        if (kSections[i].value != o.section) continue;
        lua.PushBoolean(true);
        lua.RawSetI(-2, int(i));
      }
      lua.SetField(-2, "choices");
      lua.SetField(-2, "shown_with");
    }
    lua.RawSetI(-2, ++row);
  }
  pgr4::lua::Results(ctx, 1);
}

// Lua: Pgr4.Apply(page, {[id] = choice from 0...}) -> whether a start-up option of the page
// differs from what runs (a restart applies it). The changed options are set in page order and
// their cvars saved with the values they then hold.
void LuaApply(PPCContext& ctx, uint8_t* base) {
  pgr4::lua::Lua lua(ctx, base, ctx.r3.u32);
  const std::string page = lua.ToString(1);
  if (lua.TypeOf(2) == pgr4::lua::kTable) {
    std::vector<const Option*> changed;
    for (const Option& o : Options()) {
      if (o.page != page) continue;
      lua.GetField(2, o.id);
      if (lua.TypeOf(-1) == pgr4::lua::kNumber) {
        const double n = lua.ToNumber(-1);
        if (n >= 0 && n < double(o.choices.size()) && o.choices[size_t(n)].value != Value(o)) {
          Set(o, o.choices[size_t(n)].value);
          changed.push_back(&o);
        }
      }
      lua.Pop(1);
    }
    Lines lines;
    for (const Option* o : changed) {
      for (const Assignment& a : o->assign(*o, Value(*o))) {
        // A live option's cvar as it is now (wheel buttons share one); a start-up one's chosen.
        const std::string v = o->restart ? a.value : Flag(a.cvar);
        const std::string toml = a.text ? '"' + v + '"' : v;
        auto it = std::ranges::find(lines, a.cvar, &Lines::value_type::first);
        if (it != lines.end())
          it->second = toml;
        else
          lines.emplace_back(a.cvar, toml);
      }
    }
    if (!lines.empty()) Write(lines);
  }
  lua.PushBoolean(std::ranges::any_of(
      Options(), [&](const Option& o) { return o.page == page && g_pending.contains(o.id); }));
  pgr4::lua::Results(ctx, 1);
}

// Split screen for 3 and 4 players (scripts/MultiPlayerMenu.lua). The four profiles (141984
// bytes each from 0x82BFE2A8 + 8) are active while +0x54 of theirs is set, as Profile.NumActive
// counts them; Lobby.AddPrimaryPlayer / AddCoopPlayer add the profiles whose indices are at
// 0x82C88E50 / 0x82C88E54 (Profile.SetupGamerProfiles stores the first two active there).
constexpr uint32_t kProfiles = 0x82BFE2A8, kProfileStride = 141984, kProfileActive = 0x54;
constexpr uint32_t kPrimaryProfile = 0x82C88E50, kCoopProfile = 0x82C88E54;

uint32_t GuestU32(uint8_t* base, uint32_t a) {
  return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a));
}

// Lua: Pgr4.ExtraLocalProfiles() -> {profile index...}: the active profiles other than the two
// the lobby's primary and co-op players are.
void LuaExtraLocalProfiles(PPCContext& ctx, uint8_t* base) {
  pgr4::lua::Lua lua(ctx, base, ctx.r3.u32);
  const uint32_t primary = GuestU32(base, kPrimaryProfile), coop = GuestU32(base, kCoopProfile);
  lua.CreateTable(2, 0);
  int n = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (i == primary || i == coop) continue;
    if (!GuestU32(base, kProfiles + kProfileStride * i + kProfileActive)) continue;
    lua.PushNumber(double(i));
    lua.RawSetI(-2, ++n);
  }
  pgr4::lua::Results(ctx, 1);
}

// Lua: Pgr4.SetCoopProfile(index) -> the previous: the profile Lobby.AddCoopPlayer adds.
void LuaSetCoopProfile(PPCContext& ctx, uint8_t* base) {
  pgr4::lua::Lua lua(ctx, base, ctx.r3.u32);
  const uint32_t previous = GuestU32(base, kCoopProfile);
  const double n = lua.ToNumber(1);
  if (n >= 0 && n < 4) {
    *reinterpret_cast<uint32_t*>(base + kCoopProfile) = __builtin_bswap32(uint32_t(n));
  }
  lua.PushNumber(double(previous));
  pgr4::lua::Results(ctx, 1);
}

// Lua: Pgr4.Split3Wide() -> whether the 3-player layout puts player 1 across the top
// (four_players.cpp, --split3_wide).
void LuaSplit3Wide(PPCContext& ctx, uint8_t* base) {
  pgr4::lua::Lua lua(ctx, base, ctx.r3.u32);
  lua.PushBoolean(REXCVAR_GET(split3_wide));
  pgr4::lua::Results(ctx, 1);
}

constexpr pgr4::lua::Function kFunctions[] = {
    {"Split3Wide", LuaSplit3Wide},
    {"Page", LuaPage},
    {"Apply", LuaApply},
    {"ExtraLocalProfiles", LuaExtraLocalProfiles},
    {"SetCoopProfile", LuaSetCoopProfile},
};

}  // namespace
}  // namespace pgr4::settings

std::span<const pgr4::lua::Function> pgr4::lua::Pgr4LuaFunctions() {
  return pgr4::settings::kFunctions;
}

// GothamSystem.IsFinalBuild (sub_825CC570), a lua_CFunction: true in the shipped game. The title
// screen's Y opens the debug menu only when it is false (Screens\Intro\StartScreen.lua), as
// pgr4-native's settings and the PGR4 debug menu kit found. After pgr4-native's hook.
REX_HOOK_RAW(sub_825CC570) {
  pgr4::lua::Lua lua(ctx, base, ctx.r3.u32);
  lua.PushBoolean(!REXCVAR_GET(game_debug_menus));
  pgr4::lua::Results(ctx, 1);
}
