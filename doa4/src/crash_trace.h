// doa4 - diagnostics tool
//
// Registers a vectored exception handler that logs the call stack of serious
// exceptions (division by zero, illegal instructions, host memory access
// violations...) and of reads/writes to invalid guest memory addresses (e.g.
// null pointers), with symbols resolved from the PDB. Recompiled functions are
// named sub_XXXXXXXX (original address in the XEX), so the stack points
// straight to the relevant game code. It does not change behavior: the
// exception continues to the runtime's handler.
//
// DOA4_STACK_DUMP_SECONDS / DOA4_STACK_DUMP_COUNT (environment variables) also
// log the stacks of all threads periodically, to diagnose hangs.

#pragma once

namespace doa4 {

void InstallCrashTrace();

}  // namespace doa4
