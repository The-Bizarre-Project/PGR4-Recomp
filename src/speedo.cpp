// pgr4_recompiled - the chase view's speedometer in the in-car view
//
// The speed and rev bar the chase and bumper views show ("0 MPH ..... R") is the car's dial HUD
// (car + 0x295C), drawn each frame per viewport by sub_8245CBD0(hud, data, viewport) from the
// viewport loop in sub_824DC5E8. It asks the viewport's camera for its view (vtable +88):
//   256 / 258 (chase, bumper): +36 = 1, the chase dial set
//   257: +36 = the split-screen flag (+49)
//   other views, the cockpit among them: +36 = -1, and nothing is drawn.
// Then it copies the car's speed, revs and gear from data (+292 gear, +320, +324 mph, +340) and
// draws each element whose set (+24) is +36, between sub_822B3488 and nullsub_99 on the HUD's
// render state (+124).
// With --speedo (Game Adjustments > Display) the cockpit view draws the chase set the same way.

#include "speedo.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(speedo, false, "PGR4",
                    "Show the chase view's speedometer (speed and rev bar) in the in-car view too");

REX_EXTERN(__imp__sub_8245CBD0);
REX_EXTERN(sub_822B3488);  // the dial HUD's render state, begin

namespace {
constexpr uint32_t kHudView = 36, kHudGear = 28, kHudSpeed = 4;
constexpr uint32_t kHudBuilt = 50, kHudBuild = 51, kHudState = 124, kHudSkin = 40;
constexpr uint32_t kHudElements = 140, kHudElementCount = 148;
constexpr uint32_t kElementSet = 24;
constexpr uint32_t kChaseSet = 1;
constexpr uint32_t kNoDials = 0xFFFFFFFF;

uint8_t* At(uint32_t a) { return REX_KERNEL_MEMORY()->TranslateVirtual(a); }
uint32_t Load(uint32_t a) { return rex::memory::load_and_swap<uint32_t>(At(a)); }
uint8_t Load8(uint32_t a) { return *At(a); }
void Store(uint32_t a, uint32_t v) { rex::memory::store_and_swap<uint32_t>(At(a), v); }

void CallGuest(PPCFunc* f, PPCContext& ctx, uint8_t* base, uint32_t r3, uint32_t r4) {
  PPCContext c = ctx;
  c.r3.u64 = r3;
  c.r4.u64 = r4;
  f(c, base);
}
}  // namespace

namespace pgr4::speedo {
void Install(rex::ui::ImGuiDrawer*, std::function<void(std::function<void()>)>) {}
}  // namespace pgr4::speedo

REX_HOOK_RAW(sub_8245CBD0) {
  const uint32_t hud = ctx.r3.u32, data = ctx.r4.u32;
  __imp__sub_8245CBD0(ctx, base);
  if (!REXCVAR_GET(speedo) || !hud || !data || Load(hud + kHudView) != kNoDials) {
    return;
  }
  // The checks the draw makes before it asks the camera.
  const uint32_t skin = Load(hud + kHudSkin);
  if (!Load8(hud + kHudBuilt) || !Load(hud + kHudState) || !skin || !Load(skin) ||
      !Load8(hud + kHudBuild)) {
    return;
  }
  Store(hud + kHudView, kChaseSet);
  Store(hud + kHudGear, Load(data + 292));
  Store(hud + 0, Load(data + 320));
  Store(hud + 12, Load(data + 340));
  Store(hud + kHudSpeed, Load(data + 324));
  {
    PPCContext c = ctx;
    c.r3.u64 = Load(hud + kHudState);
    sub_822B3488(c, base);
  }
  auto* dispatcher = REX_KERNEL_STATE()->function_dispatcher();
  const uint32_t count = Load(hud + kHudElementCount), elements = Load(hud + kHudElements);
  for (uint32_t i = 0; elements && i < count; ++i) {
    const uint32_t element = Load(elements + 4 * i);
    if (!element || Load(element + kElementSet) != kChaseSet) continue;
    const uint32_t vtable = Load(element);
    PPCFunc* draw = vtable && dispatcher ? dispatcher->GetFunction(Load(vtable + 4)) : nullptr;
    if (draw) CallGuest(draw, ctx, base, element, hud);
  }
  // The draw ends with nullsub_99 on the render state: nothing to do.
}
