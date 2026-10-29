// doa4 - cheats (see cheats.h)
//
// How DOA4 stores unlocks (found by tracing the save data accesses):
// - The profile save (ups.dat) lives in a global buffer at 0x83195198. The game
//   logic reaches its progress section through the pointer at 0x83022320,
//   which points to buffer + 0x7968 once a profile is loaded.
// - Three per-character byte tables follow, indexed by slot (the character ID
//   is mapped to a slot by the table at 0x82C4A038):
//     progress + 0x3FC2 + slot: number of unlocked costumes
//     progress + 0x3FD9 + slot: character unlocked (selectable)
//     progress + 0x3FF0 + slot: Story mode cleared
// - The maximum number of costumes per character ID is the table at
//   0x82C24850 (sub_823F70C8 checks it before sub_823F7020 unlocks the next
//   costume).
// - The playable characters are the IDs in the 24-slot character select grid
//   (0x82F4B5F0, 40-byte entries starting with the ID): 0-15, 17-21 and 24.
//
// - Alpha-152, the final boss, is character ID 16: it is not in the grid, but
//   it has a profile slot (21) and 2 costumes in the tables above (the same
//   model twice, so only 1 is unlocked). The last
//   grid entry (22) is the random choice, ID 0xFF.
//
// A background thread waits until a profile is loaded and then keeps these
// values set, so they also survive profile reloads.

#include "cheats.h"

#include <chrono>
#include <bit>
#include <cstdint>
#include <cstring>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(cheat_unlock_characters, false, "Cheats",
                    "Unlock every playable character");
REXCVAR_DEFINE_BOOL(cheat_unlock_costumes, false, "Cheats",
                    "Unlock every costume of every playable character");
REXCVAR_DEFINE_BOOL(cheat_play_alpha152, false, "Cheats",
                    "Experimental: the random choice of the character select screen picks Alpha-152");

namespace doa4 {
namespace {

// Host address of guest address 0: the SDK maps guest memory at the first free
// power-of-two base from 4 GB up, which is not always 4 GB.
uintptr_t GuestBase() {
  return reinterpret_cast<uintptr_t>(REX_KERNEL_MEMORY()->virtual_membase());
}
constexpr uint32_t kSaveBuffer = 0x83195198;
constexpr uint32_t kProgressPointer = 0x83022320;
constexpr uint32_t kProgressOffset = 0x7968;
constexpr uint32_t kCostumeCounts = 0x3FC2;
constexpr uint32_t kCharacterUnlocked = 0x3FD9;
constexpr uint32_t kSlotCount = 23;
constexpr uint32_t kIdToSlotTable = 0x82C4A038;
constexpr uint32_t kMaxCostumesTable = 0x82C24850;
constexpr uint32_t kSelectGrid = 0x82F4B5F0;
constexpr uint32_t kSelectGridEntrySize = 40;
constexpr uint32_t kRandomEntry = 22;
constexpr uint8_t kAlpha152 = 16;
// Character IDs in the full character select grid (excluding empty slots).
constexpr uint8_t kPlayableIds[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                                    12, 13, 14, 15, 17, 18, 19, 20, 21, 24};

uint8_t* Guest(uint32_t address) { return reinterpret_cast<uint8_t*>(GuestBase() + address); }

uint32_t GuestU32(uint32_t address) {
  uint32_t value;
  std::memcpy(&value, Guest(address), sizeof(value));
  return std::byteswap(value);
}

// Alpha-152 in place of the random choice, unlocked, with its costumes.
int ApplyAlpha152(uint32_t progress) {
  int changed = 0;
  uint8_t* grid_id = Guest(kSelectGrid + kRandomEntry * kSelectGridEntrySize);
  if (*grid_id != kAlpha152) {
    *grid_id = kAlpha152;
    ++changed;
  }
  if (progress) {
    uint8_t slot = *Guest(kIdToSlotTable + kAlpha152);
    if (slot < kSlotCount) {
      uint8_t* unlocked = Guest(progress + kCharacterUnlocked + slot);
      if (*unlocked == 0) {
        *unlocked = 1;
        ++changed;
      }
      // One costume: the table says 2, but the second is a copy of the
      // first (alp01 twice in chrthree.afs).
      uint8_t* count = Guest(progress + kCostumeCounts + slot);
      if (*count != 1) {
        *count = 1;
        ++changed;
      }
    }
  }
  return changed;
}

// Applies the enabled cheats; returns the number of bytes changed.
int Apply(uint32_t progress, bool characters, bool costumes) {
  int changed = 0;
  for (uint8_t id : kPlayableIds) {
    uint8_t slot = *Guest(kIdToSlotTable + id);
    if (slot >= kSlotCount) continue;
    if (characters) {
      uint8_t* unlocked = Guest(progress + kCharacterUnlocked + slot);
      if (*unlocked == 0) {
        *unlocked = 1;
        ++changed;
      }
    }
    if (costumes) {
      uint8_t max = *Guest(kMaxCostumesTable + id);
      uint8_t* count = Guest(progress + kCostumeCounts + slot);
      if (max && *count < max) {
        *count = max;
        ++changed;
      }
    }
  }
  return changed;
}

void CheatLoop(bool characters, bool costumes, bool alpha152) {
  bool reported = false;
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    // Only once a profile is loaded: the pointer then targets the save buffer.
    uint32_t progress = GuestU32(kProgressPointer);
    bool loaded = progress == kSaveBuffer + kProgressOffset;
    int changed = alpha152 ? ApplyAlpha152(loaded ? progress : 0) : 0;
    if (!loaded) continue;
    changed += Apply(progress, characters, costumes);
    if (changed && !reported) {
      reported = true;
      REXLOG_INFO("doa4: cheats applied to the loaded profile ({} values changed)", changed);
    }
  }
}

}  // namespace

void StartCheats() {
  bool characters = REXCVAR_GET(cheat_unlock_characters);
  bool costumes = REXCVAR_GET(cheat_unlock_costumes);
  bool alpha152 = REXCVAR_GET(cheat_play_alpha152);
  if (!characters && !costumes && !alpha152) return;
  REXLOG_INFO("doa4: cheats enabled:{}{}{}", characters ? " all characters" : "",
              costumes ? " all costumes" : "", alpha152 ? " Alpha-152" : "");
  std::thread(CheatLoop, characters, costumes, alpha152).detach();
}

}  // namespace doa4
