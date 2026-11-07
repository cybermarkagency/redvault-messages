// Desktop (Windows and Linux) test driver: the C++ side of the C ABI.
// Reads one JSON request per line on stdin, answers one JSON response per
// line on stdout, through rv_e2ee_call, exactly as the desktop app would.
#include <cstdint>
#include <iostream>
#include <string>

#include "redvault_e2ee.h"

int main() {
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::size_t length = 0;
        std::uint8_t* response = rv_e2ee_call(reinterpret_cast<const std::uint8_t*>(line.data()), line.size(), &length);
        std::cout.write(reinterpret_cast<const char*>(response), static_cast<std::streamsize>(length));
        std::cout << '\n' << std::flush;
        rv_e2ee_free(response, length);
    }
    return 0;
}
