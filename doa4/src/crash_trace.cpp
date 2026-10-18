// doa4 - diagnostics tool (see crash_trace.h)

#include "crash_trace.h"

#include <rex/logging/macros.h>

#if defined(_WIN32)

#include <windows.h>
// dbghelp.h and tlhelp32.h need windows.h first
#include <dbghelp.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace doa4 {
namespace {

// Guest memory is mapped starting at this host address (see the log:
// "Guest memory arena mapped: virtual base 0x0000000100000000").
constexpr uintptr_t kGuestBase = 0x100000000ull;
constexpr uintptr_t kNullPageSize = 0x10000;
constexpr uintptr_t kGuestVirtualLimit = 0x7F000000;
constexpr int kMaxFrames = 48;
constexpr int kMaxDumpFrames = 24;
constexpr int kMaxReports = 5;

std::atomic<int> g_reports{0};
std::mutex g_dbghelp_mutex;  // DbgHelp is not thread-safe

void EnsureSymbols() {
  static bool initialized = false;
  if (!initialized) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    initialized = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
  }
}

// Logs the function name and source line of a host address.
// Requires g_dbghelp_mutex.
void LogFrame(int index, DWORD64 pc) {
  HANDLE process = GetCurrentProcess();
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = MAX_SYM_NAME;
  DWORD64 displacement = 0;
  const char* name = "?";
  if (SymFromAddr(process, pc, &displacement, symbol)) name = symbol->Name;

  IMAGEHLP_LINE64 line = {};
  line.SizeOfStruct = sizeof(line);
  DWORD line_displacement = 0;
  if (SymGetLineFromAddr64(process, pc, &line_displacement, &line)) {
    REXLOG_ERROR("doa4 stack #{:2} {}+0x{:X}  ({}:{})", index, name, displacement, line.FileName,
                 line.LineNumber);
  } else {
    REXLOG_ERROR("doa4 stack #{:2} {}+0x{:X}", index, name, displacement);
  }
}

// Stack of the current thread, starting from an exception context.
void LogStack(CONTEXT context) {
  std::lock_guard lock(g_dbghelp_mutex);
  EnsureSymbols();
  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();

  STACKFRAME64 frame = {};
  frame.AddrPC.Offset = context.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;

  for (int i = 0; i < kMaxFrames; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
      break;
    }
    if (frame.AddrPC.Offset == 0) break;
    LogFrame(i, frame.AddrPC.Offset);
  }
}

// Exceptions that are part of normal operation (not crashes).
bool IsBenignException(DWORD code) {
  switch (code) {
    case 0x406D1388:  // thread name (SetThreadName)
    case 0x40010006:  // OutputDebugStringA
    case 0x4001000A:  // OutputDebugStringW
    case 0xE06D7363:  // C++ exception (handled by try/catch)
    case 0x80000026:  // STATUS_LONGJUMP (longjmp on Windows x64)
    case 0x80000003:  // breakpoint
    case 0x80000004:  // single step
      return true;
    default:
      return false;
  }
}

// Any serious exception that is not a guest memory access: log it with the
// stack (the runtime handles guest/MMIO accesses itself).
bool ReportOtherException(EXCEPTION_POINTERS* info) {
  const auto* record = info->ExceptionRecord;
  if (IsBenignException(record->ExceptionCode)) return false;
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
    const uintptr_t address = record->ExceptionInformation[1];
    if (address >= kGuestBase && address < kGuestBase + 0x200000000ull) return false;
  }
  if (g_reports.fetch_add(1) >= kMaxReports) return true;
  REXLOG_ERROR("doa4: exception 0x{:08X} at RIP=0x{:X} (parameters: 0x{:X} 0x{:X})",
               record->ExceptionCode, info->ContextRecord->Rip,
               record->NumberParameters > 0 ? record->ExceptionInformation[0] : 0,
               record->NumberParameters > 1 ? record->ExceptionInformation[1] : 0);
  LogStack(*info->ContextRecord);
  return true;
}

LONG CALLBACK ExceptionHandler(EXCEPTION_POINTERS* info) {
  const auto* record = info->ExceptionRecord;
  if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
    ReportOtherException(info);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const uintptr_t address = record->ExceptionInformation[1];
  // Game virtual memory below 0x7F000000: the runtime only uses access
  // violations for MMIO (0x7FC8xxxx GPU, 0x7FEAxxxx audio) and for physical
  // memory (0xA0000000+), so an exception here is a real error.
  if (address < kGuestBase || address >= kGuestBase + kGuestVirtualLimit) {
    ReportOtherException(info);  // only logs accesses outside guest memory
    return EXCEPTION_CONTINUE_SEARCH;  // MMIO and other accesses handled by the runtime
  }
  if (g_reports.fetch_add(1) >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;

  REXLOG_ERROR("doa4: invalid game memory access ({} of guest 0x{:X}{}), RIP=0x{:X}",
               record->ExceptionInformation[0] ? "write" : "read", address - kGuestBase,
               address - kGuestBase < kNullPageSize ? ", null pointer" : "",
               info->ContextRecord->Rip);
  LogStack(*info->ContextRecord);
  return EXCEPTION_CONTINUE_SEARCH;
}

// --- Stack dump of all threads (to diagnose hangs) ---

// Collects return addresses with RtlVirtualUnwind: it does not allocate, so it
// is safe while the thread is suspended (it may hold the heap lock). Symbol
// names are resolved after the thread is resumed.
std::vector<DWORD64> UnwindRaw(CONTEXT context) {
  std::vector<DWORD64> pcs;
  pcs.reserve(kMaxDumpFrames);
  for (int i = 0; i < kMaxDumpFrames && context.Rip; ++i) {
    pcs.push_back(context.Rip);
    DWORD64 image_base = 0;
    auto* entry = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!entry) {
      // leaf function: the return address is on top of the stack
      context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
      context.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, entry, &context, &handler_data,
                     &establisher, nullptr);
  }
  return pcs;
}

double CpuSeconds(HANDLE thread) {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return 0;
  auto to_100ns = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
  return (to_100ns(kernel) + to_100ns(user)) / 1e7;
}

std::string ThreadName(HANDLE thread) {
  std::string result;
  PWSTR wide = nullptr;
  if (SUCCEEDED(GetThreadDescription(thread, &wide)) && wide) {
    int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size > 1) {
      result.resize(size - 1);
      WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), size, nullptr, nullptr);
    }
    LocalFree(wide);
  }
  return result;
}

void DumpAllThreads(int round) {
  const DWORD self = GetCurrentThreadId();
  const DWORD pid = GetCurrentProcessId();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;

  REXLOG_ERROR("doa4: ===== stack dump #{} =====", round);
  THREADENTRY32 te = {sizeof(te)};
  for (BOOL ok = Thread32First(snapshot, &te); ok; ok = Thread32Next(snapshot, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_LIMITED_INFORMATION,
                               FALSE, te.th32ThreadID);
    if (!thread) continue;

    std::vector<DWORD64> pcs;
    if (SuspendThread(thread) != DWORD(-1)) {
      CONTEXT context = {};
      context.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(thread, &context)) pcs = UnwindRaw(context);
      ResumeThread(thread);
    }

    REXLOG_ERROR("doa4: thread {} '{}' CPU={:.1f}s", te.th32ThreadID, ThreadName(thread),
                 CpuSeconds(thread));
    {
      std::lock_guard lock(g_dbghelp_mutex);
      EnsureSymbols();
      for (size_t i = 0; i < pcs.size(); ++i) LogFrame(int(i), pcs[i]);
    }
    CloseHandle(thread);
  }
  CloseHandle(snapshot);
}

int EnvInt(const char* name, int fallback) {
  char value[32];
  DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
  return length > 0 && length < sizeof(value) ? std::atoi(value) : fallback;
}

}  // namespace

void InstallCrashTrace() {
  // 1 = first in the chain: we are called before the runtime's handler
  AddVectoredExceptionHandler(1, ExceptionHandler);
  REXLOG_INFO("doa4: crash trace enabled");

  // DOA4_STACK_DUMP_SECONDS=N: after N seconds, log the stack of every thread;
  // DOA4_STACK_DUMP_COUNT=K repeats it K times, N seconds apart.
  const int seconds = EnvInt("DOA4_STACK_DUMP_SECONDS", 0);
  if (seconds > 0) {
    const int count = EnvInt("DOA4_STACK_DUMP_COUNT", 1);
    REXLOG_INFO("doa4: stack dump every {}s ({} times)", seconds, count);
    std::thread([seconds, count] {
      for (int round = 1; round <= count; ++round) {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        DumpAllThreads(round);
      }
    }).detach();
  }
}

}  // namespace doa4

#else

namespace doa4 {
void InstallCrashTrace() {}
}  // namespace doa4

#endif
