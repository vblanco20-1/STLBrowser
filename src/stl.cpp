#include "stl.hpp"
#include <array>
#include <bit>
#include <charconv>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace si {
namespace {
uint32_t u32(const unsigned char* b) {
    return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}
void exact(FileReader& f, void* dst, size_t n) {
    auto* b = static_cast<unsigned char*>(dst);
    while (n) {
        auto got = f.read(b, n);
        if (!got)
            throw std::runtime_error("Truncated STL record");
        b += got;
        n -= got;
    }
}
struct Tokens {
    FileReader& file;
    std::array<char, 65536> data{};
    size_t index = 0, count = 0;
    int peek() {
        if (index == count) {
            count = file.read(data.data(), data.size());
            index = 0;
        }
        return count ? static_cast<unsigned char>(data[index]) : -1;
    }
    int get() {
        int c = peek();
        if (c >= 0)
            ++index;
        return c;
    }
    std::string next() {
        std::string s;
        int c;
        while ((c = peek()) >= 0 && c <= 32)
            get();
        while ((c = peek()) > 32) {
            if (s.size() >= 256)
                throw std::runtime_error("ASCII STL token too long");
            s += char(get());
        }
        return s;
    }
    void line() {
        int c;
        size_t n = 0;
        while ((c = get()) >= 0 && c != '\n')
            if (++n > 65536)
                throw std::runtime_error("ASCII STL header too long");
    }
    void require(const char* text) {
        if (next() != text)
            throw std::runtime_error(std::string("Invalid ASCII STL: expected ") + text);
    }
    double number() {
        auto s = next();
        double v = 0;
        const char* b = s.data();
        if (!s.empty() && s[0] == '+')
            ++b;
        auto r = std::from_chars(b, s.data() + s.size(), v);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || !std::isfinite(v))
            throw std::runtime_error("Invalid/non-finite STL coordinate");
        return v;
    }
};
using Triangle = std::array<glm::dvec3, 3>;
bool degenerate(const Triangle& t) {
    auto n = glm::cross(t[1] - t[0], t[2] - t[0]);
    return n.x == 0 && n.y == 0 && n.z == 0;
}
Metadata parse(const std::filesystem::path& path, const std::function<void(const Triangle&)>& consume,
               Cancel cancel, Progress progress) {
    FileReader f(path, cancel);
    Metadata m;
    m.sourceBytes = f.size();
    std::array<unsigned char, 84> header{};
    uint64_t expected = 0;
    if (f.size() >= 84) {
        exact(f, header.data(), 84);
        expected = 84ull + 50ull * u32(header.data() + 80);
        m.binary = expected <= f.size();
    }
    f.seek(0);
    m.minimum = glm::dvec3(std::numeric_limits<double>::infinity());
    m.maximum = -m.minimum;
    auto accept = [&](const Triangle& t) {
        for (auto v : t) {
            for (int i = 0; i < 3; ++i)
                if (!std::isfinite(v[i]) || std::abs(v[i]) > 1e100)
                    throw std::runtime_error("Non-finite or unsupported coordinate magnitude");
            m.minimum = glm::min(m.minimum, v);
            m.maximum = glm::max(m.maximum, v);
        }
        ++m.triangles;
        if (degenerate(t))
            ++m.degenerate;
        else if (consume)
            consume(t);
    };
    if (m.binary) {
        f.seek(84);
        uint64_t count = u32(header.data() + 80);
        m.trailingBytes = expected < f.size();
        std::vector<unsigned char> bytes(50 * 16384);
        for (uint64_t first = 0; first < count;) {
            if (cancel && cancel())
                throw std::runtime_error("Cancelled");
            auto n = size_t(std::min<uint64_t>(16384, count - first));
            exact(f, bytes.data(), n * 50);
            for (size_t i = 0; i < n; ++i) {
                Triangle t;
                for (int v = 0; v < 3; ++v)
                    for (int a = 0; a < 3; ++a)
                        t[v][a] = std::bit_cast<float>(u32(bytes.data() + i * 50 + 12 + (v * 3 + a) * 4));
                accept(t);
            }
            first += n;
            if (progress)
                progress(float(double(first) / std::max<uint64_t>(count, 1)));
        }
    } else {
        Tokens tok{f};
        tok.require("solid");
        tok.line();
        bool ended = false;
        while (true) {
            auto word = tok.next();
            if (word.empty()) {
                if (!ended)
                    throw std::runtime_error("Missing endsolid");
                break;
            }
            if (word == "endsolid") {
                tok.line();
                ended = true;
                continue;
            }
            if (word == "solid" && ended) {
                tok.line();
                ended = false;
                continue;
            }
            if (ended || word != "facet")
                throw std::runtime_error("Invalid ASCII STL facet");
            tok.require("normal");
            for (int i = 0; i < 3; ++i) {
                auto normal = tok.next();
                if (normal.empty())
                    throw std::runtime_error("Missing facet normal");
            }
            tok.require("outer");
            tok.require("loop");
            Triangle t;
            for (auto& v : t) {
                tok.require("vertex");
                for (int a = 0; a < 3; ++a)
                    v[a] = tok.number();
            }
            tok.require("endloop");
            tok.require("endfacet");
            accept(t);
            if ((m.triangles & 4095) == 0) {
                if (cancel && cancel())
                    throw std::runtime_error("Cancelled");
                if (progress)
                    progress(0);
            }
        }
    }
    if (m.triangles == 0 || m.triangles == m.degenerate)
        throw std::runtime_error("STL has no renderable triangles");
    if (progress)
        progress(1);
    return m;
}
} // namespace
Metadata inspectStl(const std::filesystem::path& p, Cancel c, Progress progress) {
    return parse(p, {}, std::move(c), std::move(progress));
}
void streamStl(const std::filesystem::path& p, const Metadata& m, const Chunk& callback, Cancel c,
               Progress progress, size_t chunkSize) {
    if (!chunkSize || chunkSize > 1000000)
        throw std::runtime_error("Invalid streaming chunk size");
    std::vector<glm::vec3> vertices;
    vertices.reserve(chunkSize * 3);
    auto center = m.center();
    double scale = m.scale();
    auto actual = parse(
        p,
        [&](const Triangle& t) {
            for (auto v : t)
                vertices.emplace_back((v - center) / scale);
            if (vertices.size() == chunkSize * 3) {
                callback(vertices);
                vertices.clear();
            }
        },
        std::move(c), std::move(progress));
    if (actual.triangles != m.triangles || actual.minimum != m.minimum || actual.maximum != m.maximum ||
        actual.sourceBytes != m.sourceBytes)
        throw std::runtime_error("File changed while loading");
    if (!vertices.empty())
        callback(vertices);
}
} // namespace si
