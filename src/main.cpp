// MORS - a modern C++23 chess engine
// Copyright (C) 2026 Theodore Magnus Øen
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "uci.hpp"

#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    // MORS emits UTF-8 UCI text. Match the Windows console code pages to the
    // execution character set so names such as "Øen" are not mojibake.
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (argc <= 1 || std::string_view(argv[1]) == "uci")
        return mors::run_uci();
    return mors::run_bench(argc, argv);
}
