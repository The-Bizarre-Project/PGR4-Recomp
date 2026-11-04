// pgr4_recompiled - audio lock trace (diagnostic)
//
// A tester's game froze moving to the next race of a tournament (log 035):
// the main thread waited forever in sub_827C7638, which takes the audio
// object's event at +13368 (sub_828A0248 waits for it, sub_828A02A0 sets it
// again), while every other guest thread sat idle. Something took that event
// and never set it.
//
// This records, for every event taken through sub_828A0248 and not yet set
// through sub_828A02A0, which host thread took it and from where (guest LR).
// A watchdog logs the holder once the main thread has waited on the audio
// object for 3 s.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <windows.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xobject.h>

REX_EXTERN(__imp__sub_827C7638);
REX_EXTERN(__imp__sub_828A0248);
REX_EXTERN(__imp__sub_828A02A0);

namespace {

struct Holder {
  uint32_t thread = 0;
  uint32_t lr = 0;
  std::chrono::steady_clock::time_point since;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Holder> g_held;
uint32_t g_audio_event = 0;
uint32_t g_waiter = 0, g_waiter_lr = 0;
std::chrono::steady_clock::time_point g_wait_start;
bool g_waiting = false;
// The lock is { u8 valid; u32 mutant handle } (sub_828A01D8 creates it once).
// Logs 048 and 075 froze right after XSessionStart with no holder recorded,
// so the handle may have been closed and reused for another object.
uint32_t g_first_handle = 0;

uint32_t LockHandle(uint32_t lock) {
  return rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual(lock + 4));
}

const char *ObjectTypeName(uint32_t handle) {
  static const char *const kNames[] = {"Undefined", "Enumerator",     "Event",     "File",   "IOCompletion",
                                       "Module",    "Mutant",         "NotifyListener", "Semaphore",
                                       "Session",   "Socket",         "SymbolicLink",   "Thread", "Timer"};
  auto object = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XObject>(handle);
  if (!object) {
    return "no object";
  }
  const auto type = uint32_t(object->type());
  return type < std::size(kNames) ? kNames[type] : "?";
}

void Watchdog() {
  bool reported = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::lock_guard lock(g_mutex);
    if (!g_waiting) {
      reported = false;
      continue;
    }
    if (reported || std::chrono::steady_clock::now() - g_wait_start < std::chrono::seconds(3)) {
      continue;
    }
    reported = true;
    const uint32_t handle = LockHandle(g_audio_event);
    REXLOG_WARN("audio lock trace: lock {:08X} handle {:08X} ({}), first seen {:08X}", g_audio_event, handle,
                ObjectTypeName(handle), g_first_handle);
    auto it = g_held.find(g_audio_event);
    if (it == g_held.end()) {
      REXLOG_WARN("audio lock trace: thread {} (lr {:08X}) stuck on {:08X}, no recorded holder", g_waiter,
                  g_waiter_lr, g_audio_event);
    } else {
      REXLOG_WARN("audio lock trace: thread {} (lr {:08X}) stuck on {:08X}, held by thread {} since {} ms ago, "
                  "taken at lr {:08X}",
                  g_waiter, g_waiter_lr, g_audio_event, it->second.thread,
                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                        it->second.since)
                      .count(),
                  it->second.lr);
    }
  }
}

// Debug: a `locktrace.off` file next to the exe skips the tracing
// (checked once a second).
// The game closes some of these locks (sub_828A0230 closes the handle but
// leaves the lock marked valid) and keeps using them. On the console a wait on
// the closed handle fails at once; here the handle number is soon reused for
// another object, and log 091 hung for good waiting on an Event that took it.
// A lock whose handle is no longer a mutant is dropped (marked invalid), so
// taking and releasing it succeed without waiting, as before it was created.
bool DropDeadLock(uint32_t lock) {
  auto *p = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t *>(lock);
  if (!p[0]) {
    return false;
  }
  const uint32_t handle = LockHandle(lock);
  auto object = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XObject>(handle);
  if (object && object->type() == rex::system::XObject::Type::Mutant) {
    return false;
  }
  p[0] = 0;
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 20) {
    REXLOG_WARN("audio lock: lock {:08X} handle {:08X} is {} (closed), dropped", lock, handle,
                ObjectTypeName(handle));
  }
  return true;
}

bool TraceOff() {
  static std::atomic<int64_t> next{0};
  static std::atomic<bool> off{false};
  const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
  if (now >= next.load(std::memory_order_relaxed)) {
    next = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::seconds(1)).count();
    off = std::filesystem::exists("locktrace.off");
  }
  return off;
}

} // namespace

REX_HOOK_RAW(sub_827C7638) {
  if (TraceOff()) {
    __imp__sub_827C7638(ctx, base);
    return;
  }
  static std::once_flag watchdog;
  std::call_once(watchdog, [] { std::thread(Watchdog).detach(); });
  const uint32_t event = ctx.r3.u32 + 13368;
  {
    std::lock_guard lock(g_mutex);
    const uint32_t handle = LockHandle(event);
    if (!g_first_handle) {
      g_first_handle = handle;
    } else if (handle != g_first_handle) {
      REXLOG_WARN("audio lock trace: lock {:08X} handle changed {:08X} -> {:08X} ({})", event, g_first_handle,
                  handle, ObjectTypeName(handle));
      g_first_handle = handle;
    }
    g_audio_event = event;
    g_waiter = GetCurrentThreadId();
    g_waiter_lr = uint32_t(ctx.lr);
    g_wait_start = std::chrono::steady_clock::now();
    g_waiting = true;
  }
  __imp__sub_827C7638(ctx, base);
  std::lock_guard lock(g_mutex);
  g_waiting = false;
}

REX_HOOK_RAW(sub_828A0248) {
  if (DropDeadLock(ctx.r3.u32)) {
    ctx.r3.u64 = 1;
    return;
  }
  if (TraceOff()) {
    __imp__sub_828A0248(ctx, base);
    return;
  }
  const uint32_t event = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_828A0248(ctx, base);
  if (ctx.r3.u32 & 0xFF) {
    std::lock_guard lock(g_mutex);
    g_held[event] = {uint32_t(GetCurrentThreadId()), lr, std::chrono::steady_clock::now()};
  }
}

REX_HOOK_RAW(sub_828A02A0) {
  if (DropDeadLock(ctx.r3.u32)) {
    ctx.r3.u64 = 1;
    return;
  }
  if (TraceOff()) {
    __imp__sub_828A02A0(ctx, base);
    return;
  }
  const uint32_t event = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  {
    std::lock_guard lock(g_mutex);
    g_held.erase(event);
  }
  __imp__sub_828A02A0(ctx, base);
  // A release that fails (not the owner, or a handle that is no mutant any
  // more) leaves the lock held with no holder recorded.
  if (!(ctx.r3.u32 & 0xFF)) {
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 20) {
      const uint32_t handle = LockHandle(event);
      REXLOG_WARN("audio lock trace: release of {:08X} FAILED on thread {} (lr {:08X}), handle {:08X} ({})", event,
                  uint32_t(GetCurrentThreadId()), lr, handle, ObjectTypeName(handle));
    }
  }
}
