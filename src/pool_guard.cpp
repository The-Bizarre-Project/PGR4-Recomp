// pgr4_recompiled - guards on two of the game's resource-header pools
//
// Two testers' long sessions crashed in a job thread popping a free list (logs 020 and 089,
// 2026-11-03/04): sub_822B40F8 pops the pool at 0x82A80BC0 and sub_822B4AD0 the one at
// 0x82A80BD8, each `head = pool->head (+0xC); pool->head = *head` under the pool's critical
// section (+0x14). The heads read 0x00200003 and 0x7B0150C3: something had written over a freed
// block's link. What writes it is not known yet.
//
// Before each of those allocators runs, the head and its link are checked; a bad one is logged
// with what the block holds, and the list is emptied so the allocator takes fresh memory (the
// rest of the list leaks) instead of the game crashing.

#include <array>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

REX_EXTERN(__imp__sub_822B40F8);
REX_EXTERN(__imp__sub_822B4AD0);

namespace {
constexpr uint32_t kHead = 0xC;

uint32_t Load(uint32_t a) {
  return rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual(a));
}
void Store(uint32_t a, uint32_t v) {
  rex::memory::store_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual(a), v);
}

// A block address the game's heaps hand out: aligned, in the virtual heaps.
bool Plausible(uint32_t a) { return (a & 3) == 0 && a >= 0x40000000u && a < 0xA0000000u; }

void Check(uint32_t pool) {
  const uint32_t head = Load(pool + kHead);
  if (!head) {
    return;
  }
  bool bad = !Plausible(head);
  uint32_t next = 0;
  if (!bad) {
    next = Load(head);
    bad = next && !Plausible(next);
  }
  if (!bad) {
    return;
  }
  std::array<uint32_t, 4> words{};
  if (Plausible(head)) {
    for (uint32_t i = 0; i < words.size(); ++i) words[i] = Load(head + 4 * i);
  }
  REXLOG_WARN("pool guard: pool {:08X} free list corrupt: head {:08X} next {:08X} "
              "(block {:08X} {:08X} {:08X} {:08X}); list dropped",
              pool, head, next, words[0], words[1], words[2], words[3]);
  Store(pool + kHead, 0);
}
}  // namespace

REX_HOOK_RAW(sub_822B40F8) {
  Check(0x82A80BC0);
  __imp__sub_822B40F8(ctx, base);
}

REX_HOOK_RAW(sub_822B4AD0) {
  Check(0x82A80BD8);
  __imp__sub_822B4AD0(ctx, base);
}
