// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// main() for redvault_files_preview_tests. The viewer always holds a MediaPlayer,
// so Qt Multimedia is loaded. On Windows its static clean-up runs while the DLLs
// unload, after Windows has already stopped every other thread, and it can wait
// on one of those threads forever: the tests pass, then the program never exits
// (seen 2026-11-17 on "a picture fits the window"). Once Catch has reported, the
// process ends without running that clean-up.

#include <catch2/catch_session.hpp>

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char* argv[]) {
    const int result = Catch::Session().run(argc, argv);
    std::fflush(stdout);
    std::fflush(stderr);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(result));
#endif
    return result;
}
