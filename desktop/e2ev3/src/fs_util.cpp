#include "fs_util.hpp"

#include "redvault/util/private_files.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace redvault::e2ev3::fsutil {

void writeAtomic(const std::filesystem::path& path, const std::string& data) {
    std::error_code ec;
    util::makePrivateDir(path.parent_path(), ec);
    auto temp = path;
    temp += ".tmp";
#ifndef _WIN32
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) throw std::runtime_error("couldn't write " + path.string());
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n <= 0) {
            ::close(fd);
            throw std::runtime_error("couldn't write " + path.string());
        }
        done += static_cast<std::size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
#else
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) throw std::runtime_error("couldn't write " + path.string());
    }
#endif
    // rename() replaces an existing file on POSIX and, with std::filesystem, on Windows too.
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        throw std::runtime_error("couldn't write " + path.string());
    }
}

std::optional<std::string> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace redvault::e2ev3::fsutil
