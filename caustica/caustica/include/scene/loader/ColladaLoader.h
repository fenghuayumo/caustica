#pragma once

#include <math/math.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace caustica
{
    class IFileSystem;

    // One profile_COMMON material authored in a COLLADA 1.4 effect.
    // `diffuse` / `specular` / `shininess` are the file values. `baseColor`,
    // `metalness`, and `roughness` are the metal-rough values used by the renderer.
    struct ColladaMaterialInfo
    {
        std::string id;
        std::string name;
        std::string shading;

        math::float3 diffuse = math::float3(0.8f);
        math::float3 specular = math::float3(0.f);
        math::float3 emission = math::float3(0.f);
        float shininess = 0.f;
        float opacity = 1.f;
        float indexOfRefraction = 1.f;

        math::float3 baseColor = math::float3(0.8f);
        float metalness = 0.f;
        float roughness = 1.f;
        bool doubleSided = false;

        std::filesystem::path diffuseTexture;
        std::filesystem::path emissiveTexture;
        std::filesystem::path normalTexture;
    };

    // One triangle set after node transforms, unit scale, and up-axis conversion.
    // Indices are local to `positions`. Coordinates are Z-up, in metres.
    struct ColladaPrimitive
    {
        std::string materialId;
        std::vector<math::float3> positions;
        std::vector<math::float3> normals;
        std::vector<math::float2> texcoords;
        std::vector<uint32_t> indices;
        math::box3 bounds = math::box3::empty();
    };

    struct ColladaMeshData
    {
        std::vector<ColladaPrimitive> primitives;
        std::unordered_map<std::string, ColladaMaterialInfo> materials;

        [[nodiscard]] bool empty() const { return primitives.empty(); }
    };

    // Loads a COLLADA 1.4 visual mesh (.dae): profile_COMMON materials and
    // triangles / polylist / polygons / tristrips / trifans.
    // Z_UP vertices are unchanged so they match a URDF link frame. Y_UP and X_UP
    // are rotated so +Z is up. The `<unit meter>` scale is applied.
    bool loadColladaFile(
        const std::filesystem::path& filePath,
        ColladaMeshData& outMesh,
        const std::shared_ptr<IFileSystem>& fs = nullptr);
}
