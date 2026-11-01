// PGR4-Recomp launcher: game folder, title update / DLC packages, GPU and
// shader format, cvar overrides, then starts pgr4_recompiled.exe with the
// matching command line.
//
// Plain Win32 + GDI+ with the art and icon in resources, d3d12/dxgi/xinput
// loaded at run time, so it needs nothing installed on Windows or under
// Proton (Steam Deck). Mouse, touch and controller (A play, LB/RB tabs).
#ifndef UNICODE
#define UNICODE
#endif
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;
using namespace Gdiplus;

namespace {

constexpr wchar_t kGameExe[] = L"pgr4_recompiled.exe";
constexpr wchar_t kSettingsFile[] = L"pgr4_launcher.ini";
constexpr wchar_t kCvarCacheFile[] = L"pgr4_cvars.tsv";
constexpr uint32_t kTitleId = 0x4D5307F9;
constexpr uint32_t kTypeTitleUpdate = 0x000B0000;
constexpr uint32_t kTypeMarketplace = 0x00000002;
constexpr UINT kMsgCvarsLoaded = WM_APP + 1;
constexpr UINT kMsgPickFirst = WM_APP + 2;

// Layout is in logical pixels (96 DPI), scaled on paint and hit tests.
constexpr int kWidth = 1100, kHeight = 700;
constexpr int kCardX = 40, kCardY = 150, kCardW = 1020, kCardH = 440;
constexpr int kInnerX = kCardX + 26, kInnerW = kCardW - 52;

// Palette (the PGR4Recomp launcher's).
const Color kText(0xFF, 0xF2, 0xF2, 0xF2);
const Color kMuted(0xFF, 0x9C, 0xA3, 0xAA);
const Color kDim(0xFF, 0x77, 0x7F, 0x86);
const Color kAccent(0xFF, 0x4D, 0xA3, 0xFF);
const Color kAccentHover(0xFF, 0x6F, 0xB6, 0xFF);
const Color kCard(0xDD, 0x15, 0x18, 0x1B);
const Color kCardBorder(0xFF, 0x3A, 0x41, 0x47);
const Color kInset(0xAA, 0x20, 0x24, 0x28);
const Color kInsetBorder(0xFF, 0x30, 0x36, 0x3C);
const Color kButton(0xFF, 0x24, 0x28, 0x2C);
const Color kButtonHover(0xFF, 0x30, 0x35, 0x3A);
const Color kButtonBorder(0xFF, 0x34, 0x3A, 0x40);
const Color kField(0xFF, 0x16, 0x19, 0x1C);
const Color kGood(0xFF, 0x55, 0xD6, 0x6A);
const Color kWarn(0xFF, 0xFF, 0xB5, 0x47);
const Color kBad(0xFF, 0xFF, 0x5D, 0x5D);
const Color kDlc(0xFF, 0xB0, 0x7C, 0xFF);
const COLORREF kFieldRef = RGB(0x16, 0x19, 0x1C);
const COLORREF kTextRef = RGB(0xF2, 0xF2, 0xF2);

enum Tab { kTabPlay, kTabContent, kTabGraphics, kTabAdvanced, kTabCount };
const wchar_t *kTabNames[kTabCount] = {L"Play", L"Content", L"Graphics", L"Advanced"};

// Cvars the launcher sets itself; not offered as overrides.
const wchar_t *kManagedCvars[] = {L"game_data_root", L"fullscreen", L"shader_format", L"gpu_adapter",
                                  L"audio_maxqframes", L"execute_unclipped_draw_vs_on_cpu", L"dump_cvars",
                                  L"content_root", L"user_name", L"internal_resolution", L"aspect_ratio",
                                  L"window_width", L"window_height", L"frame_rate_limit"};

enum Action {
  kActNone,
  kActTab,
  kActBrowse,
  kActAddPackages,
  kActRemovePackage,
  kActOpenContent,
  kActSelectPackage,
  kActGpu,
  kActShader,
  kActResolution,
  kActAspect,
  kActFrameRate,
  kActFullscreen,
  kActAudio,
  kActCloseOnPlay,
  kActPlay,
  kActAddCvar,
  kActRemoveCvar,
  kActCvarBool,
  kActCvarChoice,
  kActPickCvar,
  kActClosePicker,
  kActOpenGameFolder,
  kActOpenLogs,
  kActToggleSession,
};

// ---------------------------------------------------------------------------
// Data

struct Gpu {
  std::wstring name;
  bool d3d12 = false;
  bool sm6 = false;
  uint64_t vram = 0;
};

struct Package {
  fs::path path;
  std::wstring title;
  uint32_t type = 0;
};

struct CvarDef {
  std::wstring name, category, type, default_value, description;
  std::vector<std::wstring> allowed;
  bool debug = false;
};

struct CvarOverride {
  std::wstring name, value;
  HWND edit = nullptr; // value field for string / number cvars
};

// Internal rendering heights (the game draws at 720p; targets scale by this / 720).
const int kResolutions[] = {720, 1080, 1440, 2160};
const wchar_t *kResolutionNames[] = {L"720p", L"1080p", L"1440p", L"4K"};
// Display aspect: Auto (the monitor's), the game's 16:9, 21:9 (64:27) and 32:9.
const double kAspects[] = {0.0, 16.0 / 9.0, 64.0 / 27.0, 32.0 / 9.0};
const wchar_t *kAspectNames[] = {L"Auto", L"16:9", L"21:9", L"32:9"};
// Frame rate limits (0: none). The 360 runs PGR4 at 30.
const int kFrameRates[] = {30, 60, 120, 0};
const wchar_t *kFrameRateNames[] = {L"30", L"60", L"120", L"Max"};

struct Settings {
  std::wstring game_dir, gpu;
  std::wstring user_name; // profile gamertag; empty: the game's default
  int shader_format = 0; // 0 auto, 1 DXIL, 2 DXBC
  int resolution = 0;    // index into kResolutions
  int aspect = 1;        // index into kAspects (16:9)
  int frame_rate = 1;    // index into kFrameRates (60)
  bool fullscreen = true, large_audio_buffer = false, close_on_play = true;
};

struct Hit {
  RectF rect;
  Action action;
  int index;
};

// Summary of the newest game log (logs/ beside the game).
struct LastSession {
  bool found = false;
  bool clean_exit = false;
  double avg_fps = 0;
  int minutes = 0, seconds = 0;
  std::wstring when;
};

struct App {
  HWND wnd = nullptr;
  float scale = 1.0f;
  ULONG_PTR gdiplus = 0;
  Settings settings;
  Tab tab = kTabPlay;
  std::vector<Gpu> gpus;
  int gpu_index = 0;
  std::vector<Package> packages;
  int selected_package = -1;
  std::vector<CvarDef> cvar_defs;
  bool cvars_loading = false;
  std::vector<CvarOverride> overrides;
  bool picker_open = false;
  int picker_scroll = 0, content_scroll = 0, cvar_scroll = 0;
  std::vector<int> picker_matches;
  std::vector<Hit> hits;
  Action hover_action = kActNone;
  int hover_index = -1;
  // Keyboard / controller focus (ring shown once they are used).
  Action focus_action = kActPlay;
  int focus_index = -1;
  bool focus_visible = false;
  // Animation: sliding tab underline, Play button glow.
  float tab_x = -1, tab_w = 0;
  bool animating = false;
  LastSession last;
  DWORD pad_repeat_at = 0;
  int pad_dir = 0;
  bool tracking = false;
  // Background art: cross-fades every few seconds.
  std::vector<std::unique_ptr<Bitmap>> art;
  int art_index = 0;
  DWORD fade_start = 0;
  bool fading = false;
  std::unique_ptr<Bitmap> backdrop; // composed art + shading at window size
  std::unique_ptr<Bitmap> ui;       // the whole UI without the Play glow
  bool dirty = true;                // ui needs rendering again
  bool ready = false;
  int backdrop_w = 0, backdrop_h = 0;
  float backdrop_fade = -1.0f;
  // Fonts.
  std::unique_ptr<FontFamily> family, family_bold, family_mono;
  bool session_open = false;   // "Last session" expanded
  float session_t = 0;         // its animated height (0..1)
  std::unique_ptr<Font> f_title, f_sub, f_tab, f_body, f_bold, f_small, f_button, f_play, f_mono, f_badge, f_stat;
  HFONT edit_font = nullptr;
  HBRUSH field_brush = nullptr;
  HWND game_dir_edit = nullptr, search_edit = nullptr, user_name_edit = nullptr;
  // Controller.
  using XInputGetStateFn = DWORD(WINAPI *)(DWORD, XINPUT_STATE *);
  XInputGetStateFn xinput_get_state = nullptr;
  WORD pad_buttons = 0;
};
App g;

fs::path ExeDir() {
  wchar_t buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return fs::path(buf).parent_path();
}

std::wstring GetText(HWND h) {
  std::wstring s(size_t(GetWindowTextLengthW(h)), L'\0');
  GetWindowTextW(h, s.data(), int(s.size() + 1));
  return s;
}

fs::path GameDir() { return fs::path(GetText(g.game_dir_edit)); }
fs::path ContentDir() { return GameDir() / "Content"; }

bool IsGameDir(const fs::path &dir) {
  std::error_code ec;
  return !dir.empty() && fs::is_regular_file(dir / "default.xex", ec);
}

std::wstring GuessGameDir() {
  const fs::path exe = ExeDir();
  for (const fs::path &dir : {exe, exe / "game", exe / "PGR4-Files", exe.parent_path() / "PGR4-Files",
                              exe.parent_path() / "game"}) {
    if (IsGameDir(dir)) {
      return dir.wstring();
    }
  }
  return L"";
}

// ---------------------------------------------------------------------------
// GPUs: D3D12 device per adapter, shader model 6 decides DXIL vs DXBC.

std::vector<Gpu> DetectGpus() {
  std::vector<Gpu> gpus;
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
  if (!dxgi) {
    return gpus;
  }
  using CreateFactoryFn = HRESULT(WINAPI *)(REFIID, void **);
  auto create_factory = reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
  auto create_device =
      d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
  IDXGIFactory1 *factory = nullptr;
  if (!create_factory || FAILED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
    return gpus;
  }
  IDXGIAdapter1 *adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      Gpu gpu;
      gpu.name = desc.Description;
      gpu.vram = desc.DedicatedVideoMemory;
      ID3D12Device *device = nullptr;
      if (create_device && SUCCEEDED(create_device(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                                   reinterpret_cast<void **>(&device)))) {
        gpu.d3d12 = true;
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_0};
        gpu.sm6 = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) &&
                  sm.HighestShaderModel >= D3D_SHADER_MODEL_6_0;
        device->Release();
      }
      gpus.push_back(gpu);
    }
    adapter->Release();
  }
  factory->Release();
  return gpus;
}

// ---------------------------------------------------------------------------
// Content packages (Xbox 360 STFS): magic, content type 0x344, title ID
// 0x360, display name 0x411 (UTF-16BE).

uint32_t LoadBE32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool ReadPackage(const fs::path &path, Package &out, std::wstring &error) {
  uint8_t header[0x600] = {};
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = L"cannot be opened";
    return false;
  }
  file.read(reinterpret_cast<char *>(header), sizeof(header));
  if (file.gcount() < std::streamsize(sizeof(header)) ||
      (memcmp(header, "CON ", 4) && memcmp(header, "LIVE", 4) && memcmp(header, "PIRS", 4))) {
    error = L"is not an Xbox 360 content package";
    return false;
  }
  if (LoadBE32(header + 0x360) != kTitleId) {
    error = L"is for a different game";
    return false;
  }
  out.path = path;
  out.type = LoadBE32(header + 0x344);
  if (out.type != kTypeTitleUpdate && out.type != kTypeMarketplace) {
    error = L"is not a title update or DLC package";
    return false;
  }
  out.title.clear();
  for (int i = 0; i < 64; ++i) {
    const wchar_t c = wchar_t(header[0x411 + i * 2] << 8 | header[0x412 + i * 2]);
    if (!c) {
      break;
    }
    out.title.push_back(c);
  }
  if (out.title.empty()) {
    out.title = out.type == kTypeTitleUpdate ? L"Title Update" : path.filename().wstring();
  }
  return true;
}

void RefreshPackages() {
  g.packages.clear();
  std::error_code ec;
  if (fs::is_directory(ContentDir(), ec)) {
    for (const auto &entry : fs::directory_iterator(ContentDir(), ec)) {
      Package p;
      std::wstring error;
      if (entry.is_regular_file(ec) && ReadPackage(entry.path(), p, error)) {
        g.packages.push_back(p);
      }
    }
  }
  std::sort(g.packages.begin(), g.packages.end(), [](const Package &a, const Package &b) {
    return a.type != b.type ? a.type > b.type : a.title < b.title;
  });
  g.selected_package = std::min(g.selected_package, int(g.packages.size()) - 1);
}

bool TitleUpdateInstalled() {
  std::error_code ec;
  return fs::is_regular_file(GameDir() / "TitleUpdate" / "default.xexp", ec);
}

bool TitleUpdatePresent() {
  return TitleUpdateInstalled() || std::any_of(g.packages.begin(), g.packages.end(), [](const Package &p) {
           return p.type == kTypeTitleUpdate;
         });
}

bool CanPlay() {
  std::error_code ec;
  return IsGameDir(GameDir()) && fs::is_regular_file(ExeDir() / kGameExe, ec) && TitleUpdatePresent();
}

std::wstring Widen(const std::string &s) {
  if (s.empty()) {
    return {};
  }
  std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
  return w;
}

// Newest real session log: average of the renderer's "N fps" lines, first
// to last timestamp, and whether it ended with the shutdown message.
LastSession ReadLastSession() {
  LastSession out;
  std::error_code ec;
  const fs::path dir = ExeDir() / "logs";
  std::vector<fs::directory_entry> logs;
  for (const auto &e : fs::directory_iterator(dir, ec)) {
    if (e.is_regular_file(ec) && e.path().extension() == ".log" && e.file_size(ec) > 4096) {
      logs.push_back(e);
    }
  }
  std::sort(logs.begin(), logs.end(),
            [](const auto &a, const auto &b) { return a.last_write_time() > b.last_write_time(); });
  for (const auto &log : logs) {
    std::ifstream in(log.path(), std::ios::binary);
    std::string line, first_ts, last_ts;
    double fps_sum = 0;
    int fps_n = 0;
    bool game = false, clean = false;
    while (std::getline(in, line)) {
      if (line.size() > 25 && line[0] == '[') {
        (first_ts.empty() ? first_ts : last_ts) = line.substr(1, 23);
      }
      if (line.find("Function table initialized") != std::string::npos) {
        game = true;
      }
      if (line.find("Window closing, shutting down") != std::string::npos ||
          line.find("Title terminated") != std::string::npos) {
        clean = true;
      }
      const size_t f = line.find(" fps, frame ");
      if (f != std::string::npos) {
        const size_t start = line.rfind(' ', f - 1);
        fps_sum += atof(line.c_str() + start + 1);
        ++fps_n;
      }
    }
    if (!game) {
      continue;
    }
    out.found = true;
    out.clean_exit = clean;
    out.avg_fps = fps_n ? fps_sum / fps_n : 0;
    auto secs = [](const std::string &ts) { // "YYYY-MM-DD HH:MM:SS.mmm"
      return ts.size() >= 19 ? atoi(ts.c_str() + 11) * 3600 + atoi(ts.c_str() + 14) * 60 + atoi(ts.c_str() + 17)
                             : 0;
    };
    int len = last_ts.empty() ? 0 : secs(last_ts) - secs(first_ts);
    if (len < 0) {
      len += 24 * 3600;
    }
    out.minutes = len / 60;
    out.seconds = len % 60;
    out.when = Widen(first_ts.substr(0, 16));
    break;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Settings: pgr4_launcher.ini beside the launcher; [cvars] holds overrides.

std::wstring IniPath() { return (ExeDir() / kSettingsFile).wstring(); }

// The gamertag lives in gamertag.txt (UTF-8) beside the game, shared with the
// game's Guide (Change Gamertag), so a name set in either shows in both.
std::filesystem::path GamertagPath() { return ExeDir() / L"gamertag.txt"; }

std::wstring LoadGamertag() {
  std::ifstream in(GamertagPath(), std::ios::binary);
  std::string utf8;
  std::getline(in, utf8);
  while (!utf8.empty() && (utf8.back() == '\r' || utf8.back() == ' ')) {
    utf8.pop_back();
  }
  if (utf8.empty()) {
    return L"";
  }
  std::wstring name(utf8.size(), L'\0');
  name.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), name.data(), int(name.size()))));
  return name;
}

void SaveGamertag(const std::wstring &name) {
  // Only when edited here, so a launcher left open doesn't undo a rename made
  // in the game.
  static std::wstring last = LoadGamertag();
  if (name == last) {
    return;
  }
  last = name;
  std::error_code ec;
  if (name.empty()) {
    std::filesystem::remove(GamertagPath(), ec); // the game's default
    return;
  }
  std::string utf8(name.size() * 4, '\0');
  utf8.resize(size_t(
      WideCharToMultiByte(CP_UTF8, 0, name.data(), int(name.size()), utf8.data(), int(utf8.size()), nullptr, nullptr)));
  std::ofstream(GamertagPath(), std::ios::binary | std::ios::trunc) << utf8 << '\n';
}

std::wstring ReadIni(const wchar_t *key, const wchar_t *fallback) {
  wchar_t buf[2048] = {};
  GetPrivateProfileStringW(L"launcher", key, fallback, buf, 2048, IniPath().c_str());
  return buf;
}

void LoadSettings() {
  Settings &s = g.settings;
  s.game_dir = ReadIni(L"game_dir", L"");
  s.gpu = ReadIni(L"gpu", L"");
  s.user_name = LoadGamertag();
  if (s.user_name.empty()) {
    s.user_name = ReadIni(L"user_name", L""); // from before gamertag.txt
  }
  s.shader_format = std::clamp(_wtoi(ReadIni(L"shader_format", L"0").c_str()), 0, 2);
  s.resolution = std::clamp(_wtoi(ReadIni(L"resolution", L"0").c_str()), 0, int(std::size(kResolutions)) - 1);
  s.aspect = std::clamp(_wtoi(ReadIni(L"aspect", L"1").c_str()), 0, int(std::size(kAspects)) - 1);
  s.frame_rate = std::clamp(_wtoi(ReadIni(L"frame_rate", L"1").c_str()), 0, int(std::size(kFrameRates)) - 1);
  s.fullscreen = ReadIni(L"fullscreen", L"1") != L"0";
  s.large_audio_buffer = ReadIni(L"large_audio_buffer", L"0") != L"0";
  s.close_on_play = ReadIni(L"close_on_play", L"1") != L"0";
  g.session_open = ReadIni(L"session_open", L"0") != L"0";
  g.session_t = g.session_open ? 1.0f : 0.0f;
  std::vector<wchar_t> section(32768);
  const DWORD n = GetPrivateProfileSectionW(L"cvars", section.data(), DWORD(section.size()), IniPath().c_str());
  for (const wchar_t *p = section.data(); p < section.data() + n && *p; p += wcslen(p) + 1) {
    const std::wstring line = p;
    const size_t eq = line.find(L'=');
    if (eq != std::wstring::npos && eq > 0) {
      g.overrides.push_back({line.substr(0, eq), line.substr(eq + 1)});
    }
  }
}

void SaveSettings() {
  const Settings &s = g.settings;
  const std::wstring ini = IniPath();
  WritePrivateProfileStringW(L"launcher", L"game_dir", s.game_dir.c_str(), ini.c_str());
  WritePrivateProfileStringW(L"launcher", L"gpu", s.gpu.c_str(), ini.c_str());
  WritePrivateProfileStringW(L"launcher", L"user_name", s.user_name.c_str(), ini.c_str());
  SaveGamertag(s.user_name);
  WritePrivateProfileStringW(L"launcher", L"shader_format", std::to_wstring(s.shader_format).c_str(), ini.c_str());
  WritePrivateProfileStringW(L"launcher", L"resolution", std::to_wstring(s.resolution).c_str(), ini.c_str());
  WritePrivateProfileStringW(L"launcher", L"aspect", std::to_wstring(s.aspect).c_str(), ini.c_str());
  WritePrivateProfileStringW(L"launcher", L"frame_rate", std::to_wstring(s.frame_rate).c_str(), ini.c_str());
  WritePrivateProfileStringW(L"launcher", L"fullscreen", s.fullscreen ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"launcher", L"large_audio_buffer", s.large_audio_buffer ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"launcher", L"close_on_play", s.close_on_play ? L"1" : L"0", ini.c_str());
  WritePrivateProfileStringW(L"launcher", L"session_open", g.session_open ? L"1" : L"0", ini.c_str());
  std::wstring section;
  for (const auto &o : g.overrides) {
    section += o.name + L"=" + o.value;
    section.push_back(L'\0');
  }
  section.push_back(L'\0');
  WritePrivateProfileSectionW(L"cvars", section.c_str(), ini.c_str());
}

// ---------------------------------------------------------------------------
// Cvar list: the game writes it (--dump_cvars), cached beside the launcher
// until the game executable changes.

const CvarDef *FindCvar(const std::wstring &name) {
  for (const auto &d : g.cvar_defs) {
    if (d.name == name) {
      return &d;
    }
  }
  return nullptr;
}

bool IsManaged(const std::wstring &name) {
  return std::any_of(std::begin(kManagedCvars), std::end(kManagedCvars),
                     [&](const wchar_t *m) { return name == m; });
}

std::vector<std::wstring> SplitTabs(const std::wstring &line) {
  std::vector<std::wstring> out(1);
  for (wchar_t c : line) {
    if (c == L'\t') {
      out.emplace_back();
    } else {
      out.back().push_back(c);
    }
  }
  return out;
}


std::vector<CvarDef> ParseCvars(const fs::path &path) {
  std::vector<CvarDef> defs;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    const auto f = SplitTabs(Widen(line));
    if (f.size() < 7 || IsManaged(f[0])) {
      continue;
    }
    CvarDef d{f[0], f[1], f[2], f[3], f[6]};
    d.debug = f[5] == L"1";
    for (size_t start = 0; !f[4].empty() && start <= f[4].size();) {
      const size_t bar = f[4].find(L'|', start);
      d.allowed.push_back(f[4].substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start));
      if (bar == std::wstring::npos) {
        break;
      }
      start = bar + 1;
    }
    defs.push_back(std::move(d));
  }
  std::sort(defs.begin(), defs.end(), [](const CvarDef &a, const CvarDef &b) {
    return a.category != b.category ? a.category < b.category : a.name < b.name;
  });
  return defs;
}

void LoadCvarsAsync() {
  if (g.cvars_loading) {
    return;
  }
  g.cvars_loading = true;
  const fs::path exe = ExeDir() / kGameExe, cache = ExeDir() / kCvarCacheFile;
  const std::wstring game = IsGameDir(GameDir()) ? GameDir().wstring() : ExeDir().wstring();
  HWND wnd = g.wnd;
  std::thread([exe, cache, game, wnd] {
    std::error_code ec;
    auto defs = new std::vector<CvarDef>;
    const bool fresh = fs::exists(cache, ec) && fs::exists(exe, ec) &&
                       fs::last_write_time(cache, ec) >= fs::last_write_time(exe, ec);
    if (!fresh && fs::exists(exe, ec)) {
      std::wstring cmd = L"\"" + exe.wstring() + L"\" --dump_cvars \"" + cache.wstring() + L"\" --game_data_root \"" +
                         game + L"\"";
      STARTUPINFOW si{sizeof(si)};
      PROCESS_INFORMATION pi{};
      if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                         exe.parent_path().c_str(), &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 20000) == WAIT_TIMEOUT) {
          TerminateProcess(pi.hProcess, 1);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
      }
    }
    *defs = ParseCvars(cache);
    PostMessageW(wnd, kMsgCvarsLoaded, 0, LPARAM(defs));
  }).detach();
}

// ---------------------------------------------------------------------------
// Drawing helpers

void RoundPath(GraphicsPath &path, const RectF &r, float radius) {
  const float d = std::min(radius * 2, std::min(r.Width, r.Height));
  if (d <= 0) {
    path.AddRectangle(r);
    return;
  }
  path.AddArc(r.X, r.Y, d, d, 180, 90);
  path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
  path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
  path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
  path.CloseFigure();
}

void FillRound(Graphics &gr, const RectF &r, float radius, const Color &fill, const Color *border = nullptr) {
  GraphicsPath path;
  RoundPath(path, r, radius);
  SolidBrush brush(fill);
  gr.FillPath(&brush, &path);
  if (border) {
    Pen pen(*border, 1.0f);
    gr.DrawPath(&pen, &path);
  }
}

enum Align { kLeft, kCenter, kRight };

void DrawText(Graphics &gr, const std::wstring &text, const Font *font, const Color &color, const RectF &r,
              Align align = kLeft, bool vcenter = true, bool wrap = false) {
  StringFormat fmt;
  fmt.SetAlignment(align == kLeft ? StringAlignmentNear : align == kCenter ? StringAlignmentCenter
                                                                           : StringAlignmentFar);
  fmt.SetLineAlignment(vcenter ? StringAlignmentCenter : StringAlignmentNear);
  fmt.SetTrimming(StringTrimmingEllipsisCharacter);
  if (!wrap) {
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
  }
  SolidBrush brush(color);
  gr.DrawString(text.c_str(), int(text.size()), font, r, &fmt, &brush);
}

float TextWidth(Graphics &gr, const std::wstring &text, const Font *font) {
  RectF box;
  gr.MeasureString(text.c_str(), int(text.size()), font, PointF(0, 0), &box);
  return box.Width;
}

bool Hovered(Action a, int index = -1) { return g.hover_action == a && (index < 0 || g.hover_index == index); }

void AddHit(const RectF &r, Action a, int index = -1) { g.hits.push_back({r, a, index}); }

void Button(Graphics &gr, const RectF &r, const std::wstring &text, Action a, int index = -1, bool primary = false,
            bool enabled = true) {
  const bool hot = enabled && Hovered(a, index);
  if (primary) {
    const Color fill = !enabled ? Color(0xFF, 0x3A, 0x41, 0x47) : hot ? kAccentHover : kAccent;
    FillRound(gr, r, 7, fill);
    DrawText(gr, text, r.Height > 44 ? g.f_play.get() : g.f_button.get(),
             enabled ? Color(0xFF, 0xFF, 0xFF, 0xFF) : kDim, r, kCenter);
  } else {
    FillRound(gr, r, 7, hot ? kButtonHover : kButton, &kButtonBorder);
    DrawText(gr, text, g.f_button.get(), enabled ? kText : kDim, r, kCenter);
  }
  if (enabled) {
    AddHit(r, a, index);
  }
}

// iOS-style switch.
void Toggle(Graphics &gr, float x, float y, bool on, Action a, int index = -1) {
  const RectF r(x, y, 46, 26);
  FillRound(gr, r, 13, on ? kAccent : Color(0xFF, 0x3A, 0x41, 0x47));
  SolidBrush knob(Color(0xFF, 0xFF, 0xFF, 0xFF));
  gr.FillEllipse(&knob, on ? x + 23 : x + 3, y + 3, 20.0f, 20.0f);
  AddHit(RectF(x - 4, y - 4, 54, 34), a, index);
}

void ToggleRow(Graphics &gr, float y, const std::wstring &title, const std::wstring &detail, bool on, Action a) {
  const RectF row(kInnerX, y, kInnerW, 58);
  if (Hovered(a)) {
    FillRound(gr, row, 7, Color(0x30, 0xFF, 0xFF, 0xFF));
  }
  DrawText(gr, title, g.f_bold.get(), kText, RectF(kInnerX + 14, y + 8, 700, 22), kLeft);
  DrawText(gr, detail, g.f_small.get(), kMuted, RectF(kInnerX + 14, y + 30, 760, 20), kLeft);
  Toggle(gr, kInnerX + kInnerW - 64, y + 16, on, a);
  AddHit(row, a);
}

void StatusIcon(Graphics &gr, float x, float y, int state) { // 0 ok, 1 pending, 2 missing
  const Color c = state == 0 ? kGood : state == 1 ? kWarn : kBad;
  SolidBrush brush(Color(0x33, c.GetR(), c.GetG(), c.GetB()));
  gr.FillEllipse(&brush, x, y, 24.0f, 24.0f);
  Pen pen(c, 2.2f);
  pen.SetStartCap(LineCapRound);
  pen.SetEndCap(LineCapRound);
  if (state == 0) {
    PointF pts[] = {{x + 7, y + 12.5f}, {x + 10.5f, y + 16}, {x + 17, y + 8.5f}};
    gr.DrawLines(&pen, pts, 3);
  } else if (state == 1) {
    gr.DrawLine(&pen, x + 12, y + 7, x + 12, y + 13);
    gr.DrawLine(&pen, x + 12, y + 17, x + 12, y + 17.2f);
  } else {
    gr.DrawLine(&pen, x + 8, y + 8, x + 16, y + 16);
    gr.DrawLine(&pen, x + 16, y + 8, x + 8, y + 16);
  }
}

// ---------------------------------------------------------------------------
// Background: art scaled to cover the window, cross-faded, shaded so the
// panels read (gradients from the PGR4Recomp launcher).

std::unique_ptr<Bitmap> LoadArt(int id) {
  HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); // RT_RCDATA
  if (!res) {
    return nullptr;
  }
  HGLOBAL data = LoadResource(nullptr, res);
  const DWORD size = SizeofResource(nullptr, res);
  IStream *stream = SHCreateMemStream(static_cast<const BYTE *>(LockResource(data)), size);
  if (!stream) {
    return nullptr;
  }
  std::unique_ptr<Bitmap> bmp(Bitmap::FromStream(stream));
  stream->Release();
  if (!bmp || bmp->GetLastStatus() != Ok) {
    return nullptr;
  }
  return bmp;
}

void DrawCover(Graphics &gr, Bitmap *img, int w, int h, float alpha) {
  if (!img || alpha <= 0) {
    return;
  }
  const float iw = float(img->GetWidth()), ih = float(img->GetHeight());
  const float s = std::max(w / iw, h / ih);
  const float dw = iw * s, dh = ih * s;
  ImageAttributes attrs;
  ColorMatrix cm = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, alpha, 0}, {0, 0, 0, 0, 1}}};
  attrs.SetColorMatrix(&cm);
  gr.DrawImage(img, RectF((w - dw) / 2, (h - dh) / 2, dw, dh), 0, 0, iw, ih, UnitPixel,
               alpha < 1.0f ? &attrs : nullptr);
}

void ComposeBackdrop(int w, int h, float fade) {
  if (g.backdrop && g.backdrop_w == w && g.backdrop_h == h && g.backdrop_fade == fade) {
    return;
  }
  g.backdrop = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
  g.backdrop_w = w;
  g.backdrop_h = h;
  g.backdrop_fade = fade;
  Graphics gr(g.backdrop.get());
  gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
  gr.Clear(Color(0xFF, 0x0A, 0x0C, 0x0E));
  if (!g.art.empty()) {
    DrawCover(gr, g.art[size_t(g.art_index)].get(), w, h, 1.0f);
    if (fade > 0) {
      DrawCover(gr, g.art[size_t(g.art_index + 1) % g.art.size()].get(), w, h, fade);
    }
  }
  LinearGradientBrush across(PointF(0, 0), PointF(float(w), 0), Color(0xDD, 0x08, 0x0A, 0x0C),
                             Color(0x55, 0x00, 0x00, 0x00));
  gr.FillRectangle(&across, 0, 0, w, h);
  LinearGradientBrush down(PointF(0, 0), PointF(0, float(h)), Color(0x22, 0x00, 0x00, 0x00),
                           Color(0xCC, 0x08, 0x0A, 0x0C));
  gr.FillRectangle(&down, 0, 0, w, h);
}

// ---------------------------------------------------------------------------
// Child edit controls (text entry); everything else is painted.

HWND MakeEdit(const std::wstring &text, int id) {
  HWND e = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, g.wnd,
                           HMENU(intptr_t(id)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(e, WM_SETFONT, WPARAM(g.edit_font), TRUE);
  return e;
}

void PlaceEdit(HWND e, const RectF &r, bool visible) {
  if (!visible) {
    ShowWindow(e, SW_HIDE);
    return;
  }
  // The field box is painted; the edit sits inside it, vertically centred.
  const int pad = int(10 * g.scale), h = int(22 * g.scale);
  const RECT want{int(r.X * g.scale) + pad, int((r.Y + r.Height / 2) * g.scale) - h / 2, 0, 0};
  const int w = int(r.Width * g.scale) - pad * 2;
  RECT now;
  GetWindowRect(e, &now);
  MapWindowPoints(nullptr, g.wnd, reinterpret_cast<POINT *>(&now), 2);
  if (now.left != want.left || now.top != want.top || now.right - now.left != w || now.bottom - now.top != h) {
    SetWindowPos(e, nullptr, want.left, want.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (!IsWindowVisible(e)) {
    ShowWindow(e, SW_SHOWNA);
  }
}

void FieldBox(Graphics &gr, const RectF &r, bool focused) {
  const Color border = focused ? kAccent : Color(0xFF, 0x30, 0x35, 0x3A);
  FillRound(gr, r, 6, kField, &border);
}

constexpr int kIdGameDirEdit = 100, kIdSearchEdit = 101, kIdUserNameEdit = 102, kIdCvarEditBase = 1000;

void RebuildCvarEdits() {
  for (auto &o : g.overrides) {
    if (o.edit) {
      DestroyWindow(o.edit);
      o.edit = nullptr;
    }
  }
  for (size_t i = 0; i < g.overrides.size(); ++i) {
    CvarOverride &o = g.overrides[i];
    const CvarDef *d = FindCvar(o.name);
    const bool text = !d || (d->type != L"bool" && d->allowed.empty());
    if (text) {
      o.edit = MakeEdit(o.value, kIdCvarEditBase + int(i));
    }
  }
}

// ---------------------------------------------------------------------------
// Command line

std::wstring Quote(const std::wstring &v) {
  if (!v.empty() && v.find_first_of(L" \t\"") == std::wstring::npos) {
    return v;
  }
  return L"\"" + v + L"\"";
}

// Everything after the launcher's own path on its command line, as given.
std::wstring LauncherArguments() {
  const wchar_t *p = GetCommandLineW();
  if (*p == L'"') {
    for (++p; *p && *p != L'"'; ++p) {
    }
    if (*p) {
      ++p;
    }
  } else {
    while (*p && *p != L' ' && *p != L'\t') {
      ++p;
    }
  }
  while (*p == L' ' || *p == L'\t') {
    ++p;
  }
  std::wstring rest = p;
  while (!rest.empty() && (rest.back() == L' ' || rest.back() == L'\t')) {
    rest.pop_back();
  }
  return rest;
}

// The chosen aspect ratio; Auto is the monitor the launcher is on.
double DisplayAspect() {
  const int i = g.settings.aspect;
  if (i > 0) {
    return kAspects[i];
  }
  MONITORINFO mi{sizeof(mi)};
  if (GetMonitorInfoW(MonitorFromWindow(g.wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
    const LONG w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    if (w > 0 && h > 0) {
      return double(w) / double(h);
    }
  }
  return 16.0 / 9.0;
}

std::wstring BuildArguments() {
  const Settings &s = g.settings;
  std::wstring cmd = L"--game_data_root " + Quote(GameDir().wstring());
  cmd += L" --execute_unclipped_draw_vs_on_cpu";
  cmd += s.fullscreen ? L" --fullscreen=true" : L" --fullscreen=false";
  if (s.shader_format == 1) {
    cmd += L" --shader_format=dxil";
  } else if (s.shader_format == 2) {
    cmd += L" --shader_format=dxbc";
  }
  if (s.resolution > 0) {
    cmd += L" --internal_resolution=" + std::to_wstring(kResolutions[s.resolution]);
  }
  cmd += L" --frame_rate_limit=" + std::to_wstring(kFrameRates[s.frame_rate]);
  // Wider than 16:9: the game draws an ultrawide (Hor+) view.
  if (const double aspect = DisplayAspect(); aspect > 16.0 / 9.0 + 0.01) {
    wchar_t text[32];
    std::swprintf(text, 32, L"%.4f", aspect);
    cmd += L" --aspect_ratio=" + std::wstring(text);
    // A window opens at that shape (720 high) rather than at 16:9.
    if (!s.fullscreen) {
      cmd += L" --window_width=" + std::to_wstring(int(std::lround(720.0 * aspect))) + L" --window_height=720";
    }
  }
  // The game picks the first GPU itself; only another one is named.
  if (g.gpu_index > 0 && size_t(g.gpu_index) < g.gpus.size()) {
    cmd += L" --gpu_adapter " + Quote(g.gpus[size_t(g.gpu_index)].name);
  }
  if (s.large_audio_buffer) {
    cmd += L" --audio_maxqframes=24";
  }
  // The profile keeps its XUID, so the save stays with a new name.
  if (!s.user_name.empty()) {
    cmd += L" " + Quote(L"--user_name=" + s.user_name);
  }
  for (const auto &o : g.overrides) {
    if (!o.name.empty()) {
      cmd += L" " + Quote(L"--" + o.name + L"=" + o.value);
    }
  }
  // The launcher's own arguments (e.g. Steam launch options) go on to the game.
  if (const std::wstring extra = LauncherArguments(); !extra.empty()) {
    cmd += L" " + extra;
  }
  return cmd;
}

void Redraw();

// ---------------------------------------------------------------------------
// Pages

void PaintPlay(Graphics &gr) {
  float y = kCardY + 22;
  constexpr float kNameW = 240; // profile name column, right of the game folder
  const float name_x = kInnerX + kInnerW - kNameW;
  DrawText(gr, L"Game Data", g.f_bold.get(), kText, RectF(kInnerX, y, 400, 22), kLeft);
  DrawText(gr, L"Profile Name", g.f_bold.get(), kText, RectF(name_x, y, kNameW, 22), kLeft);
  y += 28;
  const RectF field(kInnerX, y, kInnerW - kNameW - 12 - 132, 42);
  FieldBox(gr, field, GetFocus() == g.game_dir_edit);
  PlaceEdit(g.game_dir_edit, field, true);
  Button(gr, RectF(name_x - 12 - 120, y, 120, 42), L"Browse...", kActBrowse);
  const RectF name_field(name_x, y, kNameW, 42);
  FieldBox(gr, name_field, GetFocus() == g.user_name_edit);
  PlaceEdit(g.user_name_edit, name_field, true);
  y += 50;
  DrawText(gr, L"Choose the folder with your extracted PGR4 files (it contains default.xex).", g.f_small.get(), kMuted,
           RectF(kInnerX + 2, y, kInnerW - kNameW - 12, 20), kLeft);
  DrawText(gr, L"Up to 15 characters; your save is kept.", g.f_small.get(), kMuted,
           RectF(name_x + 2, y, kNameW, 20), kLeft);
  y += 34;

  // Checklist.
  const RectF box(kInnerX, y, kInnerW, 4 * 40 + 16);
  FillRound(gr, box, 8, kInset, &kInsetBorder);
  std::error_code ec;
  const bool game_ok = IsGameDir(GameDir());
  const bool exe_ok = fs::is_regular_file(ExeDir() / kGameExe, ec);
  int dlc = 0;
  for (const auto &p : g.packages) {
    dlc += p.type == kTypeMarketplace;
  }
  struct Row {
    int state;
    std::wstring title, detail;
  } rows[4] = {
      {game_ok && exe_ok ? 0 : 2, L"Game files",
       !exe_ok ? L"pgr4_recompiled.exe is missing beside the launcher" : game_ok ? L"default.xex found"
                                                                                : L"Choose the game folder above"},
      {TitleUpdateInstalled() ? 0 : TitleUpdatePresent() ? 1 : 2, L"Title Update #2",
       TitleUpdateInstalled()  ? L"Installed"
       : TitleUpdatePresent() ? L"Installs when the game starts"
                              : L"Needed: add the TU package (TU_16L61VP_...) on the Content tab"},
      {0, L"DLC", dlc ? std::to_wstring(dlc) + (dlc == 1 ? L" pack" : L" packs") + L" (installed on start)"
                      : L"None added (optional)"},
      {g.gpus.empty() ? 2 : 0, L"Graphics",
       g.gpus.empty() ? L"No Direct3D 12 GPU found"
                      : g.gpus[size_t(g.gpu_index)].name + L"  \u00B7  " +
                            (g.settings.shader_format == 1   ? L"DXIL"
                             : g.settings.shader_format == 2 ? L"DXBC"
                             : g.gpus[size_t(g.gpu_index)].sm6 ? L"DXIL (auto)"
                                                               : L"DXBC (auto)") +
                            L"  ·  " + kResolutionNames[g.settings.resolution]},
  };
  for (int i = 0; i < 4; ++i) {
    const float ry = y + 8 + i * 40;
    StatusIcon(gr, kInnerX + 16, ry + 8, rows[i].state);
    DrawText(gr, rows[i].title, g.f_bold.get(), kText, RectF(kInnerX + 54, ry, 220, 40), kLeft);
    DrawText(gr, rows[i].detail, g.f_body.get(), kMuted, RectF(kInnerX + 250, ry, kInnerW - 270, 40), kLeft);
  }
  y += box.Height + 12;
  const LastSession &l = g.last;

  // "Last session": a row that expands into the stat tiles.
  const float target = g.session_open ? 1.0f : 0.0f;
  g.session_t += (target - g.session_t) * 0.22f;
  if (std::fabs(target - g.session_t) < 0.004f) {
    g.session_t = target;
  } else {
    g.animating = true;
  }
  const float t = g.session_t * g.session_t * (3 - 2 * g.session_t); // smoothstep
  const RectF head(kInnerX, y, kInnerW, 34);
  if (Hovered(kActToggleSession)) {
    FillRound(gr, head, 6, Color(0x22, 0xFF, 0xFF, 0xFF));
  }
  {
    // Chevron, turning from right to down as it opens.
    GraphicsState saved = gr.Save();
    gr.TranslateTransform(head.X + 16, head.Y + 17);
    gr.RotateTransform(90.0f * t);
    Pen pen(kMuted, 2.0f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    PointF chevron[] = {{-2.5f, -5}, {2.5f, 0}, {-2.5f, 5}};
    gr.DrawLines(&pen, chevron, 3);
    gr.Restore(saved);
  }
  DrawText(gr, L"LAST SESSION", g.f_badge.get(), kDim, RectF(head.X + 32, head.Y, 120, head.Height), kLeft);
  std::wstring summary = L"No sessions yet";
  if (l.found) {
    summary = l.when + L"   \u00B7   " + (l.avg_fps > 0 ? std::to_wstring(int(l.avg_fps + 0.5)) + L" fps   \u00B7   " : L"") +
              std::to_wstring(l.minutes) + L"m " + std::to_wstring(l.seconds) + L"s   \u00B7   " +
              (l.clean_exit ? L"clean exit" : L"crashed");
  }
  const BYTE summary_alpha = BYTE(255 * (1.0f - t));
  const Color summary_color = l.found && !l.clean_exit ? kBad : kMuted;
  DrawText(gr, summary, g.f_small.get(),
           Color(summary_alpha, summary_color.GetR(), summary_color.GetG(), summary_color.GetB()),
           RectF(head.X + 140, head.Y, head.Width - 150, head.Height), kLeft);
  if (l.found) {
    DrawText(gr, l.when, g.f_small.get(), Color(BYTE(255 * t), 0x77, 0x7F, 0x86),
             RectF(head.X + 140, head.Y, 300, head.Height), kLeft);
  }
  AddHit(head, kActToggleSession);
  y += head.Height + 6;

  if (t > 0.001f) {
    const float open_h = 62 * t;
    gr.SetClip(RectF(kInnerX - 2, y - 2, kInnerW + 4, open_h + 4));
    const float slide = (1.0f - t) * -18.0f;
    struct Tile {
      std::wstring value, label;
      Color color;
    } tiles[3] = {
        {l.found && l.avg_fps > 0 ? std::to_wstring(int(l.avg_fps + 0.5)) : L"\u2014", L"average fps", kText},
        {l.found ? std::to_wstring(l.minutes) + L"m " + std::to_wstring(l.seconds) + L"s" : L"\u2014",
         L"session length", kText},
        {!l.found ? L"\u2014" : l.clean_exit ? L"Clean exit" : L"Crashed", L"how it ended",
         !l.found ? kText : l.clean_exit ? kGood : kBad},
    };
    const float tw = (kInnerW - 150 - 2 * 12) / 3.0f;
    for (int i = 0; i < 3; ++i) {
      const RectF tile(kInnerX + i * (tw + 12), y + slide, tw, 62);
      FillRound(gr, tile, 8, kInset, &kInsetBorder);
      DrawText(gr, tiles[i].value, g.f_stat.get(), tiles[i].color, RectF(tile.X + 16, tile.Y + 4, tile.Width - 32, 34),
               kLeft);
      DrawText(gr, tiles[i].label, g.f_small.get(), kMuted, RectF(tile.X + 16, tile.Y + 36, tile.Width - 32, 20),
               kLeft);
    }
    if (t > 0.9f) {
      Button(gr, RectF(kInnerX + kInnerW - 138, y + 11, 138, 40), L"Open logs", kActOpenLogs, -1, false, l.found);
    }
    gr.ResetClip();
  }
}

void PaintContent(Graphics &gr) {
  float y = kCardY + 22;
  DrawText(gr, L"Title updates and DLC", g.f_bold.get(), kText, RectF(kInnerX, y, 500, 22), kLeft);
  DrawText(gr, L"Packages are copied into the game's Content folder and installed when the game starts.",
           g.f_small.get(), kMuted, RectF(kInnerX, y + 24, kInnerW, 20), kLeft);
  y += 56;
  const RectF list(kInnerX, y, kInnerW, 290);
  FillRound(gr, list, 8, kInset, &kInsetBorder);
  if (g.packages.empty()) {
    DrawText(gr, L"No packages yet.\nAdd Title Update #2 (TU_16L61VP_...) and any DLC packs you own.", g.f_body.get(),
             kMuted, list, kCenter, true, true);
  } else {
    gr.SetClip(list);
    const float row_h = 56;
    const int max_scroll = std::max(0, int(g.packages.size() * row_h - list.Height + 8));
    g.content_scroll = std::clamp(g.content_scroll, 0, max_scroll);
    for (size_t i = 0; i < g.packages.size(); ++i) {
      const Package &p = g.packages[i];
      const RectF row(list.X + 6, list.Y + 4 + i * row_h - g.content_scroll, list.Width - 12, row_h - 4);
      if (row.GetBottom() < list.Y || row.Y > list.GetBottom()) {
        continue;
      }
      const bool selected = int(i) == g.selected_package;
      if (selected || Hovered(kActSelectPackage, int(i))) {
        FillRound(gr, row, 6, selected ? Color(0x40, 0x4D, 0xA3, 0xFF) : Color(0x22, 0xFF, 0xFF, 0xFF));
      }
      const bool tu = p.type == kTypeTitleUpdate;
      const Color badge = tu ? kAccent : kDlc;
      const RectF chip(row.X + 12, row.Y + 15, 52, 22);
      FillRound(gr, chip, 11, Color(0x33, badge.GetR(), badge.GetG(), badge.GetB()));
      DrawText(gr, tu ? L"TU" : L"DLC", g.f_badge.get(), badge, chip, kCenter);
      DrawText(gr, p.title, g.f_bold.get(), kText, RectF(row.X + 80, row.Y, 480, row.Height), kLeft);
      DrawText(gr, p.path.filename().wstring(), g.f_small.get(), kMuted,
               RectF(row.X + 560, row.Y, row.Width - 576, row.Height), kRight);
      AddHit(row, kActSelectPackage, int(i));
    }
    gr.ResetClip();
  }
  y += list.Height + 16;
  Button(gr, RectF(kInnerX, y, 170, 40), L"Add packages...", kActAddPackages, -1, true);
  Button(gr, RectF(kInnerX + 182, y, 110, 40), L"Remove", kActRemovePackage, -1, false, g.selected_package >= 0);
  Button(gr, RectF(kInnerX + 304, y, 130, 40), L"Open folder", kActOpenContent);
}

void PaintGraphics(Graphics &gr) {
  float y = kCardY + 22;
  DrawText(gr, L"GPU", g.f_bold.get(), kText, RectF(kInnerX, y, 400, 22), kLeft);
  y += 28;
  const RectF gpu_box(kInnerX, y, kInnerW, 46);
  FillRound(gr, gpu_box, 7, Hovered(kActGpu) ? kButtonHover : kButton, &kButtonBorder);
  if (g.gpus.empty()) {
    DrawText(gr, L"No Direct3D 12 GPU found", g.f_body.get(), kBad, RectF(kInnerX + 16, y, 600, 46), kLeft);
  } else {
    const Gpu &gpu = g.gpus[size_t(g.gpu_index)];
    DrawText(gr, gpu.name, g.f_body.get(), kText, RectF(kInnerX + 16, y, 560, 46), kLeft);
    const std::wstring chip_text = gpu.d3d12 ? (gpu.sm6 ? L"SHADER MODEL 6" : L"SHADER MODEL 5.1") : L"NO D3D12";
    const float cw = TextWidth(gr, chip_text, g.f_badge.get()) + 20;
    const RectF chip(gpu_box.GetRight() - cw - 140, y + 12, cw, 22);
    const Color c = gpu.d3d12 ? (gpu.sm6 ? kGood : kWarn) : kBad;
    FillRound(gr, chip, 11, Color(0x33, c.GetR(), c.GetG(), c.GetB()));
    DrawText(gr, chip_text, g.f_badge.get(), c, chip, kCenter);
    DrawText(gr, std::to_wstring(gpu.vram >> 20) + L" MB  \u25BE", g.f_small.get(), kMuted,
             RectF(gpu_box.GetRight() - 130, y, 114, 46), kRight);
  }
  AddHit(gpu_box, kActGpu);
  y += 66;

  DrawText(gr, L"Shaders", g.f_bold.get(), kText, RectF(kInnerX, y, 400, 22), kLeft);
  y += 28;
  const wchar_t *labels[] = {L"Auto", L"DXIL  (SM 6)", L"DXBC  (SM 5.1)"};
  const RectF seg(kInnerX, y, 480, 42);
  FillRound(gr, seg, 8, kField, &kButtonBorder);
  for (int i = 0; i < 3; ++i) {
    const RectF part(seg.X + 4 + i * 158, seg.Y + 4, 154, 34);
    if (g.settings.shader_format == i) {
      FillRound(gr, part, 6, kAccent);
    } else if (Hovered(kActShader, i)) {
      FillRound(gr, part, 6, kButtonHover);
    }
    DrawText(gr, labels[i], g.f_button.get(), g.settings.shader_format == i ? Color(0xFF, 0xFF, 0xFF, 0xFF) : kText,
             part, kCenter);
    AddHit(part, kActShader, i);
  }
  std::wstring info = L"Auto picks DXIL on GPUs with shader model 6 (most since 2016, and the Steam Deck), "
                      L"else DXBC. Force one only if the game shows a white or broken screen.";
  DrawText(gr, info, g.f_small.get(), kMuted, RectF(kInnerX + 500, y - 4, kInnerW - 500, 50), kLeft, true, true);
  y += 62;

  // Internal resolution, aspect ratio and frame rate, side by side.
  const float aspect_x = kInnerX + 328, rate_x = kInnerX + 656;
  DrawText(gr, L"Internal Resolution", g.f_bold.get(), kText, RectF(kInnerX, y, 400, 22), kLeft);
  std::wstring aspect_title = L"Aspect Ratio";
  if (g.settings.aspect == 0) {
    wchar_t text[48];
    std::swprintf(text, 48, L"Aspect Ratio  (monitor %.2f:1)", DisplayAspect());
    aspect_title = text;
  }
  DrawText(gr, aspect_title, g.f_bold.get(), kText, RectF(aspect_x, y, 320, 22), kLeft);
  DrawText(gr, L"Frame Rate Limit", g.f_bold.get(), kText, RectF(rate_x, y, 312, 22), kLeft);
  y += 28;
  auto segments = [&](float x, const wchar_t *const *names, int count, int selected, Action action) {
    const RectF seg(x, y, 4 * 76 + 8, 42);
    FillRound(gr, seg, 8, kField, &kButtonBorder);
    for (int i = 0; i < count; ++i) {
      const RectF part(seg.X + 4 + i * 76, seg.Y + 4, 72, 34);
      if (selected == i) {
        FillRound(gr, part, 6, kAccent);
      } else if (Hovered(action, i)) {
        FillRound(gr, part, 6, kButtonHover);
      }
      DrawText(gr, names[i], g.f_button.get(), selected == i ? Color(0xFF, 0xFF, 0xFF, 0xFF) : kText, part, kCenter);
      AddHit(part, action, i);
    }
  };
  segments(kInnerX, kResolutionNames, int(std::size(kResolutions)), g.settings.resolution, kActResolution);
  segments(aspect_x, kAspectNames, int(std::size(kAspects)), g.settings.aspect, kActAspect);
  segments(rate_x, kFrameRateNames, int(std::size(kFrameRates)), g.settings.frame_rate, kActFrameRate);
  y += 48;
  DrawText(gr, L"Above 720p needs a much faster GPU (Steam Deck: stay at 720p). The 360 runs the game at 30 fps; "
               L"60 is the default.",
           g.f_small.get(), kMuted, RectF(kInnerX + 2, y, kInnerW, 20), kLeft);
  y += 32;
  ToggleRow(gr, y, L"Fullscreen", L"Start in fullscreen (Alt+Enter switches in game).", g.settings.fullscreen,
            kActFullscreen);
  y += 64;
  ToggleRow(gr, y, L"Larger audio buffer", L"Smooths crackling or choppy audio on slower PCs (adds ~85 ms latency).",
            g.settings.large_audio_buffer, kActAudio);
}

void PaintAdvanced(Graphics &gr) {
  float y = kCardY + 22;
  DrawText(gr, L"Cvars", g.f_bold.get(), kText, RectF(kInnerX, y, 400, 22), kLeft);
  DrawText(gr, L"Extra game settings passed on launch. Leave empty unless you know you need one.", g.f_small.get(),
           kMuted, RectF(kInnerX, y + 24, 700, 20), kLeft);
  Button(gr, RectF(kInnerX + kInnerW - 140, y + 2, 140, 40), L"+  Add cvar", kActAddCvar, -1, true);
  y += 60;
  const RectF list(kInnerX, y, kInnerW, kCardY + kCardH - y - 86);
  FillRound(gr, list, 8, kInset, &kInsetBorder);
  DrawText(gr, L"LAUNCH COMMAND", g.f_badge.get(), kDim, RectF(kInnerX + 2, list.GetBottom() + 10, 300, 16), kLeft);
  DrawText(gr, L"pgr4_recompiled.exe " + BuildArguments(), g.f_mono.get(), kMuted,
           RectF(kInnerX + 2, list.GetBottom() + 28, kInnerW, 40), kLeft, false, true);
  const float row_h = 64;
  const int max_scroll = std::max(0, int(g.overrides.size() * row_h - list.Height + 8));
  g.cvar_scroll = std::clamp(g.cvar_scroll, 0, max_scroll);
  if (g.overrides.empty()) {
    DrawText(gr, L"No cvars set. The defaults suit almost everyone.", g.f_body.get(), kMuted, list, kCenter);
  }
  gr.SetClip(list);
  for (size_t i = 0; i < g.overrides.size(); ++i) {
    CvarOverride &o = g.overrides[i];
    const CvarDef *d = FindCvar(o.name);
    const RectF row(list.X + 6, list.Y + 4 + i * row_h - g.cvar_scroll, list.Width - 12, row_h - 4);
    const bool visible = row.Y >= list.Y - 1 && row.GetBottom() <= list.GetBottom() + 1;
    if (i % 2) {
      FillRound(gr, row, 6, Color(0x12, 0xFF, 0xFF, 0xFF));
    }
    DrawText(gr, o.name, g.f_bold.get(), kText, RectF(row.X + 14, row.Y + 6, 520, 24), kLeft);
    DrawText(gr, d ? d->description : L"Not a cvar of this build", g.f_small.get(), d ? kMuted : kWarn,
             RectF(row.X + 14, row.Y + 30, 560, 22), kLeft);
    const RectF value(row.X + 600, row.Y + 12, 300, 36);
    if (d && d->type == L"bool") {
      const bool on = o.value == L"true" || o.value == L"1";
      Toggle(gr, value.X, value.Y + 5, on, kActCvarBool, int(i));
      DrawText(gr, on ? L"On" : L"Off", g.f_body.get(), kMuted, RectF(value.X + 58, value.Y, 80, 36), kLeft);
    } else if (d && !d->allowed.empty()) {
      FillRound(gr, value, 6, Hovered(kActCvarChoice, int(i)) ? kButtonHover : kButton, &kButtonBorder);
      DrawText(gr, (o.value.empty() ? d->default_value : o.value) + L"", g.f_body.get(), kText,
               RectF(value.X + 12, value.Y, value.Width - 40, value.Height), kLeft);
      DrawText(gr, L"\u25BE", g.f_body.get(), kMuted, RectF(value.GetRight() - 30, value.Y, 20, value.Height), kCenter);
      if (visible) {
        AddHit(value, kActCvarChoice, int(i));
      }
    } else {
      FieldBox(gr, value, o.edit && GetFocus() == o.edit);
    }
    if (o.edit) {
      PlaceEdit(o.edit, value, visible && g.tab == kTabAdvanced && !g.picker_open);
    }
    const RectF remove(row.GetRight() - 44, row.Y + 14, 32, 32);
    if (Hovered(kActRemoveCvar, int(i))) {
      FillRound(gr, remove, 6, Color(0x33, 0xFF, 0x5D, 0x5D));
    }
    DrawText(gr, L"\u2715", g.f_body.get(), Hovered(kActRemoveCvar, int(i)) ? kBad : kMuted, remove, kCenter);
    if (visible) {
      AddHit(remove, kActRemoveCvar, int(i));
    }
  }
  gr.ResetClip();
}

// Searchable cvar list over the Advanced page.
void PaintPicker(Graphics &gr) {
  SolidBrush dim(Color(0xA0, 0x05, 0x06, 0x08));
  gr.FillRectangle(&dim, 0, 0, kWidth, kHeight);
  AddHit(RectF(0, 0, kWidth, kHeight), kActClosePicker);
  const RectF panel(220, 90, 660, 540);
  FillRound(gr, panel, 12, Color(0xF5, 0x17, 0x1A, 0x1D), &kCardBorder);
  AddHit(panel, kActNone);
  DrawText(gr, L"Add a cvar", g.f_bold.get(), kText, RectF(panel.X + 24, panel.Y + 18, 400, 26), kLeft);
  const RectF close(panel.GetRight() - 52, panel.Y + 14, 34, 34);
  if (Hovered(kActClosePicker, 0)) {
    FillRound(gr, close, 6, kButtonHover);
  }
  DrawText(gr, L"\u2715", g.f_body.get(), kMuted, close, kCenter);
  AddHit(close, kActClosePicker, 0);
  const RectF search(panel.X + 24, panel.Y + 56, panel.Width - 48, 40);
  FieldBox(gr, search, true);
  PlaceEdit(g.search_edit, search, true);
  if (GetText(g.search_edit).empty()) {
    DrawText(gr, L"Search by name or description", g.f_body.get(), kDim,
             RectF(search.X + 12, search.Y, search.Width, search.Height), kLeft);
  }
  const RectF list(panel.X + 16, panel.Y + 108, panel.Width - 32, panel.Height - 124);
  if (g.cvars_loading) {
    DrawText(gr, L"Reading the game's cvar list...", g.f_body.get(), kMuted, list, kCenter);
    return;
  }
  if (g.cvar_defs.empty()) {
    DrawText(gr, L"The cvar list could not be read from pgr4_recompiled.exe.", g.f_body.get(), kWarn, list, kCenter);
    return;
  }
  gr.SetClip(list);
  const float row_h = 54;
  const int max_scroll = std::max(0, int(g.picker_matches.size() * row_h - list.Height));
  g.picker_scroll = std::clamp(g.picker_scroll, 0, max_scroll);
  for (size_t k = 0; k < g.picker_matches.size(); ++k) {
    const CvarDef &d = g.cvar_defs[size_t(g.picker_matches[k])];
    const RectF row(list.X, list.Y + k * row_h - g.picker_scroll, list.Width, row_h - 4);
    if (row.GetBottom() < list.Y || row.Y > list.GetBottom()) {
      continue;
    }
    if (Hovered(kActPickCvar, g.picker_matches[k])) {
      FillRound(gr, row, 6, Color(0x40, 0x4D, 0xA3, 0xFF));
    }
    DrawText(gr, d.name, g.f_bold.get(), kText, RectF(row.X + 12, row.Y + 4, 380, 24), kLeft);
    DrawText(gr, d.category + L"  \u00B7  " + d.type, g.f_small.get(), kDim,
             RectF(row.X + 300, row.Y + 4, row.Width - 312, 24), kRight);
    const std::wstring def = L"default: " + (d.default_value.empty() ? std::wstring(L"(empty)") : d.default_value);
    const float dw = std::min(TextWidth(gr, def, g.f_small.get()) + 4, 220.0f);
    DrawText(gr, d.description, g.f_small.get(), kMuted, RectF(row.X + 12, row.Y + 27, row.Width - dw - 36, 20),
             kLeft);
    DrawText(gr, def, g.f_small.get(), kDim, RectF(row.GetRight() - dw - 12, row.Y + 27, dw, 20), kRight);
    AddHit(row, kActPickCvar, g.picker_matches[k]);
  }
  gr.ResetClip();
}

void UpdatePickerMatches() {
  std::wstring q = GetText(g.search_edit);
  std::transform(q.begin(), q.end(), q.begin(), towlower);
  g.picker_matches.clear();
  for (size_t i = 0; i < g.cvar_defs.size(); ++i) {
    const CvarDef &d = g.cvar_defs[i];
    if (std::any_of(g.overrides.begin(), g.overrides.end(), [&](const CvarOverride &o) { return o.name == d.name; })) {
      continue;
    }
    std::wstring hay = d.name + L" " + d.description + L" " + d.category;
    std::transform(hay.begin(), hay.end(), hay.begin(), towlower);
    if (q.empty() || hay.find(q) != std::wstring::npos) {
      g.picker_matches.push_back(int(i));
    }
  }
  g.picker_scroll = 0;
}

RectF PlayRect() { return RectF(kCardX + kCardW - 200, kCardY + kCardH + 22, 200, 52); }

void PlayButtonFace(Graphics &gr, bool hot) {
  const RectF r = PlayRect();
  FillRound(gr, r, 7, hot ? kAccentHover : kAccent);
  DrawText(gr, L"Play", g.f_play.get(), Color(0xFF, 0xFF, 0xFF, 0xFF), r, kCenter);
}

void FocusRing(Graphics &gr, const RectF &r) {
  GraphicsPath path;
  RoundPath(path, RectF(r.X - 4, r.Y - 4, r.Width + 8, r.Height + 8), 9);
  Pen ring(Color(0xFF, 0xFF, 0xFF, 0xFF), 2.0f);
  gr.DrawPath(&ring, &path);
}

void RenderUI(int w, int h) {
  float fade = 0;
  if (g.fading) {
    fade = std::min(1.0f, (GetTickCount() - g.fade_start) / 900.0f);
    fade = fade * fade * (3 - 2 * fade);
  }
  ComposeBackdrop(w, h, fade);
  g.animating = false;
  if (!g.ui || int(g.ui->GetWidth()) != w || int(g.ui->GetHeight()) != h) {
    g.ui = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
  }
  Graphics gr(g.ui.get());
  gr.DrawImage(g.backdrop.get(), Rect(0, 0, w, h));
  gr.SetSmoothingMode(SmoothingModeAntiAlias);
  gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
  gr.SetPixelOffsetMode(PixelOffsetModeHalf);
  gr.ScaleTransform(g.scale, g.scale);
  g.hits.clear();

  // Header.
  DrawText(gr, L"PGR4-Recomp", g.f_title.get(), kText, RectF(40, 26, 600, 48), kLeft);
  DrawText(gr, L"Project Gotham Racing 4  \u00B7  native D3D12  \u00B7  Title Update 2", g.f_sub.get(), kMuted,
           RectF(42, 74, 700, 22), kLeft);
  const bool ready = g.ready = CanPlay();
  const std::wstring status = ready ? L"Ready" : L"Setup needed";
  const RectF pill(kWidth - 40 - 150, 30, 150, 36);
  const Color pill_border(0xFF, 0x33, 0x38, 0x3D);
  FillRound(gr, pill, 6, Color(0x88, 0x00, 0x00, 0x00), &pill_border);
  SolidBrush dot(ready ? kGood : kWarn);
  gr.FillEllipse(&dot, pill.X + 16, pill.Y + 14, 8.0f, 8.0f);
  DrawText(gr, status, g.f_button.get(), Color(0xFF, 0xDC, 0xE4, 0xE8), RectF(pill.X + 32, pill.Y, 110, 36), kLeft);

  // Tabs.
  float tx = 42;
  for (int i = 0; i < kTabCount; ++i) {
    const float tw = TextWidth(gr, kTabNames[i], g.f_tab.get()) + 8;
    const RectF r(tx - 4, 108, tw + 8, 34);
    const bool active = g.tab == i;
    DrawText(gr, kTabNames[i], g.f_tab.get(), active ? kText : Hovered(kActTab, i) ? Color(0xFF, 0xC8, 0xCE, 0xD3) : kMuted,
             r, kCenter);
    if (active) {
      if (g.tab_x < 0) {
        g.tab_x = tx, g.tab_w = tw;
      }
      g.tab_x += (tx - g.tab_x) * 0.3f;
      g.tab_w += (tw - g.tab_w) * 0.3f;
      if (std::fabs(tx - g.tab_x) > 0.5f || std::fabs(tw - g.tab_w) > 0.5f) {
        g.animating = true;
      } else {
        g.tab_x = tx, g.tab_w = tw;
      }
    }
    AddHit(r, kActTab, i);
    tx += tw + 26;
  }

  FillRound(gr, RectF(g.tab_x, 140, g.tab_w, 3), 1.5f, kAccent);

  // Card.
  FillRound(gr, RectF(kCardX, kCardY, kCardW, kCardH), 10, kCard, &kCardBorder);
  if (g.tab != kTabPlay) {
    ShowWindow(g.game_dir_edit, SW_HIDE);
    ShowWindow(g.user_name_edit, SW_HIDE);
  }
  if (g.tab != kTabAdvanced || g.picker_open) {
    for (auto &o : g.overrides) {
      if (o.edit) {
        ShowWindow(o.edit, SW_HIDE);
      }
    }
  }
  switch (g.tab) {
  case kTabPlay:
    PaintPlay(gr);
    break;
  case kTabContent:
    PaintContent(gr);
    break;
  case kTabGraphics:
    PaintGraphics(gr);
    break;
  case kTabAdvanced:
    PaintAdvanced(gr);
    break;
  default:
    break;
  }

  // Footer.
  const float fy = kCardY + kCardH + 22;
  Toggle(gr, kCardX, fy + 13, g.settings.close_on_play, kActCloseOnPlay);
  DrawText(gr, L"Close the launcher when the game starts", g.f_body.get(), kMuted, RectF(kCardX + 58, fy, 360, 52),
           kLeft);
  AddHit(RectF(kCardX, fy, 400, 52), kActCloseOnPlay);
  Button(gr, RectF(kCardX + kCardW - 360, fy + 4, 140, 44), L"Game folder", kActOpenGameFolder, -1, false,
         IsGameDir(GameDir()));
  Button(gr, PlayRect(), L"Play", kActPlay, -1, true, ready);

  if (g.picker_open) {
    PaintPicker(gr);
  } else {
    ShowWindow(g.search_edit, SW_HIDE);
  }
  if (g.focus_visible) {
    for (const Hit &h : g.hits) {
      if (h.action == g.focus_action && h.index == g.focus_index) {
        FocusRing(gr, h.rect);
        break;
      }
    }
  }
}

// Paints the dirty part of the window: the cached UI, plus the Play glow (a
// slow pulse while the game can start) redrawn over it each frame.
void Paint(HDC dc, const RECT &clip) {
  RECT rc;
  GetClientRect(g.wnd, &rc);
  const int w = rc.right, h = rc.bottom;
  if (g.dirty || !g.ui || int(g.ui->GetWidth()) != w || int(g.ui->GetHeight()) != h) {
    g.dirty = false;
    RenderUI(w, h);
  }
  const bool ready = g.ready;
  const int cx = std::max<LONG>(clip.left, 0), cy = std::max<LONG>(clip.top, 0);
  const int cw = std::min<LONG>(clip.right, w) - cx, ch = std::min<LONG>(clip.bottom, h) - cy;
  if (cw <= 0 || ch <= 0) {
    return;
  }
  Bitmap frame(cw, ch, PixelFormat32bppPARGB);
  {
    Graphics gr(&frame);
    gr.DrawImage(g.ui.get(), Rect(-cx, -cy, w, h));
    if (ready && !g.picker_open) {
      gr.SetSmoothingMode(SmoothingModeAntiAlias);
      gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
      gr.SetPixelOffsetMode(PixelOffsetModeHalf);
      gr.TranslateTransform(float(-cx), float(-cy));
      gr.ScaleTransform(g.scale, g.scale);
      const RectF play = PlayRect();
      const float t = (GetTickCount() % 2400) / 2400.0f;
      const float pulse = 0.5f - 0.5f * std::cos(t * 6.2831853f);
      for (int i = 4; i >= 1; --i) {
        const float grow = i * (2.0f + 2.0f * pulse);
        FillRound(gr, RectF(play.X - grow, play.Y - grow, play.Width + 2 * grow, play.Height + 2 * grow), 7 + grow,
                  Color(BYTE(10 + 8 * pulse), 0x4D, 0xA3, 0xFF));
      }
      PlayButtonFace(gr, Hovered(kActPlay));
      if (g.focus_visible && g.focus_action == kActPlay) {
        FocusRing(gr, play);
      }
    }
  }
  Graphics screen(dc);
  screen.DrawImage(&frame, Rect(cx, cy, cw, ch));

  // Keep animating while the underline slides or the Play glow can be seen.
  static int timer_ms = 0;
  const bool glow = ready && GetForegroundWindow() == g.wnd && !IsIconic(g.wnd);
  const int want = g.animating ? 16 : glow ? 50 : 0;
  if (want != timer_ms) {
    timer_ms = want;
    if (want) {
      SetTimer(g.wnd, 4, UINT(want), nullptr);
    } else {
      KillTimer(g.wnd, 4);
    }
  }
}

// ---------------------------------------------------------------------------
// Actions

void Redraw() {
  g.dirty = true;
  InvalidateRect(g.wnd, nullptr, FALSE);
}

// Glow frame: only the area around the Play button is repainted.
void RedrawGlow() {
  const RectF r = PlayRect();
  const RECT area{LONG((r.X - 16) * g.scale), LONG((r.Y - 16) * g.scale), LONG((r.GetRight() + 16) * g.scale) + 1,
                  LONG((r.GetBottom() + 16) * g.scale) + 1};
  InvalidateRect(g.wnd, &area, FALSE);
}

bool PickFiles(bool folder, std::vector<fs::path> &out) {
  IFileOpenDialog *dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    return false;
  }
  DWORD options = 0;
  dlg->GetOptions(&options);
  dlg->SetOptions(options | FOS_FORCEFILESYSTEM |
                  (folder ? FOS_PICKFOLDERS : FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST));
  dlg->SetTitle(folder ? L"Choose the PGR4 game folder (with default.xex)" : L"Choose title update / DLC packages");
  if (SUCCEEDED(dlg->Show(g.wnd))) {
    IShellItemArray *items = nullptr;
    if (SUCCEEDED(dlg->GetResults(&items))) {
      DWORD count = 0;
      items->GetCount(&count);
      for (DWORD i = 0; i < count; ++i) {
        IShellItem *item = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          out.emplace_back(path);
          CoTaskMemFree(path);
        }
        if (item) {
          item->Release();
        }
      }
      items->Release();
    }
  }
  dlg->Release();
  return !out.empty();
}

void AddPackages() {
  if (!IsGameDir(GameDir())) {
    MessageBoxW(g.wnd, L"Choose the game folder first (Play tab).", L"PGR4-Recomp", MB_ICONINFORMATION);
    return;
  }
  std::vector<fs::path> picked;
  if (!PickFiles(false, picked)) {
    return;
  }
  std::wstring rejected, from;
  bool new_tu = false;
  int added = 0;
  std::error_code ec;
  fs::create_directories(ContentDir(), ec);
  for (const fs::path &path : picked) {
    Package p;
    std::wstring error;
    if (!ReadPackage(path, p, error)) {
      rejected += L"\n\u2022 " + path.filename().wstring() + L" " + error;
      continue;
    }
    if (fs::equivalent(path.parent_path(), ContentDir(), ec)) {
      continue;
    }
    from += path.wstring();
    from.push_back(L'\0');
    new_tu |= p.type == kTypeTitleUpdate;
    ++added;
  }
  if (added) {
    // SHFileOperation shows the system copy progress (DLC packs are large).
    from.push_back(L'\0');
    std::wstring to = ContentDir().wstring();
    to.append(2, L'\0');
    SHFILEOPSTRUCTW op{};
    op.hwnd = g.wnd;
    op.wFunc = FO_COPY;
    op.pFrom = from.c_str();
    op.pTo = to.c_str();
    op.fFlags = FOF_NOCONFIRMMKDIR;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted) {
      rejected += L"\n\u2022 Copying was cancelled or failed.";
    }
    // A new title update replaces the extracted one at the next start.
    if (new_tu) {
      fs::remove_all(GameDir() / "TitleUpdate", ec);
    }
  }
  RefreshPackages();
  Redraw();
  if (!rejected.empty()) {
    MessageBoxW(g.wnd, (L"Some files were not added:" + rejected).c_str(), L"PGR4-Recomp", MB_ICONWARNING);
  }
}

void ApplyGameDir() {
  g.settings.game_dir = GetText(g.game_dir_edit);
  RefreshPackages();
  Redraw();
}

void ChooseGpu() {
  if (g.gpus.empty()) {
    return;
  }
  HMENU menu = CreatePopupMenu();
  for (size_t i = 0; i < g.gpus.size(); ++i) {
    const Gpu &gpu = g.gpus[i];
    const std::wstring label = gpu.name + L"\t" + (gpu.d3d12 ? (gpu.sm6 ? L"SM 6" : L"SM 5.1") : L"no D3D12");
    AppendMenuW(menu, MF_STRING | (int(i) == g.gpu_index ? MF_CHECKED : 0), i + 1, label.c_str());
  }
  POINT pt{LONG((kInnerX)*g.scale), LONG((kCardY + 22 + 28 + 46) * g.scale)};
  ClientToScreen(g.wnd, &pt);
  const int pick = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x, pt.y, 0, g.wnd, nullptr);
  DestroyMenu(menu);
  if (pick > 0) {
    g.gpu_index = pick - 1;
    g.settings.gpu = g.gpus[size_t(g.gpu_index)].name;
  }
}

void ChooseCvarValue(int index) {
  CvarOverride &o = g.overrides[size_t(index)];
  const CvarDef *d = FindCvar(o.name);
  if (!d) {
    return;
  }
  HMENU menu = CreatePopupMenu();
  for (size_t i = 0; i < d->allowed.size(); ++i) {
    AppendMenuW(menu, MF_STRING | (d->allowed[i] == o.value ? MF_CHECKED : 0), i + 1, d->allowed[i].c_str());
  }
  POINT pt;
  GetCursorPos(&pt);
  const int pick = TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, g.wnd, nullptr);
  DestroyMenu(menu);
  if (pick > 0) {
    o.value = d->allowed[size_t(pick - 1)];
  }
}

void OpenPicker() {
  g.picker_open = true;
  SetWindowTextW(g.search_edit, L"");
  if (g.cvar_defs.empty()) {
    LoadCvarsAsync();
  }
  UpdatePickerMatches();
  Redraw();
  SetFocus(g.search_edit);
}

void ClosePicker() {
  g.picker_open = false;
  ShowWindow(g.search_edit, SW_HIDE);
  SetFocus(g.wnd);
}

void Play() {
  if (!CanPlay()) {
    return;
  }
  g.settings.game_dir = GetText(g.game_dir_edit);
  SaveSettings();
  std::wstring cmd = Quote((ExeDir() / kGameExe).wstring()) + L" " + BuildArguments();
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  const std::wstring dir = ExeDir().wstring();
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
    MessageBoxW(g.wnd, L"pgr4_recompiled.exe could not be started.", L"PGR4-Recomp", MB_ICONERROR);
    return;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (g.settings.close_on_play) {
    DestroyWindow(g.wnd);
  }
}

void SetTab(int tab) {
  g.tab = Tab((tab + kTabCount) % kTabCount);
  if (g.tab == kTabContent) {
    RefreshPackages();
  }
  SetFocus(g.wnd);
  Redraw();
}

void Activate(Action a, int index) {
  switch (a) {
  case kActTab:
    SetTab(index);
    return;
  case kActBrowse: {
    std::vector<fs::path> dir;
    if (PickFiles(true, dir)) {
      SetWindowTextW(g.game_dir_edit, dir.front().c_str());
      ApplyGameDir();
    }
    break;
  }
  case kActAddPackages:
    AddPackages();
    break;
  case kActRemovePackage:
    if (g.selected_package >= 0 && size_t(g.selected_package) < g.packages.size()) {
      const Package &p = g.packages[size_t(g.selected_package)];
      if (MessageBoxW(g.wnd, (L"Remove \"" + p.title + L"\" from the Content folder?").c_str(), L"PGR4-Recomp",
                      MB_YESNO | MB_ICONQUESTION) == IDYES) {
        std::error_code ec;
        fs::remove(p.path, ec);
        g.selected_package = -1;
        RefreshPackages();
      }
    }
    break;
  case kActOpenContent: {
    std::error_code ec;
    fs::create_directories(ContentDir(), ec);
    ShellExecuteW(g.wnd, L"open", ContentDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    break;
  }
  case kActToggleSession:
    g.session_open = !g.session_open;
    break;
  case kActOpenLogs: {
    const fs::path logs = ExeDir() / "logs";
    ShellExecuteW(g.wnd, L"open", logs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    break;
  }
  case kActOpenGameFolder:
    ShellExecuteW(g.wnd, L"open", GameDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    break;
  case kActSelectPackage:
    g.selected_package = index;
    break;
  case kActGpu:
    ChooseGpu();
    break;
  case kActShader:
    g.settings.shader_format = index;
    break;
  case kActResolution:
    g.settings.resolution = index;
    break;
  case kActAspect:
    g.settings.aspect = index;
    break;
  case kActFrameRate:
    g.settings.frame_rate = index;
    break;
  case kActFullscreen:
    g.settings.fullscreen = !g.settings.fullscreen;
    break;
  case kActAudio:
    g.settings.large_audio_buffer = !g.settings.large_audio_buffer;
    break;
  case kActCloseOnPlay:
    g.settings.close_on_play = !g.settings.close_on_play;
    break;
  case kActPlay:
    Play();
    return;
  case kActAddCvar:
    OpenPicker();
    return;
  case kActRemoveCvar:
    if (g.overrides[size_t(index)].edit) {
      DestroyWindow(g.overrides[size_t(index)].edit);
    }
    g.overrides.erase(g.overrides.begin() + index);
    RebuildCvarEdits();
    break;
  case kActCvarBool: {
    CvarOverride &o = g.overrides[size_t(index)];
    o.value = (o.value == L"true" || o.value == L"1") ? L"false" : L"true";
    break;
  }
  case kActCvarChoice:
    ChooseCvarValue(index);
    break;
  case kActPickCvar: {
    const CvarDef &d = g.cvar_defs[size_t(index)];
    g.overrides.push_back({d.name, d.type == L"bool" ? (d.default_value == L"true" ? L"false" : L"true")
                                                     : d.default_value});
    ClosePicker();
    RebuildCvarEdits();
    g.cvar_scroll = 1 << 20; // show the new row
    break;
  }
  case kActClosePicker:
    if (index == 0 || !g.picker_open) {
      ClosePicker();
    } else {
      ClosePicker(); // click outside the panel
    }
    break;
  default:
    return;
  }
  SaveSettings();
  Redraw();
}

const Hit *HitAt(int x, int y) {
  const float lx = x / g.scale, ly = y / g.scale;
  for (auto it = g.hits.rbegin(); it != g.hits.rend(); ++it) {
    if (it->rect.Contains(lx, ly)) {
      return &*it;
    }
  }
  return nullptr;
}

// Keyboard / controller focus: moves to the nearest control in the pressed
// direction (lists scroll when nothing is further that way).
bool Focusable(const Hit &h) {
  return h.action != kActNone && !(h.action == kActClosePicker && h.index != 0);
}

const Hit *FocusedHit() {
  for (const Hit &h : g.hits) {
    if (h.action == g.focus_action && h.index == g.focus_index) {
      return &h;
    }
  }
  return nullptr;
}

void SetFocusTo(const Hit &h) {
  g.focus_action = h.action;
  g.focus_index = h.index;
}

void ResetFocus() {
  for (const Hit &h : g.hits) {
    if (h.action == (g.picker_open ? kActPickCvar : kActPlay) && Focusable(h)) {
      SetFocusTo(h);
      return;
    }
  }
  for (const Hit &h : g.hits) {
    if (Focusable(h)) {
      SetFocusTo(h);
      return;
    }
  }
}

void MoveFocus(int dx, int dy) {
  const bool was_visible = g.focus_visible;
  g.focus_visible = true;
  const Hit *cur = FocusedHit();
  if (!cur || !was_visible) {
    if (!cur) {
      ResetFocus();
    }
    Redraw();
    return;
  }
  const float cx = cur->rect.X + cur->rect.Width / 2, cy = cur->rect.Y + cur->rect.Height / 2;
  const Hit *best = nullptr;
  float best_score = 1e9f;
  for (const Hit &h : g.hits) {
    if (!Focusable(h) || (h.action == cur->action && h.index == cur->index)) {
      continue;
    }
    const float vx = h.rect.X + h.rect.Width / 2 - cx, vy = h.rect.Y + h.rect.Height / 2 - cy;
    const float primary = dx ? vx * dx : vy * dy;
    if (primary <= 4) {
      continue;
    }
    const float score = primary + 2.5f * std::fabs(dx ? vy : vx);
    if (score < best_score) {
      best_score = score;
      best = &h;
    }
  }
  if (best) {
    SetFocusTo(*best);
  } else if (dy) {
    int &scroll = g.picker_open ? g.picker_scroll : g.tab == kTabContent ? g.content_scroll : g.cvar_scroll;
    scroll = std::max(0, scroll + dy * 56);
  }
  Redraw();
}

void FocusNext(int dir) {
  std::vector<const Hit *> order;
  for (const Hit &h : g.hits) {
    if (Focusable(h) && std::none_of(order.begin(), order.end(), [&](const Hit *o) {
          return o->action == h.action && o->index == h.index;
        })) {
      order.push_back(&h);
    }
  }
  if (order.empty()) {
    return;
  }
  size_t at = 0;
  for (size_t i = 0; i < order.size(); ++i) {
    if (order[i]->action == g.focus_action && order[i]->index == g.focus_index) {
      at = (i + order.size() + dir) % order.size();
      break;
    }
  }
  g.focus_visible = true;
  SetFocusTo(*order[at]);
  Redraw();
}

void ActivateFocused() {
  if (const Hit *h = FocusedHit()) {
    const Hit hit = *h;
    Activate(hit.action, hit.index);
  } else {
    ResetFocus();
    Redraw();
  }
}

void PollController() {
  if (!g.xinput_get_state || GetForegroundWindow() != g.wnd) {
    return;
  }
  XINPUT_STATE state{};
  if (g.xinput_get_state(0, &state) != ERROR_SUCCESS) {
    return;
  }
  const WORD now = state.Gamepad.wButtons, pressed = WORD(now & ~g.pad_buttons);
  g.pad_buttons = now;
  // D-pad or left stick moves the focus, repeating while held.
  const SHORT lx = state.Gamepad.sThumbLX, ly = state.Gamepad.sThumbLY;
  int dir = 0;
  if (now & XINPUT_GAMEPAD_DPAD_UP || ly > 20000) {
    dir = 1;
  } else if (now & XINPUT_GAMEPAD_DPAD_DOWN || ly < -20000) {
    dir = 2;
  } else if (now & XINPUT_GAMEPAD_DPAD_LEFT || lx < -20000) {
    dir = 3;
  } else if (now & XINPUT_GAMEPAD_DPAD_RIGHT || lx > 20000) {
    dir = 4;
  }
  const DWORD tick = GetTickCount();
  if (dir && (dir != g.pad_dir || int(tick - g.pad_repeat_at) >= 0)) {
    g.pad_repeat_at = tick + (dir != g.pad_dir ? 380 : 110);
    MoveFocus(dir == 3 ? -1 : dir == 4 ? 1 : 0, dir == 1 ? -1 : dir == 2 ? 1 : 0);
  }
  g.pad_dir = dir;
  if (pressed & XINPUT_GAMEPAD_A) {
    ActivateFocused();
  }
  if (pressed & XINPUT_GAMEPAD_START) {
    Play();
  }
  if (pressed & XINPUT_GAMEPAD_B) {
    if (g.picker_open) {
      ClosePicker();
    } else {
      g.focus_action = kActPlay;
      g.focus_index = -1;
      g.focus_visible = true;
    }
    Redraw();
  }
  if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) {
    SetTab(g.tab - 1);
  }
  if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) {
    SetTab(g.tab + 1);
  }
}

void CreateFonts() {
  // Bahnschrift (Windows 10+) like the PGR4Recomp launcher; Segoe UI or the
  // default sans elsewhere (Proton).
  auto pick = [](std::initializer_list<const wchar_t *> names) {
    for (const wchar_t *n : names) {
      auto f = std::make_unique<FontFamily>(n);
      if (f->GetLastStatus() == Ok) {
        return f;
      }
    }
    return std::unique_ptr<FontFamily>(FontFamily::GenericSansSerif()->Clone());
  };
  g.family = pick({L"Bahnschrift", L"Segoe UI", L"Tahoma"});
  g.family_bold = pick({L"Bahnschrift SemiBold", L"Segoe UI Semibold", L"Segoe UI", L"Tahoma"});
  g.family_mono = pick({L"Cascadia Mono", L"Consolas", L"Courier New"});
  const bool real_semibold = g.family_bold->GetLastStatus() == Ok;
  const int bold_style = real_semibold ? FontStyleRegular : FontStyleBold;
  g.f_title = std::make_unique<Font>(g.family_bold.get(), 38.0f, bold_style, UnitPixel);
  g.f_sub = std::make_unique<Font>(g.family.get(), 14.0f, FontStyleRegular, UnitPixel);
  g.f_tab = std::make_unique<Font>(g.family_bold.get(), 17.0f, bold_style, UnitPixel);
  g.f_body = std::make_unique<Font>(g.family.get(), 15.0f, FontStyleRegular, UnitPixel);
  g.f_bold = std::make_unique<Font>(g.family_bold.get(), 15.0f, bold_style, UnitPixel);
  g.f_small = std::make_unique<Font>(g.family.get(), 13.0f, FontStyleRegular, UnitPixel);
  g.f_button = std::make_unique<Font>(g.family_bold.get(), 15.0f, bold_style, UnitPixel);
  g.f_play = std::make_unique<Font>(g.family_bold.get(), 22.0f, bold_style, UnitPixel);
  g.f_mono = std::make_unique<Font>(g.family_mono.get(), 12.5f, FontStyleRegular, UnitPixel);
  g.f_badge = std::make_unique<Font>(g.family_bold.get(), 11.5f, bold_style, UnitPixel);
  g.f_stat = std::make_unique<Font>(g.family_bold.get(), 24.0f, bold_style, UnitPixel);
  LOGFONTW lf{};
  lf.lfHeight = -LONG(15 * g.scale);
  lf.lfQuality = CLEARTYPE_QUALITY;
  wcscpy_s(lf.lfFaceName, L"Segoe UI");
  g.edit_font = CreateFontIndirectW(&lf);
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_CREATE: {
    g.wnd = wnd;
    g.field_brush = CreateSolidBrush(kFieldRef);
    g.game_dir_edit = MakeEdit(g.settings.game_dir, kIdGameDirEdit);
    g.search_edit = MakeEdit(L"", kIdSearchEdit);
    g.user_name_edit = MakeEdit(g.settings.user_name, kIdUserNameEdit);
    SendMessageW(g.user_name_edit, EM_SETLIMITTEXT, 15, 0); // gamertag length
    RebuildCvarEdits();
    RefreshPackages();
    SetTimer(wnd, 1, 9000, nullptr); // next background
    SetTimer(wnd, 2, 50, nullptr);   // controller
    LoadCvarsAsync();
    // Dark title bar (Windows 10 2004+; ignored elsewhere).
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
      using SetAttrFn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
      if (auto set_attr = reinterpret_cast<SetAttrFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"))) {
        const BOOL dark = TRUE;
        set_attr(wnd, 20, &dark, sizeof(dark));
      }
    }
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(wnd, &ps);
    Paint(dc, ps.rcPaint);
    EndPaint(wnd, &ps);
    return 0;
  }
  case WM_TIMER:
    if (wp == 1 && g.art.size() > 1 && !g.fading) {
      g.fading = true;
      g.fade_start = GetTickCount();
      SetTimer(wnd, 3, 33, nullptr);
    } else if (wp == 3) {
      if (GetTickCount() - g.fade_start >= 900) {
        g.fading = false;
        g.art_index = int((g.art_index + 1) % g.art.size());
        g.backdrop_fade = -1;
        KillTimer(wnd, 3);
      }
      Redraw();
    } else if (wp == 2) {
      PollController();
    } else if (wp == 4) {
      if (g.animating) {
        Redraw();
      } else {
        RedrawGlow();
      }
    }
    return 0;
  case WM_MOUSEMOVE: {
    static LPARAM last_pos = -1;
    if (lp != last_pos && last_pos != -1 && g.focus_visible) {
      g.focus_visible = false;
      Redraw();
    }
    last_pos = lp;
    if (!g.tracking) {
      TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, wnd, 0};
      TrackMouseEvent(&tme);
      g.tracking = true;
    }
    const Hit *h = HitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
    const Action a = h ? h->action : kActNone;
    const int index = h ? h->index : -1;
    if (a != g.hover_action || index != g.hover_index) {
      g.hover_action = a;
      g.hover_index = index;
      Redraw();
    }
    return 0;
  }
  case WM_MOUSELEAVE:
    g.tracking = false;
    g.hover_action = kActNone;
    g.hover_index = -1;
    Redraw();
    return 0;
  case WM_SETCURSOR:
    if (LOWORD(lp) == HTCLIENT) {
      SetCursor(LoadCursorW(nullptr, g.hover_action != kActNone && g.hover_action != kActClosePicker
                                         ? IDC_HAND
                                         : IDC_ARROW));
      return TRUE;
    }
    break;
  case WM_LBUTTONUP: {
    const Hit *h = HitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
    if (h) {
      const Hit hit = *h;
      Activate(hit.action, hit.index);
    } else {
      SetFocus(wnd);
      Redraw();
    }
    return 0;
  }
  case WM_MOUSEWHEEL: {
    const int delta = -GET_WHEEL_DELTA_WPARAM(wp) / 2;
    if (g.picker_open) {
      g.picker_scroll += delta;
    } else if (g.tab == kTabContent) {
      g.content_scroll += delta;
    } else if (g.tab == kTabAdvanced) {
      g.cvar_scroll += delta;
    }
    Redraw();
    return 0;
  }
  case WM_KEYDOWN:
    switch (wp) {
    case VK_ESCAPE:
      if (g.picker_open) {
        ClosePicker();
        Redraw();
      }
      break;
    case VK_RETURN:
    case VK_SPACE:
      g.focus_visible = true;
      ActivateFocused();
      break;
    case VK_TAB:
      FocusNext(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
      break;
    case VK_LEFT:
      MoveFocus(-1, 0);
      break;
    case VK_RIGHT:
      MoveFocus(1, 0);
      break;
    case VK_UP:
      MoveFocus(0, -1);
      break;
    case VK_DOWN:
      MoveFocus(0, 1);
      break;
    case VK_PRIOR:
    case VK_NEXT:
      SetTab(g.tab + (wp == VK_NEXT ? 1 : -1));
      break;
    }
    return 0;
  case WM_COMMAND: {
    const int id = LOWORD(wp);
    if (HIWORD(wp) == EN_CHANGE) {
      if (id == kIdGameDirEdit) {
        ApplyGameDir();
      } else if (id == kIdUserNameEdit) {
        g.settings.user_name = GetText(g.user_name_edit);
        SaveSettings();
      } else if (id == kIdSearchEdit) {
        UpdatePickerMatches();
        Redraw();
      } else if (id >= kIdCvarEditBase && size_t(id - kIdCvarEditBase) < g.overrides.size()) {
        g.overrides[size_t(id - kIdCvarEditBase)].value = GetText(HWND(lp));
        SaveSettings();
      }
    } else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
      Redraw(); // field border highlight
    }
    return 0;
  }
  case WM_CTLCOLOREDIT: {
    HDC dc = HDC(wp);
    SetTextColor(dc, kTextRef);
    SetBkColor(dc, kFieldRef);
    return LRESULT(g.field_brush);
  }
  case kMsgPickFirst:
    if (!g.picker_matches.empty()) {
      Activate(kActPickCvar, g.picker_matches.front());
    }
    return 0;
  case WM_ACTIVATEAPP:
    Redraw(); // starts or stops the glow timer
    if (wp) {
      // Back from the game: new log, maybe a title update was installed.
      g.last = ReadLastSession();
      RefreshPackages();
      Redraw();
    }
    return 0;
  case kMsgCvarsLoaded: {
    std::unique_ptr<std::vector<CvarDef>> defs(reinterpret_cast<std::vector<CvarDef> *>(lp));
    g.cvar_defs = std::move(*defs);
    g.cvars_loading = false;
    RebuildCvarEdits();
    UpdatePickerMatches();
    Redraw();
    return 0;
  }
  case WM_DESTROY:
    g.settings.game_dir = GetText(g.game_dir_edit);
    SaveSettings();
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  SetProcessDPIAware();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);
  GdiplusStartupInput gsi;
  GdiplusStartup(&g.gdiplus, &gsi, nullptr);

  HDC screen = GetDC(nullptr);
  g.scale = GetDeviceCaps(screen, LOGPIXELSY) / 96.0f;
  ReleaseDC(nullptr, screen);
  // Fit small screens (Steam Deck is 1280x800).
  const float fit = std::min((GetSystemMetrics(SM_CXSCREEN) - 40) / float(kWidth),
                             (GetSystemMetrics(SM_CYSCREEN) - 80) / float(kHeight));
  g.scale = std::max(0.6f, std::min(g.scale, fit));

  LoadSettings();
  if (!IsGameDir(fs::path(g.settings.game_dir))) {
    g.settings.game_dir = GuessGameDir();
  }
  CreateFonts();
  for (int id : {101, 102, 103}) {
    if (auto art = LoadArt(id)) {
      g.art.push_back(std::move(art));
    }
  }
  g.art_index = g.art.empty() ? 0 : int(GetTickCount() % g.art.size());
  g.gpus = DetectGpus();
  g.last = ReadLastSession();
  for (size_t i = 0; i < g.gpus.size(); ++i) {
    if (g.gpus[i].name == g.settings.gpu) {
      g.gpu_index = int(i);
    }
  }
  if (HMODULE xinput = LoadLibraryW(L"xinput1_4.dll")) {
    g.xinput_get_state = reinterpret_cast<App::XInputGetStateFn>(GetProcAddress(xinput, "XInputGetState"));
  }

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"PGR4Launcher";
  wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  wc.hIconSm = wc.hIcon;
  RegisterClassExW(&wc);

  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
  RECT rc{0, 0, LONG(kWidth * g.scale), LONG(kHeight * g.scale)};
  AdjustWindowRect(&rc, style, FALSE);
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"PGR4-Recomp", style, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                             std::max(0, (GetSystemMetrics(SM_CYSCREEN) - h) / 2), w, h, nullptr, nullptr, instance,
                             nullptr);
  ShowWindow(wnd, show);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_KEYDOWN && msg.hwnd != g.wnd &&
        (msg.wParam == VK_ESCAPE || msg.wParam == VK_RETURN || msg.wParam == VK_TAB)) {
      // Enter / Escape / Tab leave a text field; Enter in the cvar search
      // adds the first match.
      if (msg.wParam == VK_RETURN && msg.hwnd == g.search_edit && !g.picker_matches.empty()) {
        SendMessageW(g.wnd, kMsgPickFirst, 0, 0);
      } else if (msg.wParam == VK_ESCAPE && g.picker_open) {
        SendMessageW(g.wnd, WM_KEYDOWN, VK_ESCAPE, 0);
      } else {
        SetFocus(g.wnd);
        InvalidateRect(g.wnd, nullptr, FALSE);
      }
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  // GDI+ objects go before GdiplusShutdown.
  g.art.clear();
  g.backdrop.reset();
  g.f_title.reset(), g.f_sub.reset(), g.f_tab.reset(), g.f_body.reset(), g.f_bold.reset(), g.f_small.reset();
  g.f_button.reset(), g.f_play.reset(), g.f_mono.reset(), g.f_badge.reset(), g.f_stat.reset();
  g.family.reset(), g.family_bold.reset(), g.family_mono.reset();
  GdiplusShutdown(g.gdiplus);
  CoUninitialize();
  return 0;
}
