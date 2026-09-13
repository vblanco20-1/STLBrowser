#include "application.hpp"

#include <iostream>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

int main(int argc, char** argv)
{
    try {
        std::vector<std::string> args;
#ifdef _WIN32
        (void)argc;
        (void)argv;
        int count = 0;
        auto wide = CommandLineToArgvW(GetCommandLineW(), &count);
        for (int i = 1; i < count; ++i) {
            args.push_back(si::utf8(std::filesystem::path(wide[i])));
        }

        LocalFree(wide);
#else
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }
#endif

        si::Options options;
#ifndef NDEBUG
        options.validation = true;
#endif

        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--validation") {
                options.validation = true;
            } else if (args[i] == "--no-validation") {
                options.validation = false;
            } else if (args[i] == "--single-queue") {
                options.singleQueue = true;
            } else if (args[i] == "--self-test") {
                options.selfTest = true;
            } else if (args[i] == "--exercise") {
                options.exercise = true;
            } else if (args[i] == "--smoke" && i + 1 < args.size()) {
                options.smokeSeconds = std::stoi(args[++i]);
            } else if (args[i] == "--screenshot" && i + 1 < args.size()) {
                options.screenshot = si::fromUtf8(args[++i]);
            } else if (args[i] == "--help") {
                std::cout << "STLInspector [directory] [--validation] [--single-queue] [--smoke seconds] "
                             "[--exercise]\nSTLInspector --self-test file.stl [--screenshot output.ppm]\n";

                return 0;
            } else {
                options.root = si::fromUtf8(args[i]);
            }
        }

        si::Application app(options);

        return app.run();
    } catch (const std::exception& e) {
        std::cerr << "STL Inspector: " << e.what() << '\n';

        return 1;
    }
}
