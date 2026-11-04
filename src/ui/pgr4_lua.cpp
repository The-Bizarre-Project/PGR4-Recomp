// The game's Lua scripts and this project's (pgr4_lua.h).
//
// The UI loads each script by its archive name ("Screens\\Options\\MainMenu.lua") through
// sub_82508360(L, name): it opens the archive entry (sub_82883FA8, which keeps the last entry
// when the name has none), pushes "@name" and lua_loads the entry with its reader, leaving the
// chunk or the error message on the stack and returning lua_load's status. Its caller runs the
// chunk (sub_82508568: lua_pcall, then the error report). Replaced here: a screen of this project
// is its chunk, loaded from its source (the game's Lua keeps its parser: luaB_loadstring and the
// parser's messages are in the image); a patch runs after the game's chunk of its name, as one
// chunk.
//
// Ported from pgr4-native (games/pgr4/src/pgr4_lua.cpp).
#include "ui/pgr4_lua.h"

#include <rex/cvar.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// The game's lapi.c and lauxlib.c.
REX_EXTERN(sub_82505960);         // lua_gettop
REX_EXTERN(sub_82505978);         // lua_settop
REX_EXTERN(sub_82505A50);         // lua_insert
REX_EXTERN(sub_82505B98);         // lua_pushvalue
REX_EXTERN(sub_82505BE0);         // lua_type
REX_EXTERN(sub_82505EE0);         // lua_tonumber
REX_EXTERN(sub_82505F98);         // lua_toboolean
REX_EXTERN(sub_82505FF0);         // lua_tolstring
REX_EXTERN(sub_82506310);         // lua_pushnil
REX_EXTERN(sub_82506330);         // lua_pushnumber
REX_EXTERN(sub_82506380);         // lua_pushlstring
REX_EXTERN(sub_82506560);         // lua_pushcclosure
REX_EXTERN(sub_82506638);         // lua_pushboolean
REX_EXTERN(sub_82506700);         // lua_getfield
REX_EXTERN(sub_82506840);         // lua_createtable
REX_EXTERN(sub_82506A18);         // lua_setfield
REX_EXTERN(sub_82506B40);         // lua_rawseti
REX_EXTERN(sub_82508178);         // luaL_loadbuffer
REX_EXTERN(__imp__sub_82508360);  // the archive's chunk loader, replaced below
REX_EXTERN(__imp__sub_82506E10);  // lua_pcall, whose failures are logged below
REX_EXTERN(__imp__sub_8289B1D0);  // the string table's lookup, which answers Lua::Text's IDs
REX_EXTERN(__imp__sub_8289B898);

REXCVAR_DEFINE_STRING(ui_text_language, "", "PGR4",
                      "Debug: the language of the port's own menu texts by langname.lip name "
                      "(german, french, japanese...), whatever the game's (empty: the game's)");  // a .loc file's load, which names the language (pgr4_texts.cpp)

namespace pgr4::lua {

uint64_t Lua::Call(PPCFunc* f, int64_t a, int64_t b, int64_t c) {
  ctx_.r3.u64 = L_;
  ctx_.r4.s64 = a;
  ctx_.r5.s64 = b;
  ctx_.r6.s64 = c;
  f(ctx_, base_);
  return ctx_.r3.u64;
}

uint32_t Lua::StackString(std::string_view s) {
  const uint32_t at = (ctx_.r1.u32 - uint32_t(s.size()) - 1) & ~15u;
  std::memcpy(base_ + at, s.data(), s.size());
  base_[at + s.size()] = 0;
  ctx_.r1.u64 = at;
  return at;
}

int Lua::GetTop() { return int32_t(Call(sub_82505960)); }
void Lua::SetTop(int index) { Call(sub_82505978, index); }
void Lua::Insert(int index) { Call(sub_82505A50, index); }
void Lua::PushValue(int index) { Call(sub_82505B98, index); }
int Lua::TypeOf(int index) { return int32_t(Call(sub_82505BE0, index)); }
double Lua::ToNumber(int index) {
  Call(sub_82505EE0, index);
  return ctx_.f1.f64;
}
bool Lua::ToBoolean(int index) { return uint32_t(Call(sub_82505F98, index)) != 0; }

std::string Lua::ToString(int index) {
  const uint32_t s = uint32_t(Call(sub_82505FF0, index, 0));
  return s ? std::string(reinterpret_cast<const char*>(base_ + s)) : std::string();
}

void Lua::PushNil() { Call(sub_82506310); }
void Lua::PushNumber(double n) {
  ctx_.f1.f64 = n;
  Call(sub_82506330);
}
void Lua::PushBoolean(bool b) { Call(sub_82506638, b); }

void Lua::PushString(std::string_view s) {
  const uint32_t r1 = ctx_.r1.u32;
  Call(sub_82506380, StackString(s), int64_t(s.size()));
  ctx_.r1.u64 = r1;
}

void Lua::PushFunction(uint32_t guest_function) { Call(sub_82506560, guest_function, 0); }

void Lua::GetField(int index, std::string_view key) {
  const uint32_t r1 = ctx_.r1.u32;
  Call(sub_82506700, index, StackString(key));
  ctx_.r1.u64 = r1;
}

void Lua::SetField(int index, std::string_view key) {
  const uint32_t r1 = ctx_.r1.u32;
  Call(sub_82506A18, index, StackString(key));
  ctx_.r1.u64 = r1;
}

void Lua::CreateTable(int array, int records) { Call(sub_82506840, array, records); }
void Lua::RawSetI(int index, int n) { Call(sub_82506B40, index, n); }
int Lua::PCall(int args, int results) {
  return int32_t(Call(__imp__sub_82506E10, args, results, 0));
}

int Lua::LoadBuffer(uint32_t chunk, uint32_t size, std::string_view name) {
  const uint32_t r1 = ctx_.r1.u32;
  const uint32_t guest_name = StackString(name);
  ctx_.r3.u64 = L_;
  ctx_.r4.u64 = chunk;
  ctx_.r5.u64 = size;
  ctx_.r6.u64 = guest_name;
  sub_82508178(ctx_, base_);
  ctx_.r1.u64 = r1;
  return ctx_.r3.s32;
}

namespace {

// The game's string table (sub_828814C0 looks its IDs up there) and what the lookup returns for
// an ID it lacks.
constexpr uint32_t kStringTables = 0x82CA178C, kMissingString = 0x820042E8;
// The port's string IDs count up from kFirstText, skipping those the table has: the game's IDs
// are hashes of the strings' names, and 4096-32766 its dynamic strings (sub_828814C0).
constexpr uint32_t kFirstText = 0x8000, kMaxTexts = 4096;

struct Texts {
  std::mutex mutex;
  std::unordered_map<std::string, uint32_t> ids;
  std::vector<uint32_t> strings;  // by ID - kFirstText: guest UTF-16BE, 0 for the table's own
  std::vector<std::string> english;  // by ID - kFirstText: the text asked for
  // Translations made so far (pgr4_texts.cpp): (ID, language) -> guest UTF-16BE.
  std::map<std::pair<uint32_t, int>, uint32_t> translated;
};
// The game's language (LanguageIndex of its langname.lip), set as its strings load.
std::atomic<int> g_language{-1};
Texts& OwnTexts() {
  static Texts texts;
  return texts;
}

std::u16string Utf16(std::string_view s) {
  std::u16string w;
  for (size_t i = 0; i < s.size();) {
    const uint8_t c = uint8_t(s[i]);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 0;
    if (!n || i + n > s.size()) {
      w += u'?';
      ++i;
      continue;
    }
    uint32_t u = n == 1 ? c : c & (n == 2 ? 0x1F : 0x0F);
    for (size_t k = 1; k < n; ++k) u = u << 6 | (uint8_t(s[i + k]) & 0x3F);
    w += char16_t(u);
    i += n;
  }
  return w;
}

}  // namespace

// `s` (UTF-8) as a NUL-terminated UTF-16BE string in guest memory, kept: 0 if out of memory.
uint32_t GuestUtf16(std::string_view s) {
  const std::u16string w = Utf16(s);
  auto* memory = rex::system::kernel_memory();
  const uint32_t at = memory->SystemHeapAlloc(uint32_t(2 * (w.size() + 1)));
  if (!at) return 0;
  auto* out = memory->TranslateVirtual<uint16_t*>(at);
  for (size_t i = 0; i < w.size(); ++i) out[i] = __builtin_bswap16(uint16_t(w[i]));
  out[w.size()] = 0;
  return at;
}

uint32_t Lua::Text(std::string_view s) {
  Texts& texts = OwnTexts();
  std::lock_guard lock(texts.mutex);
  if (const auto it = texts.ids.find(std::string(s)); it != texts.ids.end()) return it->second;
  const std::u16string w = Utf16(s);
  auto* memory = rex::system::kernel_memory();
  const uint32_t at = memory->SystemHeapAlloc(uint32_t(2 * (w.size() + 1)));
  if (!at) return 0;
  auto* out = memory->TranslateVirtual<uint16_t*>(at);
  for (size_t i = 0; i < w.size(); ++i) out[i] = __builtin_bswap16(uint16_t(w[i]));
  out[w.size()] = 0;
  // The lookup is a leaf that spills into its caller's frame: it gets a frame below r1.
  const uint64_t r1 = ctx_.r1.u64;
  ctx_.r1.u64 = (r1 - 128) & ~uint64_t(15);
  while (texts.strings.size() < kMaxTexts) {
    const uint32_t id = kFirstText + uint32_t(texts.strings.size());
    ctx_.r3.u64 = kStringTables;
    ctx_.r4.u64 = id;
    __imp__sub_8289B1D0(ctx_, base_);
    if (ctx_.r3.u32 != kMissingString) {
      texts.strings.push_back(0);
      continue;
    }
    texts.strings.push_back(at);
    texts.english.resize(texts.strings.size());
    texts.english.back() = std::string(s);
    texts.ids.emplace(s, id);
    ctx_.r1.u64 = r1;
    return id;
  }
  ctx_.r1.u64 = r1;
  REXLOG_ERROR("PGR4 Lua: no string ID left for \"{}\"", s);
  return 0;
}

namespace {

// src/ui/scripts/, embedded (CMakeLists.txt rebuilds this file when one changes).
struct Script {
  std::string_view name;  // the archive name it answers
  std::string_view source;
  bool patch;  // runs after the game's chunk of the name, instead of being the chunk
};

// #embed is C23's and C++26's; clang takes it in C++23 as an extension.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
constexpr char kSettingsScreen[] = {
#embed "scripts/Settings.lua"
};
constexpr char kOptionsMainMenuPatch[] = {
#embed "scripts/OptionsMainMenu.lua"
};
constexpr char kPauseMenuPatch[] = {
#embed "scripts/PauseMenu.lua"
};
constexpr char kDebugScreenPatch[] = {
#embed "scripts/DebugScreen.lua"
};
constexpr char kMultiPlayerMenuPatch[] = {
#embed "scripts/MultiPlayerMenu.lua"
};
constexpr char kStartScreenPatch[] = {
#embed "scripts/StartScreen.lua"
};
constexpr char kIntroMainMenuPatch[] = {
#embed "scripts/IntroMainMenu.lua"
};
constexpr char kGUIBankPatch[] = {
#embed "scripts/GUIBank.lua"
};
#pragma clang diagnostic pop
static_assert(sizeof kSettingsScreen > 0 && sizeof kOptionsMainMenuPatch > 0 && sizeof kPauseMenuPatch > 0 &&
              sizeof kDebugScreenPatch > 0);

constexpr Script kScripts[] = {
    {"Screens\\Pgr4\\Settings.lua", {kSettingsScreen, sizeof kSettingsScreen}, false},
    {"Screens\\Options\\MainMenu.lua", {kOptionsMainMenuPatch, sizeof kOptionsMainMenuPatch}, true},
    {"Screens\\InGame\\PauseMenu.lua", {kPauseMenuPatch, sizeof kPauseMenuPatch}, true},
    {"Screens\\Debug\\DebugScreen.lua", {kDebugScreenPatch, sizeof kDebugScreenPatch}, true},
    {"Screens\\Intro\\MultiPlayerMenu.lua", {kMultiPlayerMenuPatch, sizeof kMultiPlayerMenuPatch}, true},
    {"Screens\\Intro\\StartScreen.lua", {kStartScreenPatch, sizeof kStartScreenPatch}, true},
    {"Screens\\Intro\\MainMenu.lua", {kIntroMainMenuPatch, sizeof kIntroMainMenuPatch}, true},
    {"Screens\\GUIBank.lua", {kGUIBankPatch, sizeof kGUIBankPatch}, true},
};

std::span<const Script> Scripts() { return kScripts; }

bool SameName(std::string_view a, std::string_view b) {
  return std::ranges::equal(a, b, [](char x, char y) {
    auto fold = [](char c) { return c == '/' ? '\\' : (c >= 'A' && c <= 'Z' ? char(c + 32) : c); };
    return fold(x) == fold(y);
  });
}

// Each script's source, copied into guest memory at its first load and kept: lua_load reads it
// from there, as the game's reader reads its entries.
uint32_t GuestSource(size_t index) {
  static std::mutex mutex;
  static std::vector<uint32_t> sources(Scripts().size());
  std::lock_guard lock(mutex);
  if (!sources[index]) {
    const std::string_view s = Scripts()[index].source;
    auto* memory = rex::system::kernel_memory();
    if (const uint32_t at = memory->SystemHeapAlloc(uint32_t(s.size()))) {
      std::memcpy(memory->TranslateVirtual<char*>(at), s.data(), s.size());
      sources[index] = at;
    }
  }
  return sources[index];
}

int LoadScript(Lua& lua, size_t index) {
  const Script& s = Scripts()[index];
  const uint32_t chunk = GuestSource(index);
  if (!chunk) {
    lua.PushString("out of memory");
    return 4;  // LUA_ERRMEM
  }
  return lua.LoadBuffer(chunk, uint32_t(s.source.size()), std::string("@pgr4\\") += s.name);
}

// The patch run after the game's chunk, as one chunk: (game chunk, patch) -> the chunk the
// game runs. Both get its arguments; the patch's results are its.
constexpr std::string_view kComposeSource =
    "local chunk, patch = ...\n"
    "return function(...) chunk(...) return patch(...) end\n";

uint32_t GuestCompose() {
  static const uint32_t at = [] {
    auto* memory = rex::system::kernel_memory();
    const uint32_t a = memory->SystemHeapAlloc(uint32_t(kComposeSource.size()));
    if (a)
      std::memcpy(memory->TranslateVirtual<char*>(a), kComposeSource.data(), kComposeSource.size());
    return a;
  }();
  return at;
}

// Lua: Pgr4.Text(s) -> the string ID of s.
void LuaText(PPCContext& ctx, uint8_t* base) {
  Lua lua(ctx, base, ctx.r3.u32);
  lua.PushNumber(lua.Text(lua.ToString(1)));
  Results(ctx, 1);
}

constexpr Function kOwnFunctions[] = {{"Text", LuaText}};

// The Pgr4 table in L's globals, unless it is there: a state the UI made again gets it again.
void InstallFunctions(Lua& lua) {
  lua.GetGlobal("Pgr4");
  const bool present = lua.TypeOf(-1) != kNil;
  lua.Pop(1);
  if (present) return;
  static const std::vector<Function> functions = [] {
    std::vector<Function> f(std::begin(kOwnFunctions), std::end(kOwnFunctions));
    std::ranges::copy(Pgr4LuaFunctions(), std::back_inserter(f));
    return f;
  }();
  static const std::vector<uint32_t> thunks = [] {
    auto* dispatcher = rex::system::kernel_state()->function_dispatcher();
    std::vector<uint32_t> t;
    for (const Function& f : functions) t.push_back(dispatcher->AllocateThunk(f.function, 0));
    return t;
  }();
  lua.CreateTable(0, int(functions.size()));
  for (size_t i = 0; i < functions.size(); ++i) {
    if (!thunks[i]) continue;
    lua.PushFunction(thunks[i]);
    lua.SetField(-2, functions[i].name);
  }
  lua.SetGlobal("Pgr4");
}
void ReportError(Lua& lua, std::string_view what, std::string_view name) {
  REXLOG_ERROR("PGR4 Lua: {} {}: {}", what, name, lua.ToString(-1));
}

}  // namespace
}  // namespace pgr4::lua

REXCVAR_DEFINE_BOOL(ui_dump_scripts, false, "PGR4",
                    "Debug: write each game Lua script, as loaded, to lua_dump beside the exe");
REX_EXTERN(__imp__sub_82508308);  // the script loader's lua_Reader(L, state, size*) -> piece

namespace {
thread_local std::string g_loading;  // the script sub_82508360 is loading (ui_dump_scripts)
thread_local std::ofstream g_dump;
}  // namespace

// The loader's reader: with --ui_dump_scripts each piece it hands lua_load is appended to
// lua_dump/<script name>.
REX_HOOK_RAW(sub_82508308) {
  const uint32_t size_ptr = ctx.r5.u32;
  __imp__sub_82508308(ctx, base);
  if (!REXCVAR_GET(ui_dump_scripts) || g_loading.empty() || !ctx.r3.u32 || !size_ptr) {
    return;
  }
  if (!g_dump.is_open()) {
    std::string file = g_loading;
    std::ranges::replace(file, '\\', '/');
    const std::filesystem::path path = std::filesystem::path("lua_dump") / file;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    g_dump.open(path, std::ios::binary | std::ios::trunc);
  }
  const uint32_t size = rex::memory::load_and_swap<uint32_t>(base + size_ptr);
  if (g_dump && size && size < (16u << 20)) {
    g_dump.write(reinterpret_cast<const char*>(base + ctx.r3.u32), size);
  }
}

REX_HOOK_RAW(sub_82508360) {
  using namespace pgr4::lua;
  const uint32_t L = ctx.r3.u32, guest_name = ctx.r4.u32;
  const std::string name(reinterpret_cast<const char*>(base + guest_name));
  Lua lua(ctx, base, L);
  InstallFunctions(lua);
  const std::span<const Script> scripts = Scripts();
  const auto own = std::ranges::find_if(
      scripts, [&](const Script& s) { return !s.patch && SameName(s.name, name); });
  if (own != scripts.end()) {
    const int status = LoadScript(lua, size_t(own - scripts.begin()));
    if (status) ReportError(lua, "cannot load", name);
    ctx.r3.u64 = uint32_t(status);
    return;
  }
  // Diagnostic: a tester's game crashed inside this load after an hour (log 006, 2026-11-04),
  // its reader past the end of the script's buffer; the name says which script.
  REXLOG_INFO("PGR4 Lua: loading {}", name);
  g_loading = name;
  ctx.r3.u64 = L;
  ctx.r4.u64 = guest_name;
  __imp__sub_82508360(ctx, base);
  g_loading.clear();
  if (g_dump.is_open()) g_dump.close();
  const int status = ctx.r3.s32;
  const auto patch = std::ranges::find_if(
      scripts, [&](const Script& s) { return s.patch && SameName(s.name, name); });
  if (status == 0 && patch != scripts.end()) {
    // chunk -> compose(chunk, patch), or on a failure the game's chunk alone.
    const uint32_t compose = GuestCompose();
    if (!compose || lua.LoadBuffer(compose, uint32_t(kComposeSource.size()), "=pgr4") != 0) {
      ReportError(lua, "cannot patch", name);
      lua.Pop(1);
    } else {
      lua.PushValue(-2);  // chunk, compose, chunk
      if (LoadScript(lua, size_t(patch - scripts.begin())) != 0) {
        ReportError(lua, "cannot load the patch of", name);
        lua.Pop(3);
      } else if (lua.PCall(2, 1) != 0) {  // chunk, composed
        ReportError(lua, "cannot patch", name);
        lua.Pop(1);
      } else {
        lua.Insert(-2);
        lua.Pop(1);
      }
    }
  }
  ctx.r3.u64 = uint32_t(status);
}

// The string table's lookup (sub_8289B1D0(tables, id) -> the string, or kMissingString): the
// port's IDs (Lua::Text) are its strings.
REX_HOOK_RAW(sub_8289B1D0) {
  using namespace pgr4::lua;
  if (const uint32_t n = ctx.r4.u32 - kFirstText; n < kMaxTexts) {
    Texts& texts = OwnTexts();
    std::lock_guard lock(texts.mutex);
    if (n < texts.strings.size() && texts.strings[n]) {
      // In the game's language when it is one the port's texts have.
      const std::string forced = REXCVAR_GET(ui_text_language);
      const int language = forced.empty() ? g_language.load(std::memory_order_relaxed)
                                          : LanguageIndex(forced);
      if (language >= 0 && n < texts.english.size()) {
        auto [it, added] = texts.translated.try_emplace({n, language}, 0);
        if (added) it->second = GuestUtf16(Translate(texts.english[n], language));
        if (it->second) {
          ctx.r3.u64 = it->second;
          return;
        }
      }
      ctx.r3.u64 = texts.strings[n];
      return;
    }
  }
  __imp__sub_8289B1D0(ctx, base);
}

// sub_8289B898(tables + 4, path, language name, slot): a .loc file's load, with the language
// sub_82881238 found (the loc folder's langname.lip, else the console's language).
REX_HOOK_RAW(sub_8289B898) {
  using namespace pgr4::lua;
  if (const uint32_t name = ctx.r5.u32) {
    const std::string language(reinterpret_cast<const char*>(base + name));
    const int index = LanguageIndex(language);
    static std::atomic<bool> logged{false};
    if (g_language.exchange(index) != index || !logged.exchange(true)) {
      REXLOG_INFO("PGR4 Lua: game language {} ({})", language,
                  index < 0 ? "the port's texts in English" : "the port's texts translated");
    }
  }
  __imp__sub_8289B898(ctx, base);
}

// lua_pcall (sub_82506E10(L, nargs, nresults, errfunc)): the message of a failed call. The UI
// runs every chunk and screen callback through it and drops the message (sub_82508400 keeps it
// in a buffer nobody reads), and the game's scripts never call pcall themselves.
REX_HOOK_RAW(sub_82506E10) {
  const uint32_t L = ctx.r3.u32;
  __imp__sub_82506E10(ctx, base);
  if (ctx.r3.s32 != 0) {
    const uint64_t status = ctx.r3.u64;
    pgr4::lua::Lua lua(ctx, base, L);
    REXLOG_ERROR("PGR4 Lua: {}", lua.ToString(-1));
    ctx.r3.u64 = status;
  }
}
