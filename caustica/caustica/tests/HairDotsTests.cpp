#include <scene/HairDots.h>

#include <cmath>
#include <cstdio>

namespace
{
bool expect(bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "Static DOTS test failed: %s\n", message);
    return false;
}
}

int main()
{
    caustica::hair::StrandSegment segment;
    segment.vertices[0].position = math::float3(0.f, 0.f, 0.f);
    segment.vertices[1].position = math::float3(0.f, 1.f, 0.f);
    segment.vertices[0].radius = 0.01f;
    segment.vertices[1].radius = 0.005f;
    segment.vertices[0].texcoord = math::float2(0.f, 0.f);
    segment.vertices[1].texcoord = math::float2(0.f, 1.f);

    caustica::BufferGroup output;
    caustica::hair::appendStaticDots({ segment }, output);

    bool passed = true;
    passed &= expect(output.indexData.size() == 12, "one segment must emit four triangles");
    passed &= expect(output.positionData.size() == 12, "DOTS positions were not emitted");
    passed &= expect(output.normalData.size() == 12, "DOTS normals were not emitted");
    passed &= expect(output.tangentData.size() == 12, "strand tangents were not emitted");
    passed &= expect(output.radiusData.size() == 12, "DOTS radii were not emitted");
    passed &= expect(output.indexData[0] == 0 && output.indexData[11] == 11,
        "DOTS indices must be geometry-local and sequential");
    passed &= expect(output.texcoord1Data[0].y == 0.f && output.texcoord1Data[2].y == 1.f,
        "strand endpoint UVs were not preserved");
    passed &= expect(output.radiusData[0] > output.radiusData[2],
        "tapered endpoint radii were not preserved");

    for (const math::float3& position : output.positionData)
        passed &= expect(std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z),
            "DOTS emitted a non-finite position");

    return passed ? 0 : 1;
}
