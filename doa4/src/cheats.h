// doa4 - cheats (enabled with command-line options, see cheats.cpp)
//
// --cheat_unlock_characters  unlocks every playable character
// --cheat_unlock_costumes    unlocks every costume of every playable character
// --cheat_unlock_announcer_voices  unlocks every announcer voice and marks Story
//                            mode cleared (with all costumes: the Tatami stage)
// --cheat_play_alpha152      the random choice picks Alpha-152 (experimental)
//
// The unlocks are written to the live profile, so they are stored in the save
// the next time the game saves, like unlocks earned by playing.

#pragma once

namespace doa4 {

void StartCheats();

}  // namespace doa4
