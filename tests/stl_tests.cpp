#include "browser.hpp"
#include "stl.hpp"

#include <array>
#include <bit>
#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

namespace {

void require(bool value, const char* message)
{
    if (!value) {
        throw std::runtime_error(message);
    }
}

void word(std::ostream& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out.put(char(value >> (8 * i)));
    }
}

void number(std::ostream& out, float value)
{
    word(out, std::bit_cast<uint32_t>(value));
}

void binary(const std::filesystem::path& path, uint32_t count = 4, bool invalid = false)
{
    std::ofstream out(path, std::ios::binary);
    std::string header = "solid binary fixture";
    header.resize(80, '\0');
    out.write(header.data(), 80);
    word(out, count);
    std::array<glm::vec3, 4> v { { { 0, 0, 0 }, { 10, 0, 0 }, { 0, 20, 0 }, { 0, 0, 30 } } };
    int faces[4][3] = { { 0, 2, 1 }, { 0, 1, 3 }, { 0, 3, 2 }, { 1, 2, 3 } };
    for (uint32_t i = 0; i < count; ++i) {
        for (int a = 0; a < 3; ++a) {
            number(out, 0);
        }

        for (int j = 0; j < 3; ++j) {
            for (int a = 0; a < 3; ++a) {
                number(out,
                    invalid && i == 0 && j == 0 && a == 0 ? std::numeric_limits<float>::infinity()
                                                          : v[faces[i % 4][j]][a]);
            }
        }

        out.put(0);
        out.put(0);
    }
}

template <class F> void fails(F fn)
{
    bool caught = false;
    try {
        fn();
    } catch (const std::exception&) {
        caught = true;
    }

    require(caught, "Expected parser failure");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        auto folder = std::filesystem::current_path() / "test-output";
        std::filesystem::create_directories(folder);
        auto p = folder / "tetra.stl";
        binary(p);
        auto m = si::inspectStl(p);
        require(m.binary && m.triangles == 4, "Binary detection/count");
        require(m.minimum == glm::dvec3(0) && m.maximum == glm::dvec3(10, 20, 30), "Binary bounds");

        size_t calls = 0;
        size_t vertices = 0;
        si::streamStl(
            p, m,
            [&](auto chunk) {
                ++calls;
                vertices += chunk.size();
                for (auto v : chunk) {
                    require(glm::all(glm::lessThanEqual(glm::abs(v), glm::vec3(.5f))), "Normalized bounds");
                }
            },
            {}, {}, 1);
        require(calls == 4 && vertices == 12, "Streaming chunk boundaries");

        auto ascii = folder / "ascii.stl";
        {
            std::ofstream out(ascii);
            out << "solid sample\nfacet normal 0 0 1\nouter loop\nvertex +0 0 0\nvertex 1e1 0 0\nvertex 0 "
                   "2E1 0\nendloop\nendfacet\nendsolid sample\n";
        }

        auto a = si::inspectStl(ascii);
        require(!a.binary && a.triangles == 1 && a.maximum == glm::dvec3(10, 20, 0), "ASCII bounds");

        auto bad = folder / "bad.stl";
        binary(bad, 4, true);
        fails([&] {
            si::inspectStl(bad);
        });

        binary(bad);
        std::filesystem::resize_file(bad, 100);
        fails([&] {
            si::inspectStl(bad);
        });

        binary(bad, 0);
        fails([&] {
            si::inspectStl(bad);
        });

        fails([&] {
            si::inspectStl(p, [] {
                return true;
            });
        });

        binary(bad);
        {
            std::ofstream out(bad, std::ios::app | std::ios::binary);
            out << "extra";
        }

        require(si::inspectStl(bad).trailingBytes, "Trailing data warning");

        {
            std::ofstream out(bad);
            out << "solid sample\nfacet normal 0 0 1\nouter loop\nvertex nan 0 0\nvertex 1 0 0\nvertex 0 1 "
                   "0\nendloop\nendfacet\nendsolid\n";
        }

        fails([&] {
            si::inspectStl(bad);
        });

        auto unicode = folder / si::fromUtf8("pièce_模型.STL");
        binary(unicode);
        require(si::inspectStl(unicode).triangles == 4, "Unicode file path");

        auto listing = si::enumerate(folder, false);
        require(listing.files.size() >= 4, "STL directory enumeration");
        auto limited = si::enumerate(folder, false, {}, 1);
        require(limited.partial, "Bounded directory listing");

        auto filterRoot = folder / "folder-filter";
        std::filesystem::create_directories(filterRoot / "__MACOSX");
        std::filesystem::create_directories(filterRoot / "normal" / "__macosx");
        std::filesystem::create_directories(filterRoot / "__MACOSX_notes");
        binary(filterRoot / "__MACOSX" / "ignored.stl");
        binary(filterRoot / "normal" / "__macosx" / "also-ignored.stl");
        binary(filterRoot / "normal" / "visible.stl");
        binary(filterRoot / "__MACOSX_notes" / "visible-too.stl");

        auto direct = si::enumerate(filterRoot, false);
        require(direct.folders.size() == 2, "Mac metadata directory excluded from folder tree");
        auto recursive = si::enumerate(filterRoot, true);
        require(recursive.files.size() == 2, "Mac metadata subtrees excluded from recursive/prebuild scan");
        require(si::enumerate(filterRoot / "__MACOSX", true).files.empty(), "Ignored root directory");

        binary(p, 20000);
        auto large = si::inspectStl(p);
        size_t streamed = 0;
        si::streamStl(
            p, large,
            [&](auto v) {
                streamed += v.size() / 3;
            },
            {}, {}, 7);
        require(streamed == 20000, "Multiple read/chunk boundaries");
        binary(p);
        if (argc > 1 && std::string(argv[1]) == "--large") {
            auto largePath = folder / "large.stl";
            binary(largePath, 20000000);
            std::cout << "Generated 1 GB fixture: " << si::utf8(largePath) << '\n';
        }

        std::cout << "All STL parser, streaming, cancellation, Unicode, and directory tests passed\n";

        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';

        return 1;
    }
}
