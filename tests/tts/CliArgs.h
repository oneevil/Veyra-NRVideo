// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Command-line arguments of the voice tools, the same on every platform: paths as paths and
// text as UTF-8. (On Windows main()'s argv is in the ANSI code page, which loses Cyrillic.)
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

struct CliArgs {
    std::vector<std::filesystem::path> paths;
    std::vector<std::string> utf8;

    CliArgs(int argc, char** argv) {
#ifdef _WIN32
        (void)argc;
        (void)argv;
        SetConsoleOutputCP(CP_UTF8); // the tools print UTF-8
        int count = 0;
        wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
        for (int i = 0; wide && i < count; ++i) {
            paths.emplace_back(wide[i]);
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string text(bytes > 1 ? size_t(bytes - 1) : 0, '\0');
            if (bytes > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), bytes, nullptr, nullptr);
            utf8.push_back(std::move(text));
        }
        if (wide) LocalFree(wide);
#else
        for (int i = 0; i < argc; ++i) {
            paths.emplace_back(argv[i]);
            utf8.emplace_back(argv[i]);
        }
#endif
    }
    size_t size() const { return utf8.size(); }
};
