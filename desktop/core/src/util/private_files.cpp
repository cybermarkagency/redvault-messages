// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/util/private_files.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <vector>
#endif

namespace fs = std::filesystem;

namespace redvault::util {

#ifdef _WIN32

void restrictNewFiles() {}

void makePrivateDir(const fs::path& dir) {
    fs::create_directories(dir);
}

void makePrivateDir(const fs::path& dir, std::error_code& error) {
    fs::create_directories(dir, error);
}

void makePrivateFile(const fs::path&) {}

void tightenTree(const fs::path&) {}

#else

void restrictNewFiles() {
    ::umask(077);
}

void makePrivateDir(const fs::path& dir, std::error_code& error) {
    error.clear();
    // From the deepest folder that's there already, so no folder on the way down is ever open to others.
    std::vector<fs::path> missing;
    std::error_code ignored;
    for (fs::path p = dir; !p.empty() && !fs::exists(p, ignored); p = p.parent_path()) {
        missing.push_back(p);
        if (p == p.parent_path()) break;
    }
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        if (::mkdir(it->c_str(), 0700) != 0 && errno != EEXIST) {
            error.assign(errno, std::generic_category());
            return;
        }
    }
    if (::chmod(dir.c_str(), 0700) != 0) error.assign(errno, std::generic_category());
}

void makePrivateDir(const fs::path& dir) {
    std::error_code error;
    makePrivateDir(dir, error);
    if (error) throw fs::filesystem_error("Couldn't create a private folder", dir, error);
}

void makePrivateFile(const fs::path& file) {
    const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return;  // the stream that follows reports it
    ::fchmod(fd, 0600);
    ::close(fd);
}

void tightenTree(const fs::path& root) {
    std::error_code error;
    const auto top = fs::symlink_status(root, error);
    if (error) return;
    if (fs::is_regular_file(top)) {
        ::chmod(root.c_str(), 0600);
        return;
    }
    if (!fs::is_directory(top)) return;
    ::chmod(root.c_str(), 0700);
    // Links aren't followed by default, and a link's own target is left alone below.
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error);
    const fs::recursive_directory_iterator end;
    while (!error && it != end) {
        const auto status = it->symlink_status(error);
        if (!error) {
            if (fs::is_directory(status)) ::chmod(it->path().c_str(), 0700);
            else if (fs::is_regular_file(status)) ::chmod(it->path().c_str(), 0600);
        }
        error.clear();
        it.increment(error);
    }
}

#endif

}  // namespace redvault::util
