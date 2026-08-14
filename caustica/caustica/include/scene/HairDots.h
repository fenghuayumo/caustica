#pragma once

#include <scene/SceneTypes.h>

#include <cstdint>
#include <vector>

namespace caustica::hair
{
    struct StrandVertex
    {
        dm::float3 position = 0.f;
        float radius = 0.f;
        dm::float2 texcoord = 0.f;
    };

    struct StrandSegment
    {
        StrandVertex vertices[2];
    };

    // Expands independent strand segments into RTXCR's static Disjoint
    // Orthogonal Triangle Strips representation (four triangles per segment).
    void appendStaticDots(
        const std::vector<StrandSegment>& segments,
        BufferGroup& output,
        uint32_t firstOutputVertex = 0);
}
