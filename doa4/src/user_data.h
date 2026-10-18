// doa4 - user data folder (saves, shader cache)
#pragma once

#include <rex/rex_app.h>

namespace doa4 {

// The app is named "Hajinmon", so the runtime keeps saves and the shader cache
// in Documents\Hajinmon. Builds before the rename used Documents\doa4: move it
// to the new location the first time, or keep using it if it cannot be moved.
void MigrateUserData(rex::PathConfig& paths);

}  // namespace doa4
