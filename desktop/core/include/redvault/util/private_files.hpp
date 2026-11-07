#pragma once

#include <filesystem>
#include <system_error>

// What's on disk stays this user's. Windows already keeps a user's profile to that
// user, so all of this is about Linux, where new files are normally readable by
// every other account on the computer. Decrypted files in the cache would be too.
// On Windows the folder functions just create folders and the rest does nothing.

namespace redvault::util {

/// umask 077 for the whole process: everything created from now on, by anything (Qt, Tor), is this user's alone.
void restrictNewFiles();

/// Creates a folder and any missing parents, rwx for this user only, and tightens the folder itself if it existed.
void makePrivateDir(const std::filesystem::path& dir);
void makePrivateDir(const std::filesystem::path& dir, std::error_code& error);

/// Makes `file` exist with rw for this user only. Call before opening a stream: one that's already there keeps its mode.
void makePrivateFile(const std::filesystem::path& file);

/**
 * Tightens what an earlier version, or a crash, left behind: folders to 0700 and files to
 * 0600, everywhere under `root`. A symbolic link is never followed. Best effort: a file
 * it can't change is left as it was.
 */
void tightenTree(const std::filesystem::path& root);

}  // namespace redvault::util
