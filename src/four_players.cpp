// pgr4_recompiled - race-mode table for 3 and 4 players
//
// The race-mode object at 0x82A423CC (set up by sub_825FCE58 when a race loads, from
// sub_824C8860) keeps, at +28, an inline array of 2 per-player entries of 24 bytes:
//   +0 state (1-4, sub_825F5048..78)   +4 ?   +8 player index
//   +12 array of the player's race-mode objects (140 bytes each, vtable by game mode):
//       data, capacity, size
// followed by its count (+48, 0x82A42418). The shipped game has 2-player split screen only; a
// 3- or 4-viewport race (the debug menu's race setup) wrote player 3's entry over the count and
// the float constants after it, and crashed freeing one of them (1/60) as the array's data.
//
// Every use of the entries indexes them by player from the table's address (sub_825F9AC0,
// sub_825FA0E0, sub_825FA958, sub_825F2250, sub_825F5048..78); only the setup, the reset
// (sub_825FDE48) and the teardown (sub_825FD9E8) read the count. So with more than 2 players:
// - the setup builds players 0-1 as the game does, then players 2-3 after them, over the count
//   and the 48 bytes of unused float constants that follow (saved first);
// - the count is kept here while the table is extended, and the reset and teardown use it;
// - shrinking back to 2 or fewer restores those bytes and the count.

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, split3_wide);
// 3-player wide layout: player 1's view (sub_824DB0F0's manager) and its full width in pixels.
static uint32_t g_view0 = 0, g_view0_full_width = 0;
REX_EXTERN(__imp__sub_825FCE58);
REX_EXTERN(__imp__sub_825FDE48);
REX_EXTERN(__imp__sub_825FD9E8);
REX_EXTERN(sub_8230C200);  // operator new
REX_EXTERN(sub_8230BCE0);  // delete (the objects in the arrays)
REX_EXTERN(sub_82712EC8);  // new[] (the arrays' data)
REX_EXTERN(sub_82302CF0);  // delete[] (the arrays' data)
REX_EXTERN(sub_825FDC88);  // the race-mode object's base constructor

namespace {
constexpr uint32_t kObject = 0x82A423CC;
constexpr uint32_t kTable = kObject + 28;  // the entries
constexpr uint32_t kCount = kTable + 48;   // the count; player 2's entry when extended
constexpr uint32_t kEntrySize = 24;
constexpr uint32_t kEntryData = 12, kEntryCapacity = 16, kEntrySize_ = 20, kEntryPlayer = 8;
constexpr uint32_t kObjectsPerPlayer = kObject + 20;
constexpr uint32_t kPlayers = 0x82B5EDD4;  // the race's players, the setup's loop
constexpr uint32_t kShippedPlayers = 2, kMaxPlayers = 4;
constexpr uint32_t kModeObjectSize = 140;
constexpr uint32_t kExtraBytes = (kMaxPlayers - kShippedPlayers) * kEntrySize;

bool g_extended = false;
uint32_t g_count = 0;
std::array<uint8_t, kExtraBytes> g_saved{};

uint8_t* At(uint32_t address) { return REX_KERNEL_MEMORY()->TranslateVirtual(address); }
uint32_t Load(uint32_t address) { return rex::memory::load_and_swap<uint32_t>(At(address)); }
void Store(uint32_t address, uint32_t value) { rex::memory::store_and_swap<uint32_t>(At(address), value); }
uint32_t Entry(uint32_t player) { return kTable + player * kEntrySize; }

using GuestFunc = void (*)(PPCContext&, uint8_t*);
uint32_t Call(GuestFunc f, PPCContext& ctx, uint8_t* base, uint32_t r3) {
  PPCContext c = ctx;
  c.r3.u64 = r3;
  f(c, base);
  return c.r3.u32;
}

// The reset (sub_825FDE48) of the extended table to `count` entries.
void Reset(PPCContext& ctx, uint8_t* base, uint32_t count) {
  for (uint32_t i = count; i < g_count; ++i) {
    const uint32_t entry = Entry(i);
    const uint32_t data = Load(entry + kEntryData);
    Store(entry + kEntrySize_, 0);
    Call(sub_82302CF0, ctx, base, data);
    Store(entry + kEntryData, 0);
    Store(entry + kEntryCapacity, 0);
    Store(entry + kEntrySize_, 0);
  }
  if (count <= kShippedPlayers) {
    std::memcpy(At(kCount), g_saved.data(), g_saved.size());
    Store(kCount, count);
    g_extended = false;
    g_count = 0;
  } else {
    g_count = count;
  }
}
}  // namespace

// sub_825FCE58(object): the race-mode object's setup when a race loads.
REX_HOOK_RAW(sub_825FCE58) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t players = Load(kPlayers);
  if (object != kObject || players <= kShippedPlayers) {
    __imp__sub_825FCE58(ctx, base);
    return;
  }
  const uint32_t extra = std::min(players, kMaxPlayers);
  if (players > kMaxPlayers) {
    REXLOG_WARN("4 players: the race has {} players; setting up {}", players, kMaxPlayers);
  }
  // Players 0-1 as the game does.
  PPCContext c = ctx;
  Store(kPlayers, kShippedPlayers);
  __imp__sub_825FCE58(c, base);
  Store(kPlayers, players);
  if (g_extended) {  // not reset since the last race (the setup resets first, so not expected)
    Reset(ctx, base, kShippedPlayers);
  }
  std::memcpy(g_saved.data(), At(kCount), g_saved.size());

  // Players 2-3: the same race-mode objects as player 0's (same vtables, base-constructed).
  const uint32_t per_player = Load(kObjectsPerPlayer);
  const uint32_t first = Entry(0);
  for (uint32_t p = kShippedPlayers; p < extra; ++p) {
    const uint32_t entry = Entry(p);
    uint32_t data = 0;
    if (per_player > 0) {
      data = Call(sub_82712EC8, ctx, base, per_player * 4);
      for (uint32_t i = 0; i < per_player; ++i) {
        const uint32_t model = Load(first + kEntrySize_) > i ? Load(Load(first + kEntryData) + i * 4) : 0;
        uint32_t mode_object = Call(sub_8230C200, ctx, base, kModeObjectSize);
        if (mode_object) {
          Call(sub_825FDC88, ctx, base, mode_object);
          if (model) Store(mode_object, Load(model));  // the game mode's vtable
        }
        if (data) Store(data + i * 4, mode_object);
      }
    }
    Store(entry + 0, 0);
    Store(entry + 4, 0);
    Store(entry + kEntryPlayer, p);
    Store(entry + kEntryData, data);
    Store(entry + kEntryCapacity, data ? per_player : 0);
    Store(entry + kEntrySize_, data ? per_player : 0);
  }
  g_extended = true;
  g_count = extra;
  REXLOG_INFO("4 players: race-mode table extended to {} players ({} objects each)", extra, per_player);
}

// sub_825FDE48(table, count): frees the entries from `count` on.
REX_HOOK_RAW(sub_825FDE48) {
  if (ctx.r3.u32 == kTable && g_extended) {
    Reset(ctx, base, ctx.r4.u32);
    return;
  }
  __imp__sub_825FDE48(ctx, base);
}

// sub_825FD9E8(object): the race-mode object's teardown when a race ends.
REX_HOOK_RAW(sub_825FD9E8) {
  if (ctx.r3.u32 == kObject && g_extended) {
    for (uint32_t p = 0; p < g_count; ++p) {
      const uint32_t entry = Entry(p);
      const uint32_t size = Load(entry + kEntrySize_), data = Load(entry + kEntryData);
      for (uint32_t i = 0; i < size; ++i) {
        const uint32_t mode_object = Load(data + i * 4);
        if (mode_object) Call(sub_8230BCE0, ctx, base, mode_object);
      }
      Store(entry + kEntrySize_, 0);
    }
    Reset(ctx, base, 0);  // the game's loop now sees no entries
  }
  __imp__sub_825FD9E8(ctx, base);
}

// The minimap (the object at 0x82BDFE80) keeps a render target per player at +24 (16 bytes each)
// and three matrices per player at +272/+400/+528 (64 bytes each), room for 2 players: with 4,
// the race setup (sub_82602E60) made render targets over its colours at +56, and the draw
// (sub_826004A0) wrote player 4's matrices over its vehicle and checkpoint arrays (+772, +784)
// and crashed reading them. Both now run for 2 players: viewports 3 and 4 have no minimap.
namespace {
class TwoPlayers {
 public:
  TwoPlayers() : players_(Load(kPlayers)) {
    if (players_ > kShippedPlayers) Store(kPlayers, kShippedPlayers);
  }
  ~TwoPlayers() {
    if (players_ > kShippedPlayers) Store(kPlayers, players_);
  }

 private:
  uint32_t players_;
};
}  // namespace

REX_EXTERN(__imp__sub_82602E60);
REX_EXTERN(__imp__sub_826004A0);

REX_HOOK_RAW(sub_82602E60) {
  TwoPlayers two;
  __imp__sub_82602E60(ctx, base);
}

REX_HOOK_RAW(sub_826004A0) {
  TwoPlayers two;
  __imp__sub_826004A0(ctx, base);
}

// sub_825FF9F8(minimap, player): the player's minimap matrices (+272/+400/+528 + 64 * player).
// Players 3 and 4's landed on the vehicle array pointer and counts (+772..+780).
REX_EXTERN(__imp__sub_825FF9F8);
REX_HOOK_RAW(sub_825FF9F8) {
  if (ctx.r3.u32 == 0x82BDFE80 && ctx.r4.u32 >= kShippedPlayers) return;
  __imp__sub_825FF9F8(ctx, base);
}

// sub_826A12A8: the light glow / sun occlusion queries for the viewport being drawn
// (*(0x82A8104C) + 32). Its per-viewport tables (0x82C8A2E0..0x82C8A4E0: 8 values or 128 bytes
// per viewport) hold 2 viewports; viewport 3 issued a "query" read from past them and crashed
// in D3DQuery_Issue. Viewports 3 and 4 skip it (no glows there).
REX_EXTERN(__imp__sub_826A12A8);
REX_HOOK_RAW(sub_826A12A8) {
  const uint32_t view_state = Load(0x82A8104C);
  if (view_state && Load(view_state + 32) >= kShippedPlayers) return;
  __imp__sub_826A12A8(ctx, base);
}

// The speed/rev bar: each viewport's TACHOMETER widget (the HUD layout makes one per viewport)
// draws its car's dials, which the car's HUD setup builds with sub_8245E7E0(car + 0x295C, ..., r5
// split screen, r6 build, r7 layout). The race setup's call (sub_82416640, returning to
// 0x82416ADC, player index still in r11) asks for them only when the player's slot at
// 0x82B5E3E0 + 320 * player is 0, which it is not for players 3 and 4: their cars had no dials,
// so viewports 3 and 4 had no speed bar. They are built for them too, and as the placement table
// (sub_8245C2A8/sub_8245C310, 0x82AAD3EC) has 2 columns only, players 3/4 take player 1/2's
// column moved to the bottom row.
REX_EXTERN(__imp__sub_8245E7E0);
REX_HOOK_RAW(sub_8245E7E0) {
  constexpr uint32_t kRaceSetupReturn = 0x82416ADC;
  const uint32_t hud = ctx.r3.u32, player = ctx.r11.u32;
  const bool from_setup = uint32_t(ctx.lr) == kRaceSetupReturn;
  const uint32_t players = Load(kPlayers);
  // Players 3/4 (and player 2 in the 3-player layout, bottom-left) are on the bottom row.
  const bool three = players == 3 && REXCVAR_GET(split3_wide);
  const bool extra = from_setup && player < kMaxPlayers && player < players &&
                     (player >= kShippedPlayers || (three && player == 1));
  if (extra) {
    ctx.r6.u64 = 1;
    // The column: players 3/4 take player 1/2's; with 3, players 2/3 are left/right.
    ctx.r7.u64 = three ? player - 1 : player - kShippedPlayers;
  }
  __imp__sub_8245E7E0(ctx, base);
  if (extra) {
    // Players 3 and 4's bars go in the bottom row: the placement (+68/+72 and +84/+88: x, y in
    // the HUD's 1280x720 space; then the size) is the top row's, half a screen up.
    constexpr float kHalfScreen = 360.0f;
    for (const uint32_t y : {hud + 72, hud + 88}) {
      Store(y, std::bit_cast<uint32_t>(std::bit_cast<float>(Load(y)) + kHalfScreen));
    }
  }
}

// GothamSystem.SetRaceCam (sub_825CD060), when the race starts: each viewport's camera goes from
// the intro to the player's chosen race camera, sub_824BC168(viewport, the profile's camera
// choice, +0x15C of the 141984-byte profile at 0x82BFE2A8 + 8). It sets viewport 1 only when
// exactly 2 profiles are active (Profile.NumActive), so with 3 or 4 split-screen players
// viewports 1-3 kept the intro camera for the whole race. They are set here: viewport 1 from the
// co-op profile (0x82C88E54), viewports 2-3 from the other active profiles in the order the
// lobby added them (ui/scripts/MultiPlayerMenu.lua, Pgr4.ExtraLocalProfiles).
REX_EXTERN(__imp__sub_825CD060);
REX_EXTERN(sub_824BC168);
REX_HOOK_RAW(sub_825CD060) {
  __imp__sub_825CD060(ctx, base);
  constexpr uint32_t kProfiles = 0x82BFE2A8 + 8, kProfileStride = 141984;
  constexpr uint32_t kProfileActive = 0x54 - 8, kProfileRaceCam = 0x15C;  // 0x82BFE2FC, 0x82BFE40C
  constexpr uint32_t kPrimary = 0x82C88E50, kCoop = 0x82C88E54;
  const uint32_t players = Load(kPlayers);
  if (players < 3) return;
  const uint32_t primary = Load(kPrimary), coop = Load(kCoop);
  std::array<uint32_t, kMaxPlayers> order{};
  uint32_t n = 0;
  order[n++] = primary;
  if (coop < 4) order[n++] = coop;
  for (uint32_t i = 0; i < 4 && n < kMaxPlayers; ++i) {
    if (i == primary || i == coop) continue;
    if (Load(kProfiles + kProfileStride * i + kProfileActive)) order[n++] = i;
  }
  for (uint32_t viewport = 1; viewport < std::min(players, n); ++viewport) {
    PPCContext c = ctx;
    c.r3.u64 = viewport;
    c.r4.u64 = Load(kProfiles + kProfileStride * order[viewport] + kProfileRaceCam);
    sub_824BC168(c, base);
  }
}

// The 3-player layout: the game splits the screen into quadrants for 3 viewports and leaves the
// bottom-right one black. sub_824DB0F0(manager) builds the viewports from descriptors at
// manager + 0x3354C (64 bytes each: x, y, width, height as fractions of the screen; the count
// at +0x100). For 3 player quadrants they become player 1 across the top half and players 2 and
// 3 side by side below (the HUD's rectangles to match: ui/scripts/GUIBank.lua).
REXCVAR_DEFINE_BOOL(split3_wide, true, "PGR4",
                    "3-player split screen: player 1 across the top half, players 2 and 3 below; off: "
                    "quadrants with the bottom-right one empty");
REX_EXTERN(__imp__sub_824DB0F0);
REX_HOOK_RAW(sub_824DB0F0) {
  constexpr uint32_t kDescriptors = 0x3354C, kDescriptorSize = 64, kCount = 0x100;
  const uint32_t desc = ctx.r3.u32 + kDescriptors;
  auto f = [&](uint32_t i, uint32_t field) { return desc + kDescriptorSize * i + 4 * field; };
  auto get = [&](uint32_t a) { return std::bit_cast<float>(Load(a)); };
  auto set = [&](uint32_t a, float v) { Store(a, std::bit_cast<uint32_t>(v)); };
  if (REXCVAR_GET(split3_wide) && ctx.r3.u32 && Load(desc + kCount) == 3 && get(f(0, 2)) == 0.5f && get(f(0, 3)) == 0.5f &&
      get(f(2, 1)) == 0.5f) {
    const float layout[3][4] = {{0.0f, 0.0f, 1.0f, 0.5f}, {0.0f, 0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f, 0.5f}};
    for (uint32_t i = 0; i < 3; ++i) {
      for (uint32_t k = 0; k < 4; ++k) set(f(i, k), layout[i][k]);
    }
    // +16: the view's intended width / height (the projection's aspect over the rectangle's):
    // 32/9 for the wide view, as for the game's own 2-player halves; the quadrant's 16/9 squeezed
    // a 16:9 picture across it.
    set(f(0, 4), get(f(0, 4)) * 2.0f);
  }
  const uint32_t manager = ctx.r3.u32;
  __imp__sub_824DB0F0(ctx, base);
  g_view0 = g_view0_full_width = 0;
  if (manager && REXCVAR_GET(split3_wide) && Load(desc + kCount) == 3 && Load(manager + 8) > Load(manager + 21008 + 8)) {
    g_view0 = manager;
    g_view0_full_width = Load(manager + 8);
  }
}

// sub_82869DB0(system 0x82C98E48, context, copy) makes a view's culling context current and sets its
// viewport: the context's own (copied to system+168) when asked, otherwise the one cached there, which
// the last full set left: player 1's. The game's layouts give every view one size, so the cached
// viewport fitted them all; with player 1 across the top it was twice too wide for players 2 and 3,
// whose cars, barriers and other objects drawn through this were stretched off their place (cars
// without bodies, barriers in the air). For another view the cached viewport takes that view's size.
REX_EXTERN(__imp__sub_82869DB0);
REX_HOOK_RAW(sub_82869DB0) {
  const uint32_t system = ctx.r3.u32, cache = system + 168;
  const uint32_t current = Load(0x82A8104C);
  const bool swap = system && !(ctx.r5.u32 & 0xFF) && g_view0_full_width && current && current != g_view0 &&
                    Load(cache + 8) == g_view0_full_width;
  uint32_t saved_width = 0, saved_height = 0;
  if (swap) {
    saved_width = Load(cache + 8);
    saved_height = Load(cache + 12);
    Store(cache + 8, Load(current + 8));
    Store(cache + 12, Load(current + 12));
  }
  __imp__sub_82869DB0(ctx, base);
  if (swap) {
    Store(cache + 8, saved_width);
    Store(cache + 12, saved_height);
  }
}

// sub_82317950(width, height, multisample) sets up the EDRAM scene surface for a view. The game
// asks 4x for its 3-4 player quadrants, which fit in EDRAM (10 MiB, 4 bytes color and 4 depth a
// sample); player 1 across the top at 4x does not, and the tiling table (sub_82317618) has no entry
// for it, so it drew into a quarter-size surface. Its 2-player halves are 2x: the wide view gets
// that too.
REX_EXTERN(__imp__sub_82317950);
REX_HOOK_RAW(sub_82317950) {
  constexpr uint64_t kEdram = 10u << 20;
  if (REXCVAR_GET(split3_wide) && Load(kPlayers) == 3 && ctx.r4.u32 <= 360) {
    while (ctx.r5.u32 > 0 && ctx.r5.u32 < 3 &&
           uint64_t(ctx.r3.u32) * ctx.r4.u32 * (1u << ctx.r5.u32) * 8 > kEdram) {
      --ctx.r5.u64;
    }
  }
  __imp__sub_82317950(ctx, base);
}

// sub_82324DD8(pool 0x82A94620, viewport, mirror view, count) gives every view a color texture (pool
// +0x24 + 4i, aliased at +0x38 + 4i in another format) and a depth texture (+0x80 + 4i), all the size
// of the viewport passed (the game's last: its layouts have one view size), carved one after another
// out of a region sized for one 1280x720 color plus depth. With player 1 across the top it is
// twice as wide as that size: its view rendered into a quarter-screen texture (black / stretched,
// and running into the textures after it). Player 1 gets a set of its own at its size; the others
// keep the game's. The pool's lists of the same textures (+0x50 + 12k and +0x98 + 12k, k from 1)
// follow.
REX_EXTERN(__imp__sub_82324DD8);
REX_EXTERN(sub_822B3238);  // texture size in bytes (width, height, levels, format)
REX_EXTERN(sub_822B3FC8);  // texture at an address (width, height, levels, usage, format, address, size)
// The post-processing manager's per-view entries (sub_8283FFA8(manager, width, height, view, ...),
// added by sub_82324DD8 with the same viewport's size) size the surface each view's HUD pass draws
// into; player 1's gets its own size too (set while sub_82324DD8 runs).
thread_local uint32_t g_view0_width = 0, g_view0_height = 0;
REX_EXTERN(__imp__sub_8283FFA8);
REX_HOOK_RAW(sub_8283FFA8) {
  if (g_view0_width && ctx.r6.u32 == 0) {
    ctx.r4.u64 = g_view0_width;
    ctx.r5.u64 = g_view0_height;
  }
  __imp__sub_8283FFA8(ctx, base);
}

REX_HOOK_RAW(sub_82324DD8) {
  const uint32_t pool = ctx.r3.u32, last = ctx.r4.u32;
  const int32_t count = int32_t(ctx.r6.u32);
  constexpr uint32_t kViewStride = 21008, kColorFormat = 0x18, kAliasFormat = 438479709,
                     kDepthFormat = 438436247;
  uint32_t w = 0, h = 0;
  if (REXCVAR_GET(split3_wide) && pool && last && count == 3) {
    const uint32_t first = last - kViewStride * uint32_t(count - 1);
    w = Load(first + 8);
    h = Load(first + 12);
    if (w <= Load(last + 8) || h != Load(last + 12)) w = h = 0;
  }
  PPCContext saved = ctx;
  g_view0_width = w;
  g_view0_height = h;
  __imp__sub_82324DD8(ctx, base);
  g_view0_width = g_view0_height = 0;
  if (!w) return;
  auto call = [&](auto fn, std::initializer_list<uint32_t> args) {
    PPCContext c = saved;
    uint64_t *regs[] = {&c.r3.u64, &c.r4.u64, &c.r5.u64, &c.r6.u64, &c.r7.u64, &c.r8.u64, &c.r9.u64};
    size_t i = 0;
    for (uint32_t a : args) *regs[i++] = a;
    fn(c, base);
    return c.r3.u32;
  };
  const uint32_t color_format = Load(pool + kColorFormat);
  const uint32_t color_size = call(sub_822B3238, {w, h, 1, color_format});
  const uint32_t depth_size = call(sub_822B3238, {w, h, 1, kDepthFormat});
  static uint32_t region = 0, region_size = 0;
  const uint32_t need = (color_size + depth_size + 0xFFFF) & ~0xFFFFu;
  if (need > region_size) {
    auto *heap = REX_KERNEL_MEMORY()->LookupHeapByType(true, 0x10000);
    uint32_t address = 0;
    if (!heap || !heap->Alloc(need, 0x10000,
                              rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                              rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, false, &address)) {
      REXLOG_WARN("split screen: no memory for player 1's {}x{} view textures", w, h);
      return;
    }
    region = address;
    region_size = need;
  }
  const uint32_t color = call(sub_822B3FC8, {w, h, 1, 1, color_format, region, color_size});
  const uint32_t alias = call(sub_822B3FC8, {w, h, 1, 1, kAliasFormat, region, color_size});
  const uint32_t depth = call(sub_822B3FC8, {w, h, 1, 1, kDepthFormat, region + color_size, depth_size});
  if (!color || !alias || !depth) return;
  Store(pool + 4 * 9, color);
  Store(pool + 4 * 14, alias);
  Store(pool + 4 * 32, depth);
  Store(pool + 4 * (3 * 1 + 17), color);
  Store(pool + 4 * (3 * 1 + 35), depth);
  REXLOG_INFO("split screen: player 1's view textures {}x{} at {:08X}", w, h, region);
}
