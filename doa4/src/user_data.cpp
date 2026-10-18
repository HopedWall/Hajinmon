// doa4 - user data folder migration (see user_data.h)
//
// Runs from OnConfigurePaths, before logging is set up: nothing is logged here.

#include "user_data.h"

#include <filesystem>
#include <system_error>

#include <rex/filesystem.h>

namespace doa4 {

void MigrateUserData(rex::PathConfig& paths) {
  namespace fs = std::filesystem;
  const fs::path documents = rex::filesystem::GetUserFolder();
  const fs::path current = documents / "Hajinmon";
  const fs::path legacy = documents / "doa4";

  // Only the default location is migrated: a folder chosen with
  // --user_data_root is used as it is.
  if (paths.user_data_root != current) return;

  std::error_code ec;
  if (fs::exists(current, ec) || !fs::is_directory(legacy, ec)) return;

  fs::rename(legacy, current, ec);
  if (!ec) return;

  // Could not move it (e.g. a file in it is open): keep using the old folder.
  const bool default_cache = paths.cache_root == current / "cache";
  paths.user_data_root = legacy;
  if (default_cache) paths.cache_root = legacy / "cache";
}

}  // namespace doa4
