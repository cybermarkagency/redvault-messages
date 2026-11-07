#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace redvault::e2ev3::fsutil {

/// Writes `data` to `path` through a temporary file in the same folder and a rename, readable by the owner only
/// (0600 on Linux; on Windows the folder's ACL, i.e. the user's profile, applies). A crash leaves the old file or the new one.
void writeAtomic(const std::filesystem::path& path, const std::string& data);

std::optional<std::string> readAll(const std::filesystem::path& path);

}  // namespace redvault::e2ev3::fsutil
