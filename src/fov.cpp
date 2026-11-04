// pgr4_recompiled - field of view
//
// sub_826E32F0 builds a view's projection from its vertical field of view (f1) and aspect (f2).
// The game's cameras (chase, bumper, cockpit, replays, the mirror) reach it from a view's
// perspective, sub_822C2410, with the field of view of the camera object (the call at
// 0x822C24C0) or the view's own (0x822C2540); its other callers are the shadow maps, reflections
// and UI models, left alone. --fov_scale widens or narrows those cameras: tan(fov / 2) scaled,
// so every camera keeps its proportion to the others. Square views (cube faces) are skipped.
// Set live from the DISPLAY page (src/ui/pgr4_settings.cpp).
//
// The call sites are pgr4-native's (games/pgr4/src/pgr4_hooks.cpp, Pgr4ViewProjectionHook).

#include <algorithm>
#include <cmath>

#include <atomic>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_DOUBLE(fov_scale, 1.0, "PGR4",
                      "Field of view of the game's cameras: 1.0 is the game's, 1.2 wider by a fifth "
                      "(tan of the half angle scaled)");

REX_EXTERN(__imp__sub_826E32F0);

namespace {
// Return addresses of sub_822C2410's two calls.
constexpr uint32_t kCameraFov = 0x822C24C4, kViewFov = 0x822C2544;
// Wider than this is a screen's view, not a cube face or shadow map.
constexpr double kMinAspect = 1.2;
constexpr double kMaxFov = 2.9; // about 166 degrees vertical
} // namespace

// sub_826E32F0(matrix, f1 = vertical field of view, f2 = aspect, ...)
REXCVAR_DEFINE_BOOL(fov_log, false, "PGR4", "Debug: log the game cameras' field of view and aspect");

REX_HOOK_RAW(sub_826E32F0) {
  const uint32_t lr = uint32_t(ctx.lr);
  if (REXCVAR_GET(fov_log) && (lr == kCameraFov || lr == kViewFov)) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 400) {
      REXLOG_INFO("fov log: lr {:08X} fov {:.4f} aspect {:.4f} near {:.3f} far {:.1f}", lr, ctx.f1.f64,
                  ctx.f2.f64, ctx.f3.f64, ctx.f4.f64);
    }
  }
  const double scale = REXCVAR_GET(fov_scale);
  if ((lr == kCameraFov || lr == kViewFov) && scale > 0.0 && std::abs(scale - 1.0) > 1e-6 &&
      ctx.f2.f64 > kMinAspect && ctx.f1.f64 > 0.0) {
    const double fov = 2.0 * std::atan(std::tan(ctx.f1.f64 * 0.5) * scale);
    ctx.f1.f64 = double(float(std::min(fov, kMaxFov)));
  }
  __imp__sub_826E32F0(ctx, base);
}
