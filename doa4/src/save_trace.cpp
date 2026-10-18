// doa4 - save data access tracing (see save_trace.h)

#include "save_trace.h"

#include <rex/logging/macros.h>

#if defined(_WIN32)

#include <windows.h>
// dbghelp.h needs windows.h first
#include <dbghelp.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace doa4 {
namespace {

constexpr uintptr_t kGuestBase = 0x100000000ull;
// Save file buffer (read/written by the save manager).
constexpr uint32_t kSaveAddress = 0x83195198;
constexpr uint32_t kSaveSize = 0x10900;
// Global pointer to the live profile used by the game logic.
constexpr uint32_t kLiveProfilePointer = 0x83022320;
constexpr uintptr_t kPageSize = 0x1000;

// Traced range (host addresses), set when a trace starts.
uintptr_t g_begin = 0, g_end = 0, g_pages_begin = 0, g_pages_end = 0;

uint32_t GuestU32(uint32_t address) {
  uint32_t value;
  memcpy(&value, reinterpret_cast<const void*>(kGuestBase + address), sizeof(value));
  return _byteswap_ulong(value);
}

std::atomic<bool> g_active{false};
DWORD g_original_protect = PAGE_READWRITE;
thread_local void* t_pending_page = nullptr;

struct Access {
  uint32_t count = 0;
  DWORD64 rip = 0;
};
std::mutex g_mutex;
// key: offset << 1 | write
std::map<uint32_t, Access> g_accesses;

void ProtectPages(DWORD protect) {
  DWORD old;
  VirtualProtect(reinterpret_cast<void*>(g_pages_begin), g_pages_end - g_pages_begin, protect,
                 &old);
}

LONG CALLBACK SaveTraceHandler(EXCEPTION_POINTERS* info) {
  const auto* record = info->ExceptionRecord;
  if (record->ExceptionCode == EXCEPTION_SINGLE_STEP) {
    if (!t_pending_page) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    VirtualProtect(t_pending_page, kPageSize,
                   g_active ? PAGE_NOACCESS : g_original_protect, &old);
    t_pending_page = nullptr;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const uintptr_t address = record->ExceptionInformation[1];
  if (address < g_pages_begin || address >= g_pages_end) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (address >= g_begin && address < g_end) {
    uint32_t key = uint32_t(address - g_begin) << 1 |
                   (record->ExceptionInformation[0] ? 1u : 0u);
    std::lock_guard lock(g_mutex);
    Access& access = g_accesses[key];
    if (!access.count++) access.rip = info->ContextRecord->Rip;
  }
  void* page = reinterpret_cast<void*>(address & ~(kPageSize - 1));
  DWORD old;
  VirtualProtect(page, kPageSize, g_original_protect, &old);
  t_pending_page = page;
  info->ContextRecord->EFlags |= 0x100;  // trap flag: single-step this instruction
  return EXCEPTION_CONTINUE_EXECUTION;
}

std::string SymbolName(DWORD64 address) {
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = MAX_SYM_NAME;
  DWORD64 displacement = 0;
  if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
    std::string name = symbol->Name;
    if (name.rfind("__imp__", 0) == 0) name = name.substr(7);
    return name;
  }
  return "?";
}

void WriteSummary(const std::filesystem::path& path, int round) {
  std::map<uint32_t, Access> accesses;
  {
    std::lock_guard lock(g_mutex);
    accesses.swap(g_accesses);
  }
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);  // may already be initialized

  // Group by function: function -> list of "offset R/W xcount".
  std::map<std::string, std::set<std::string>> by_function;
  for (const auto& [key, access] : accesses) {
    char entry[64];
    snprintf(entry, sizeof(entry), "%05X%s", key >> 1, (key & 1) ? "W" : "R");
    by_function[SymbolName(access.rip)].insert(entry);
  }

  std::ofstream out(path);
  out << "save trace #" << round << ": " << accesses.size() << " distinct accesses\n";
  REXLOG_INFO("doa4: save trace #{}: {} distinct accesses, {} functions", round, accesses.size(),
              by_function.size());
  for (const auto& [function, entries] : by_function) {
    std::string line = function + ":";
    for (const auto& entry : entries) line += " " + entry;
    out << line << "\n";
    REXLOG_INFO("doa4:   {}", line.size() > 400 ? line.substr(0, 400) + " ..." : line);
  }
}

void TraceLoop(std::filesystem::path dir) {
  const auto request = dir / "savetrace.req";
  int round = 0;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::error_code ec;
    if (!std::filesystem::exists(request, ec)) continue;
    // Request format: "<seconds> [live]".
    int seconds = 10;
    std::string target;
    {
      std::ifstream in(request);
      in >> seconds;
      if (!in || seconds < 1) seconds = 10;
      if (seconds > 120) seconds = 120;
      in >> target;
    }
    std::filesystem::remove(request, ec);

    uint32_t guest = kSaveAddress;
    if (target == "live") {
      guest = GuestU32(kLiveProfilePointer);
      if (!guest || guest >= 0xE0000000) {
        REXLOG_WARN("doa4: live profile pointer is 0x{:08X}, trace skipped", guest);
        continue;
      }
      REXLOG_INFO("doa4: live profile at 0x{:08X}, first bytes {:08X} {:08X} {:08X} {:08X}",
                  guest, GuestU32(guest), GuestU32(guest + 4), GuestU32(guest + 8),
                  GuestU32(guest + 12));
    }
    g_begin = kGuestBase + guest;
    g_end = g_begin + kSaveSize;
    g_pages_begin = g_begin & ~(kPageSize - 1);
    g_pages_end = (g_end + kPageSize - 1) & ~(kPageSize - 1);

    ++round;
    REXLOG_INFO("doa4: save trace #{} ({}) started for {}s", round,
                target == "live" ? "live profile" : "save buffer", seconds);
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<void*>(g_pages_begin), &mbi, sizeof(mbi))) {
      g_original_protect = mbi.Protect;
    }
    g_active = true;
    ProtectPages(PAGE_NOACCESS);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    g_active = false;
    ProtectPages(g_original_protect);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let pending steps finish
    WriteSummary(dir / ("savetrace_" + std::to_string(round) + ".txt"), round);
  }
}

}  // namespace

void StartSaveTrace() {
  char dir[MAX_PATH];
  DWORD length = GetEnvironmentVariableA("DOA4_CAPTURE_DIR", dir, sizeof(dir));
  if (!length || length >= sizeof(dir)) return;
  // 1 = first in the chain: called before the crash trace and runtime handlers.
  AddVectoredExceptionHandler(1, SaveTraceHandler);
  std::thread(TraceLoop, std::filesystem::path(dir)).detach();
}

}  // namespace doa4

#else

namespace doa4 {
void StartSaveTrace() {}
}  // namespace doa4

#endif
