// pgr4_recompiled - game frame step
//
// sub_822C2CD8 updates PGR4's frame timer (the object at 0x82A8131C, once per
// frame from the main loop). Its step at +8 is 1 / (refresh rate / present
// interval) with Dynamics.UsePresentIntervalForTimer, else 1/30: on the 360,
// 60 Hz over interval 2, so every frame counts 1/30 s. Above 30 fps that
// step still counts 1/30 per frame and the race clock (and whatever else
// steps by it) ran fast: twice as fast at 60. The step is now the real time
// since the last update, as the timer's own real-time delta (+16) is.
//
// Timer object: +4 time scale, +8 step, +12 previous step, +16 scaled real
// delta (at most 0.1), +24 accumulated time.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>

#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

REX_EXTERN(__imp__sub_822C2CD8);

namespace {
constexpr uint32_t kStep = 8;
constexpr uint32_t kPreviousStep = 12;
// The timer's own bounds for its real-time delta.
constexpr double kMinStep = 1.0 / 1000.0, kMaxStep = 0.1;

// Debug: a `frametimer.off` file next to the exe restores the fixed step
// (checked once a second).
bool Disabled() {
  static std::atomic<int64_t> next{0};
  static std::atomic<bool> off{false};
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  if (now >= next.load(std::memory_order_relaxed)) {
    next = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(1)).count();
    off = std::filesystem::exists("frametimer.off");
  }
  return off;
}
} // namespace

// sub_822C2CD8(timer, keep_previous)
REX_HOOK_RAW(sub_822C2CD8) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last;
  const uint32_t timer = ctx.r3.u32;
  const bool keep_previous = ctx.r4.u8 != 0;
  __imp__sub_822C2CD8(ctx, base);

  const auto now = Clock::now();
  const bool first = last == Clock::time_point{};
  const double dt = first ? 1.0 / 30.0 : std::chrono::duration<double>(now - last).count();
  last = now;
  if (!timer || Disabled()) {
    return;
  }
  auto *memory = REX_KERNEL_MEMORY();
  const float step = float(std::clamp(dt, kMinStep, kMaxStep));
  rex::memory::store_and_swap<float>(memory->TranslateVirtual(timer + kStep), step);
  // Without keep_previous the original copied the step it had just set.
  if (!keep_previous) {
    rex::memory::store_and_swap<float>(memory->TranslateVirtual(timer + kPreviousStep), step);
  }
}
