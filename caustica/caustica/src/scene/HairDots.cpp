#include <scene/HairDots.h>

#include <algorithm>
#include <cmath>

namespace caustica::hair
{
namespace
{
constexpr uint32_t kVerticesPerSegment = 12;
constexpr float kMinRadius = 1e-6f;

void buildFrame(const dm::float3& forward, dm::float3& side, dm::float3& up)
{
    // Pick the least aligned cardinal axis to keep the cross product stable.
    const dm::float3 helper = std::abs(forward.x) < 0.8f
        ? dm::float3(1.f, 0.f, 0.f)
        : dm::float3(0.f, 1.f, 0.f);
    side = dm::normalize(dm::cross(forward, helper));
    up = dm::cross(forward, side);
}
}

void appendStaticDots(
    const std::vector<StrandSegment>& segments,
    BufferGroup& output,
    uint32_t firstOutputVertex)
{
    const size_t requiredVertices = size_t(firstOutputVertex) + segments.size() * kVerticesPerSegment;
    output.indexData.resize(requiredVertices);
    output.positionData.resize(requiredVertices);
    output.normalData.resize(requiredVertices);
    output.tangentData.resize(requiredVertices);
    output.texcoord1Data.resize(requiredVertices);
    output.radiusData.resize(requiredVertices);

    // Compensate the volume lost when a circular fiber is represented by two
    // orthogonal ribbons. This is the scale used by RTXCR static DOTS.
    const float pi = std::acos(-1.f);
    const float volumeCompensation = 1.f / (std::sin(pi * 0.25f) / (pi * 0.25f));

    for (size_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex)
    {
        const StrandSegment& segment = segments[segmentIndex];
        const dm::float3 delta = segment.vertices[1].position - segment.vertices[0].position;
        const float lengthSquared = dm::dot(delta, delta);
        const dm::float3 forward = lengthSquared > 1e-20f
            ? delta / std::sqrt(lengthSquared)
            : dm::float3(0.f, 1.f, 0.f);
        dm::float3 side;
        dm::float3 up;
        buildFrame(forward, side, up);
        const dm::float3 ribbonDirections[2] = { side, up };
        const float radii[2] = {
            std::max(segment.vertices[0].radius, kMinRadius) * volumeCompensation,
            std::max(segment.vertices[1].radius, kMinRadius) * volumeCompensation
        };

        for (uint32_t face = 0; face < 2; ++face)
        {
            const uint32_t base = firstOutputVertex
                + uint32_t(segmentIndex) * kVerticesPerSegment + face * 6;
            const dm::float3 ribbon = ribbonDirections[face];
            const dm::float3 positions[6] = {
                segment.vertices[0].position + ribbon * radii[0],
                segment.vertices[1].position - ribbon * radii[1],
                segment.vertices[1].position + ribbon * radii[1],
                segment.vertices[0].position + ribbon * radii[0],
                segment.vertices[0].position - ribbon * radii[0],
                segment.vertices[1].position - ribbon * radii[1]
            };
            const bool positiveSide[6] = { true, false, true, true, false, false };
            const uint32_t endpoint[6] = { 0, 1, 1, 0, 0, 1 };

            for (uint32_t vertex = 0; vertex < 6; ++vertex)
            {
                const uint32_t dst = base + vertex;
                output.indexData[dst] = dst - firstOutputVertex;
                output.positionData[dst] = positions[vertex];
                output.normalData[dst] = dm::vectorToSnorm8(positiveSide[vertex] ? ribbon : -ribbon);
                output.tangentData[dst] = dm::vectorToSnorm8(dm::float4(forward, 1.f));
                output.texcoord1Data[dst] = segment.vertices[endpoint[vertex]].texcoord;
                output.radiusData[dst] = radii[endpoint[vertex]];
            }
        }
    }
}
}
