// pgr4_recompiled - the chase camera's height (Game Adjustments > Display)
//
// sub_827DC090(camera, ..., r6 = the car's chase parameters {distance, height, tilt, lag, ...})
// places the chase camera. Each parameter can be overridden by its tweakable, read when it is
// not 0: Camera.Chase.Dist (0x82C94220), Camera.Chase.Height (0x82C94254) and
// Camera.Chase.Tilt (0x82C94288), whose values are at node +32. --camera_height scales the
// car's own height: the override is set to it for the call and cleared after.

#include <algorithm>
#include <bit>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_DOUBLE(camera_height, 1.0, "PGR4",
                      "Chase camera height, as a multiple of the car's own (1 = the game's)");

REX_EXTERN(__imp__sub_827DC090);

namespace {
constexpr uint32_t kHeightOverride = 0x82C94254 + 32;  // Camera.Chase.Height's value
constexpr uint32_t kParamHeight = 4;

float LoadFloat(uint32_t a) {
  return std::bit_cast<float>(rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual(a)));
}
void StoreFloat(uint32_t a, float v) {
  rex::memory::store_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual(a), std::bit_cast<uint32_t>(v));
}
}  // namespace

REX_HOOK_RAW(sub_827DC090) {
  const double scale = REXCVAR_GET(camera_height);
  const uint32_t params = ctx.r6.u32;
  if (!params || scale == 1.0 || LoadFloat(kHeightOverride) != 0.0f) {
    __imp__sub_827DC090(ctx, base);
    return;
  }
  // 0 is "no override": the lowest choice stays just above the car's roofline.
  const float height = std::max(0.01f, float(LoadFloat(params + kParamHeight) * scale));
  StoreFloat(kHeightOverride, height);
  __imp__sub_827DC090(ctx, base);
  StoreFloat(kHeightOverride, 0.0f);
}
