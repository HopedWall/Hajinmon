// doa4 - fixes to the game code through mid-asm hooks
// (declared in doa4_manifest.toml, [[entrypoint.midasm_hook]] section).

#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

#include <rex/logging/macros.h>
#include <rex/system/kernel_state.h>
#include <rex/ppc.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

// Host address of guest address 0: the SDK maps guest memory at the first free
// power-of-two base from 4 GB up, which is not always 4 GB.
uintptr_t GuestBase() {
  return reinterpret_cast<uintptr_t>(REX_KERNEL_MEMORY()->virtual_membase());
}

uint32_t GuestU32(uint32_t address) {
  uint32_t value;
  std::memcpy(&value, reinterpret_cast<const void*>(GuestBase() + address), sizeof(value));
  return std::byteswap(value);
}

uint8_t GuestU8(uint32_t address) {
  return *reinterpret_cast<const uint8_t*>(GuestBase() + address);
}

// Sleeps for a short time (a fraction of a millisecond) instead of spinning. Sleep()
// on Windows has a 1 ms granularity, so a high-resolution waitable timer is used there.
void IdleWait(std::chrono::microseconds duration) {
#if defined(_WIN32)
  thread_local HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(duration.count()) * 10;  // Relative, 100 ns units.
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      return;
    }
  }
  ::Sleep(1);
#else
  std::this_thread::sleep_for(duration);
#endif
}

// Whether the guest page containing address is allocated and readable.
bool GuestReadable(uint32_t address) {
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(reinterpret_cast<const void*>(GuestBase() + address), &info, sizeof(info))) {
    return false;
  }
  constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
  return info.State == MEM_COMMIT && (info.Protect & kReadable) && !(info.Protect & PAGE_GUARD);
#else
  return true;
#endif
}

}  // namespace

// Safety net. When the runtime allocated the game's memory at low addresses
// (0x00010000...), Hayabusa's animation script took a wrong path: every frame,
// opcode 52 of the script interpreter sub_823FB7A0 passed index 0x3FFF to
// sub_824073F8, outside the table of 19 lists at 0x82C44188, and the game read
// an invalid "pointer" (0x3FC00000) and crashed. The real cause was the memory
// layout (fixed with --alloc_top_down, like on the Xbox 360): with that option
// this case no longer happens.
//
// Should it come back: hook at 0x8240742C, after the destination entry has been
// cleared. If the memory pointed to is readable the original code runs,
// otherwise we return as for an empty list (first byte 0xFF).
bool doa4_list_index_out_of_range(PPCRegister& r3, PPCRegister& r4) {
  constexpr uint32_t kListTable = 0x82C44188;
  constexpr uint32_t kListTableSize = 19;
  uint32_t index = r4.u32 & 0xFFFF;
  if (index < kListTableSize) {
    return false;
  }
  uint32_t list = GuestU32(kListTable + ((index << 2) & 0x3FFFC));
  bool readable = GuestReadable(list);
  static bool reported_readable = false, reported_unreadable = false;
  bool& reported = readable ? reported_readable : reported_unreadable;
  if (!reported) {
    reported = true;
    REXLOG_INFO("doa4: list {} outside the table -> 0x{:08X} ({}, entry {})", index, list,
                readable ? "valid memory, running the original code"
                         : "not allocated, treated as empty",
                r3.u32);
  }
  return !readable;
}

// The game's busy-waits: on the Xbox 360 they only spin a hardware thread, on a PC
// each one keeps a CPU core at 100% (on handhelds that takes power away from the GPU).

// Main thread, D3D waiting for free space in the GPU ring buffer (sub_82745F60):
// the loop at 0x82745FF8 polls the GPU read pointer (with a 2 s hang check,
// sub_8274D960) until enough commands have been consumed. The GPU frees space within
// a fraction of a millisecond, so waiting briefly per iteration costs no frame time.
void doa4_ring_wait_idle() {
  IdleWait(std::chrono::microseconds(100));
}

// Job worker thread (sub_823EB468): loops forever over a 16-slot queue at r30
// (current index, then 12-byte slots; state 1 = job ready), taking a critical
// section on every check. Hook at the top of the loop (0x823EB48C): when the next
// slot has no job, wait briefly before checking again. Ready jobs run at once.
void doa4_worker_idle(PPCRegister& r30) {
  uint32_t index = GuestU32(r30.u32);
  uint32_t slot = r30.u32 + 4 + (index & 0xF) * 12;
  if (GuestU8(slot) != 1) {
    IdleWait(std::chrono::microseconds(250));
  }
}
