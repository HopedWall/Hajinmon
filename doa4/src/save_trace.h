// doa4 - save data access tracing (diagnostics)
//
// DOA4 reads/writes its profile save (ups.dat, 0x108F8 bytes plus a checksum)
// through a global buffer at 0x83195198; the game logic uses a live copy
// pointed to by the global at 0x83022320. When DOA4_CAPTURE_DIR is set and the
// file "savetrace.req" appears in that directory (content: "<seconds> [live]",
// default 10 seconds on the buffer; "live" traces the live profile), the pages
// holding the structure are made inaccessible for that long. Every access then raises an exception: the handler records
// the offset, whether it is a read or a write and the game function, re-opens
// the page, single-steps the instruction and protects the page again. At the
// end, a summary grouped by function is written to the log and to
// savetrace_N.txt. Used to find where the game keeps its unlock flags.

#pragma once

namespace doa4 {

void StartSaveTrace();

}  // namespace doa4
