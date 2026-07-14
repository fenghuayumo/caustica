#include <scene/loader/StlLoader.h>

#include <core/log.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace caustica
{
namespace
{
    dm::float3 NormalizeOrFallback(dm::float3 value, dm::float3 fallback)
    {
        const float len = dm::length(value);
        return len > 1e-20f ? value / len : fallback;
    }

    bool LooksLikeAsciiStl(const std::vector<uint8_t>& data)
    {
        if (data.size() < 15)
            return false;

        size_t i = 0;
        while (i < data.size() && std::isspace(static_cast<unsigned char>(data[i])))
            ++i;

        const char* solid = "solid";
        for (size_t c = 0; c < 5; ++c)
        {
            if (i + c >= data.size() || std::tolower(static_cast<unsigned char>(data[i + c])) != solid[c])
                return false;
        }

        // Many binary STL files incorrectly start with "solid". Prefer ASCII only when facet keywords exist.
        const size_t probe = std::min<size_t>(data.size(), 4096);
        std::string head(reinterpret_cast<const char*>(data.data()), probe);
        std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return head.find("facet") != std::string::npos || head.find("endsolid") != std::string::npos;
    }

    bool LoadAsciiStl(const std::vector<uint8_t>& data, StlMeshData& outMesh)
    {
        std::string text(reinterpret_cast<const char*>(data.data()), data.size());
        std::istringstream stream(text);
        std::string token;
        dm::float3 currentNormal(0.f, 1.f, 0.f);
        std::vector<dm::float3> faceVertices;
        faceVertices.reserve(3);

        while (stream >> token)
        {
            std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            if (token == "facet")
            {
                std::string normalKeyword;
                stream >> normalKeyword;
                float nx = 0.f, ny = 0.f, nz = 0.f;
                stream >> nx >> ny >> nz;
                currentNormal = NormalizeOrFallback(dm::float3(nx, ny, nz), dm::float3(0.f, 1.f, 0.f));
                faceVertices.clear();
            }
            else if (token == "vertex")
            {
                float x = 0.f, y = 0.f, z = 0.f;
                stream >> x >> y >> z;
                faceVertices.emplace_back(x, y, z);
            }
            else if (token == "endfacet")
            {
                if (faceVertices.size() < 3)
                    continue;

                // Fan-triangulate in case of malformed polygons with >3 vertices.
                for (size_t i = 1; i + 1 < faceVertices.size(); ++i)
                {
                    const dm::float3& p0 = faceVertices[0];
                    const dm::float3& p1 = faceVertices[i];
                    const dm::float3& p2 = faceVertices[i + 1];
                    dm::float3 normal = currentNormal;
                    if (dm::length(normal) < 1e-20f)
                        normal = NormalizeOrFallback(dm::cross(p1 - p0, p2 - p0), dm::float3(0.f, 1.f, 0.f));

                    const uint32_t base = static_cast<uint32_t>(outMesh.positions.size());
                    outMesh.positions.push_back(p0);
                    outMesh.positions.push_back(p1);
                    outMesh.positions.push_back(p2);
                    outMesh.normals.push_back(normal);
                    outMesh.normals.push_back(normal);
                    outMesh.normals.push_back(normal);
                    outMesh.indices.push_back(base + 0);
                    outMesh.indices.push_back(base + 1);
                    outMesh.indices.push_back(base + 2);
                    outMesh.bounds |= p0;
                    outMesh.bounds |= p1;
                    outMesh.bounds |= p2;
                }
                faceVertices.clear();
            }
        }

        return !outMesh.empty();
    }

    bool LoadBinaryStl(const std::vector<uint8_t>& data, StlMeshData& outMesh)
    {
        if (data.size() < 84)
            return false;

        uint32_t triangleCount = 0;
        std::memcpy(&triangleCount, data.data() + 80, sizeof(uint32_t));
        const uint64_t expectedSize = 84ull + uint64_t(triangleCount) * 50ull;
        if (expectedSize > data.size())
        {
            // Tolerate trailing bytes, but reject truncated files.
            if (84ull + uint64_t(triangleCount) * 50ull > data.size())
                return false;
        }

        outMesh.positions.reserve(size_t(triangleCount) * 3);
        outMesh.normals.reserve(size_t(triangleCount) * 3);
        outMesh.indices.reserve(size_t(triangleCount) * 3);

        size_t offset = 84;
        for (uint32_t t = 0; t < triangleCount; ++t)
        {
            if (offset + 50 > data.size())
                break;

            float values[12];
            std::memcpy(values, data.data() + offset, sizeof(values));
            offset += 50; // 12 floats + 2-byte attribute

            const dm::float3 fileNormal(values[0], values[1], values[2]);
            const dm::float3 p0(values[3], values[4], values[5]);
            const dm::float3 p1(values[6], values[7], values[8]);
            const dm::float3 p2(values[9], values[10], values[11]);
            const dm::float3 normal = dm::length(fileNormal) > 1e-20f
                ? NormalizeOrFallback(fileNormal, dm::float3(0.f, 1.f, 0.f))
                : NormalizeOrFallback(dm::cross(p1 - p0, p2 - p0), dm::float3(0.f, 1.f, 0.f));

            const uint32_t base = static_cast<uint32_t>(outMesh.positions.size());
            outMesh.positions.push_back(p0);
            outMesh.positions.push_back(p1);
            outMesh.positions.push_back(p2);
            outMesh.normals.push_back(normal);
            outMesh.normals.push_back(normal);
            outMesh.normals.push_back(normal);
            outMesh.indices.push_back(base + 0);
            outMesh.indices.push_back(base + 1);
            outMesh.indices.push_back(base + 2);
            outMesh.bounds |= p0;
            outMesh.bounds |= p1;
            outMesh.bounds |= p2;
        }

        return !outMesh.empty();
    }
} // namespace

bool loadStlFile(const std::filesystem::path& filePath, StlMeshData& outMesh)
{
    outMesh = StlMeshData{};

    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file)
    {
        caustica::error("STL file could not be opened: '%s'", filePath.string().c_str());
        return false;
    }

    const std::streamoff size = file.tellg();
    if (size <= 0)
    {
        caustica::error("STL file is empty: '%s'", filePath.string().c_str());
        return false;
    }

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size))
    {
        caustica::error("STL file could not be read: '%s'", filePath.string().c_str());
        return false;
    }

    const bool ok = LooksLikeAsciiStl(data) ? LoadAsciiStl(data, outMesh) : LoadBinaryStl(data, outMesh);
    if (!ok)
    {
        caustica::error("STL file has no usable triangles: '%s'", filePath.string().c_str());
        return false;
    }

    return true;
}
} // namespace caustica
