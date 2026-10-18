// doa4 - default keyboard layout (see input_defaults.h)

#include "input_defaults.h"

#include <rex/cvar.h>
#include <rex/logging/macros.h>

namespace doa4 {
namespace {

struct KeyDefault {
  const char* option;
  const char* keys;
};

// Face buttons on S/D/A/W (A/B/X/Y), triggers on Q/E, Start on Enter and the
// arrow keys driving both the left stick and the D-pad. Bindings that are not
// listed keep the runtime's default.
constexpr KeyDefault kKeyDefaults[] = {
    // Keyboard and mouse work out of the box, next to any controller.
    {"mnk_mode", "true"},
    {"keybind_a", "S"},
    {"keybind_b", "D"},
    {"keybind_x", "A"},
    {"keybind_y", "W"},
    {"keybind_start", "Return"},
    {"keybind_left_trigger", "Q"},
    {"keybind_right_trigger", "E"},
    {"keybind_lstick_up", "Up"},
    {"keybind_lstick_down", "Down"},
    {"keybind_lstick_left", "Left"},
    {"keybind_lstick_right", "Right"},
    {"keybind_dpad_up", "Up"},
    {"keybind_dpad_down", "Down"},
    {"keybind_dpad_left", "Left"},
    {"keybind_dpad_right", "Right"},
    // The arrow keys now belong to the left stick and the D-pad.
    {"keybind_rstick_up", "None"},
    {"keybind_rstick_down", "None"},
    {"keybind_rstick_left", "None"},
    {"keybind_rstick_right", "None"},
};

}  // namespace

void ApplyInputDefaults() {
  for (const auto& entry : kKeyDefaults) {
    if (rex::cvar::GetFlagSource(entry.option) != rex::cvar::Source::kDefault) {
      REXLOG_DEBUG("doa4: keeping {}={}", entry.option, rex::cvar::GetFlagByName(entry.option));
      continue;
    }
    if (!rex::cvar::SetFlagByName(entry.option, entry.keys)) {
      REXLOG_WARN("doa4: could not set the default for {}", entry.option);
    }
  }
}

}  // namespace doa4
