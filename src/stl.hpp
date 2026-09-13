#pragma once
#include "platform.hpp"
#include <glm/glm.hpp>
#include <span>
#include <vector>
namespace si {
struct Metadata {
    glm::dvec3 minimum{0}, maximum{0};
    uint64_t triangles = 0, degenerate = 0, sourceBytes = 0;
    bool binary = false, trailingBytes = false;
    glm::dvec3 center() const {
        return minimum + (maximum - minimum) * 0.5;
    }
    double scale() const {
        auto d = maximum - minimum;
        return std::max({d.x, d.y, d.z, 1e-30});
    }
    uint64_t geometryBytes() const {
        return (triangles - degenerate) * 36;
    }
};
using Progress = std::function<void(float)>;
using Chunk = std::function<void(std::span<const glm::vec3>)>;
Metadata inspectStl(const std::filesystem::path&, Cancel = {}, Progress = {});
void streamStl(const std::filesystem::path&, const Metadata&, const Chunk&, Cancel = {}, Progress = {},
               size_t trianglesPerChunk = 65536);
} // namespace si
