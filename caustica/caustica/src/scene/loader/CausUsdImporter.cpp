#include <scene/loader/CausUsdImporter.h>

#include <scene/SceneImport.h>
#include <scene/SceneEcs.h>
#include <scene/SceneObjects.h>
#include <scene/SceneTypes.h>
#include <scene/SceneAnimation.h>
#include <scene/SceneAnimationAccess.h>
#include <animation/KeyframeAnimation.h>
#include <assets/loader/TextureLoader.h>
#include <core/log.h>
#include <math/math.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <type_traits>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace caustica
{
namespace
{
constexpr char kMagic[8] = { 'C', 'A', 'U', 'S', 'U', 'S', 'D', '\0' };
constexpr uint32_t kVersion = 2;
constexpr uint32_t kMeshFlagXformAnim = 1u << 0;
constexpr uint32_t kMeshFlagPointAnim = 1u << 1;

class BinaryReader
{
public:
    explicit BinaryReader(std::vector<uint8_t> data)
        : m_data(std::move(data))
    {
    }

    [[nodiscard]] size_t remaining() const { return m_data.size() - m_offset; }

    template<typename T>
    T read()
    {
        static_assert(std::is_trivially_copyable_v<T>);
        if (m_offset + sizeof(T) > m_data.size())
            throw std::runtime_error("caususd: unexpected end of file");
        T value{};
        std::memcpy(&value, m_data.data() + m_offset, sizeof(T));
        m_offset += sizeof(T);
        return value;
    }

    std::string readString()
    {
        const uint32_t len = read<uint32_t>();
        if (m_offset + len > m_data.size())
            throw std::runtime_error("caususd: truncated string");
        std::string s(reinterpret_cast<const char*>(m_data.data() + m_offset), len);
        m_offset += len;
        return s;
    }

    void readFloats(std::vector<float>& out, size_t count)
    {
        const size_t bytes = count * sizeof(float);
        if (m_offset + bytes > m_data.size())
            throw std::runtime_error("caususd: truncated float array");
        out.resize(count);
        std::memcpy(out.data(), m_data.data() + m_offset, bytes);
        m_offset += bytes;
    }

    void readUints(std::vector<uint32_t>& out, size_t count)
    {
        const size_t bytes = count * sizeof(uint32_t);
        if (m_offset + bytes > m_data.size())
            throw std::runtime_error("caususd: truncated uint array");
        out.resize(count);
        std::memcpy(out.data(), m_data.data() + m_offset, bytes);
        m_offset += bytes;
    }

private:
    std::vector<uint8_t> m_data;
    size_t m_offset = 0;
};

std::vector<uint8_t> ReadEntireFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("failed to open " + path.string());
    const std::streamoff size = in.tellg();
    if (size < 0)
        throw std::runtime_error("failed to size " + path.string());
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size))
        throw std::runtime_error("failed to read " + path.string());
    return data;
}

bool IsCurrentCacheVersion(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    char magic[8] = {};
    uint32_t version = 0;
    in.read(magic, sizeof(magic));
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    return in && std::memcmp(magic, kMagic, sizeof(kMagic)) == 0 && version == kVersion;
}

ecs::Entity EnsurePathEntity(
    scene::SceneEntityWorld& world,
    ecs::Entity root,
    const std::string& usdPath,
    std::unordered_map<std::string, ecs::Entity>& pathMap)
{
    if (usdPath.empty() || usdPath == "/")
        return root;

    auto found = pathMap.find(usdPath);
    if (found != pathMap.end())
        return found->second;

    // Build parents first: /World/Robot/base_link -> /World, /World/Robot
    std::string parentPath;
    const size_t slash = usdPath.find_last_of('/');
    if (slash != std::string::npos && slash > 0)
        parentPath = usdPath.substr(0, slash);

    ecs::Entity parent = root;
    if (!parentPath.empty())
        parent = EnsurePathEntity(world, root, parentPath, pathMap);

    std::string leaf = (slash == std::string::npos) ? usdPath : usdPath.substr(slash + 1);
    if (leaf.empty())
        leaf = usdPath;

    ecs::Entity entity = world.createEntity(leaf, parent);
    pathMap[usdPath] = entity;
    return entity;
}

void ApplyRestTransform(
    scene::SceneEntityWorld& world,
    ecs::Entity entity,
    const float* trs10)
{
    const dm::double3 translation(trs10[0], trs10[1], trs10[2]);
    const dm::dquat rotation = dm::dquat::fromXYZW(dm::double4(trs10[3], trs10[4], trs10[5], trs10[6]));
    const dm::double3 scaling(trs10[7], trs10[8], trs10[9]);
    world.setLocalTransform(entity, &translation, &rotation, &scaling);
}

void AppendTransformChannels(
    scene::AnimationComponent& anim,
    ecs::Entity target,
    const std::vector<float>& times,
    const std::vector<float>& trs)
{
    if (times.size() < 2 || trs.size() < times.size() * 10)
        return;

    auto translation = std::make_shared<animation::Sampler>();
    auto rotation = std::make_shared<animation::Sampler>();
    auto scaling = std::make_shared<animation::Sampler>();
    translation->SetInterpolationMode(animation::InterpolationMode::Step);
    rotation->SetInterpolationMode(animation::InterpolationMode::Step);
    scaling->SetInterpolationMode(animation::InterpolationMode::Step);

    for (size_t i = 0; i < times.size(); ++i)
    {
        const float* k = trs.data() + i * 10;
        animation::Keyframe tKf;
        tKf.time = times[i];
        tKf.value = dm::float4(k[0], k[1], k[2], 0.f);
        translation->AddKeyframe(tKf);

        animation::Keyframe rKf;
        rKf.time = times[i];
        rKf.value = dm::float4(k[3], k[4], k[5], k[6]);
        rotation->AddKeyframe(rKf);

        animation::Keyframe sKf;
        sKf.time = times[i];
        sKf.value = dm::float4(k[7], k[8], k[9], 0.f);
        scaling->AddKeyframe(sKf);
    }

    scene::addAnimationChannel(anim, scene::AnimationChannelData{ translation, target, nullptr, AnimationAttribute::Translation, {} });
    scene::addAnimationChannel(anim, scene::AnimationChannelData{ rotation, target, nullptr, AnimationAttribute::Rotation, {} });
    scene::addAnimationChannel(anim, scene::AnimationChannelData{ scaling, target, nullptr, AnimationAttribute::Scaling, {} });
}

std::shared_ptr<MeshInfo> BuildMesh(
    SceneTypeFactory& factory,
    const std::string& name,
    const dm::float3& color,
    const std::vector<float>& positions,
    const std::vector<float>& normals,
    const std::vector<uint32_t>& indices,
    bool keepDeformationIndices)
{
    auto mesh = factory.createMesh();
    mesh->name = name;
    mesh->buffers = std::make_shared<BufferGroup>();
    auto& buffers = *mesh->buffers;

    const size_t vertexCount = positions.size() / 3;
    buffers.positionData.resize(vertexCount);
    buffers.normalData.resize(vertexCount);
    buffers.tangentData.resize(vertexCount);
    buffers.texcoord1Data.resize(vertexCount, dm::float2(0.f));
    buffers.indexData = indices;

    mesh->objectSpaceBounds = dm::box3::empty();
    for (size_t i = 0; i < vertexCount; ++i)
    {
        const dm::float3 p(positions[i * 3 + 0], positions[i * 3 + 1], positions[i * 3 + 2]);
        const dm::float3 n(normals[i * 3 + 0], normals[i * 3 + 1], normals[i * 3 + 2]);
        buffers.positionData[i] = p;
        buffers.normalData[i] = dm::vectorToSnorm8(n);
        buffers.tangentData[i] = dm::vectorToSnorm8(dm::float4(1.f, 0.f, 0.f, 1.f));
        mesh->objectSpaceBounds |= p;
        if (keepDeformationIndices)
            mesh->DeformationSourcePositionIndices.push_back(uint32_t(i));
    }

    auto material = factory.createMaterial();
    material->name = name + "_mat";
    material->baseOrDiffuseColor = color;
    material->domain = MaterialDomain::Opaque;

    auto geometry = factory.createMeshGeometry();
    geometry->material = material;
    geometry->objectSpaceBounds = mesh->objectSpaceBounds;
    geometry->indexOffsetInMesh = 0;
    geometry->vertexOffsetInMesh = 0;
    geometry->numIndices = uint32_t(indices.size());
    geometry->numVertices = uint32_t(vertexCount);
    mesh->geometries.push_back(geometry);

    mesh->totalVertices = uint32_t(vertexCount);
    mesh->totalIndices = uint32_t(indices.size());
    mesh->vertexOffset = 0;
    mesh->indexOffset = 0;
    return mesh;
}

std::filesystem::path FindBakeScript()
{
    // Prefer repo-relative tools/ next to common cwd layouts.
    const std::filesystem::path candidates[] = {
        std::filesystem::path("tools") / "usd_bake_caustica.py",
        std::filesystem::path("..") / "tools" / "usd_bake_caustica.py",
        std::filesystem::path("../..") / "tools" / "usd_bake_caustica.py",
        std::filesystem::path("../../..") / "tools" / "usd_bake_caustica.py",
    };
    for (const auto& c : candidates)
    {
        std::error_code ec;
        if (std::filesystem::exists(c, ec))
            return std::filesystem::absolute(c);
    }

#if defined(_WIN32)
    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, modulePath, MAX_PATH) > 0)
    {
        std::filesystem::path exe = std::filesystem::path(modulePath).parent_path();
        const std::filesystem::path nearExe[] = {
            exe / "tools" / "usd_bake_caustica.py",
            exe / ".." / "tools" / "usd_bake_caustica.py",
            exe / ".." / ".." / "tools" / "usd_bake_caustica.py",
            exe / ".." / ".." / ".." / "tools" / "usd_bake_caustica.py",
        };
        for (const auto& c : nearExe)
        {
            std::error_code ec;
            if (std::filesystem::exists(c, ec))
                return std::filesystem::weakly_canonical(c);
        }
    }
#endif
    return {};
}

bool RunPythonBake(const std::filesystem::path& usdPath, const std::filesystem::path& cachePath, std::string* errorMessage)
{
    const std::filesystem::path script = FindBakeScript();
    if (script.empty())
    {
        if (errorMessage)
            *errorMessage = "cannot find tools/usd_bake_caustica.py";
        return false;
    }

    const char* python = std::getenv("CAUSTICA_PYTHON");
    if (!python || !*python)
        python = "python";

    std::ostringstream cmd;
    cmd << '"' << python << "\" "
        << '"' << script.string() << "\" "
        << '"' << usdPath.string() << "\" "
        << "-o \"" << cachePath.string() << '"';

    caustica::info("Baking USD cache: %s", cmd.str().c_str());
    const int rc = std::system(cmd.str().c_str());
    if (rc != 0)
    {
        if (errorMessage)
            *errorMessage = "usd bake failed with exit code " + std::to_string(rc);
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(cachePath, ec))
    {
        if (errorMessage)
            *errorMessage = "bake finished but cache file was not created";
        return false;
    }
    return true;
}

} // namespace

CausUsdImporter::CausUsdImporter(std::shared_ptr<SceneTypeFactory> sceneTypeFactory)
    : m_SceneTypeFactory(std::move(sceneTypeFactory))
{
}

using scene::AnimationComponent;
using scene::GeometrySequenceComponent;
using scene::PerspectiveCameraData;
using scene::CameraComponent;

std::filesystem::path CausUsdImporter::resolveCachePath(const std::filesystem::path& usdOrCachePath)
{
    std::string ext = usdOrCachePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".caususd")
        return usdOrCachePath;
    std::filesystem::path cache = usdOrCachePath;
    cache.replace_extension(".caususd");
    return cache;
}

bool CausUsdImporter::ensureCache(
    const std::filesystem::path& usdPath,
    std::filesystem::path& outCachePath,
    std::string* errorMessage)
{
    outCachePath = resolveCachePath(usdPath);
    std::string ext = usdPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".caususd")
        return std::filesystem::exists(outCachePath);

    std::error_code ec;
    if (std::filesystem::exists(outCachePath, ec))
    {
        const auto usdTime = std::filesystem::last_write_time(usdPath, ec);
        const auto cacheTime = std::filesystem::last_write_time(outCachePath, ec);
        if (!ec && cacheTime >= usdTime && IsCurrentCacheVersion(outCachePath))
            return true;
        caustica::info("USD cache is stale or uses an old format, rebaking: %s", outCachePath.string().c_str());
    }

    return RunPythonBake(usdPath, outCachePath, errorMessage);
}

bool CausUsdImporter::load(
    const std::filesystem::path& fileName,
    TextureLoader& /*textureCache*/,
    SceneLoadingStats& /*stats*/,
    ThreadPool* /*threadPool*/,
    SceneImportResult& result,
    const std::filesystem::path& /*sceneDirectory*/) const
{
    try
    {
        std::filesystem::path cachePath = fileName;
        std::string ext = fileName.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == ".usd" || ext == ".usda" || ext == ".usdc")
        {
            std::string error;
            if (!ensureCache(fileName, cachePath, &error))
            {
                caustica::error("USD load failed for '%s': %s", fileName.string().c_str(), error.c_str());
                return false;
            }
        }

        BinaryReader reader(ReadEntireFile(cachePath));
        char magic[8] = {};
        for (char& c : magic)
            c = reader.read<char>();
        if (std::memcmp(magic, kMagic, 8) != 0)
        {
            caustica::error("Not a .caususd file: %s", cachePath.string().c_str());
            return false;
        }

        const uint32_t version = reader.read<uint32_t>();
        if (version != kVersion)
        {
            caustica::error("Unsupported .caususd version %u in %s", version, cachePath.string().c_str());
            return false;
        }

        (void)reader.read<uint32_t>(); // flags
        const float fps = reader.read<float>();
        const float startTime = reader.read<float>();
        const float endTime = reader.read<float>();
        const uint32_t meshCount = reader.read<uint32_t>();
        const uint32_t cameraCount = reader.read<uint32_t>();
        (void)fps;
        (void)startTime;

        result.entityWorld = std::make_shared<scene::SceneEntityWorld>();
        scene::SceneEntityWorld& world = *result.entityWorld;
        ecs::Entity root = world.createEntity(cachePath.stem().string());
        result.rootEntity = root;

        std::unordered_map<std::string, ecs::Entity> pathMap;
        pathMap["/"] = root;
        pathMap[""] = root;

        AnimationComponent animation;
        animation.duration = endTime;

        for (uint32_t mi = 0; mi < meshCount; ++mi)
        {
            const std::string primPath = reader.readString();
            const uint32_t meshFlags = reader.read<uint32_t>();
            const float cr = reader.read<float>();
            const float cg = reader.read<float>();
            const float cb = reader.read<float>();
            const uint32_t vertexCount = reader.read<uint32_t>();
            const uint32_t indexCount = reader.read<uint32_t>();

            std::vector<float> positions;
            std::vector<float> normals;
            std::vector<uint32_t> indices;
            reader.readFloats(positions, size_t(vertexCount) * 3);
            reader.readFloats(normals, size_t(vertexCount) * 3);
            reader.readUints(indices, indexCount);

            const uint32_t xformKeyCount = reader.read<uint32_t>();
            std::vector<float> xformTimes;
            std::vector<float> xformTrs;
            reader.readFloats(xformTimes, xformKeyCount);
            reader.readFloats(xformTrs, size_t(xformKeyCount) * 10);

            std::vector<float> pointTimes;
            std::vector<float> pointFrames;
            if (meshFlags & kMeshFlagPointAnim)
            {
                const uint32_t frameCount = reader.read<uint32_t>();
                reader.readFloats(pointTimes, frameCount);
                reader.readFloats(pointFrames, size_t(frameCount) * size_t(vertexCount) * 3);
            }

            if (vertexCount == 0 || indexCount < 3)
                continue;

            ecs::Entity entity = EnsurePathEntity(world, root, primPath, pathMap);
            const bool deforming = (meshFlags & kMeshFlagPointAnim) != 0;
            auto mesh = BuildMesh(
                *m_SceneTypeFactory,
                primPath,
                dm::float3(cr, cg, cb),
                positions,
                normals,
                indices,
                deforming);
            world.setMeshInstance(entity, mesh);

            if (!xformTrs.empty())
                ApplyRestTransform(world, entity, xformTrs.data());

            const bool animateXform = (meshFlags & kMeshFlagXformAnim) != 0 || xformKeyCount > 1;
            if (animateXform)
                AppendTransformChannels(animation, entity, xformTimes, xformTrs);

            if (deforming)
            {
                scene::GeometrySequenceComponent sequence;
                sequence.mesh = mesh;
                sequence.vertexCount = vertexCount;
                sequence.timesSeconds = std::move(pointTimes);
                sequence.positions = std::move(pointFrames);
                sequence.recomputeNormals = true;
                if (!sequence.timesSeconds.empty())
                    animation.duration = std::max(animation.duration, sequence.timesSeconds.back());
                world.world().emplace<scene::GeometrySequenceComponent>(entity, std::move(sequence));
            }
        }

        for (uint32_t ci = 0; ci < cameraCount; ++ci)
        {
            const std::string primPath = reader.readString();
            const float vfov = reader.read<float>();
            const float zNear = reader.read<float>();
            const float zFar = reader.read<float>();
            (void)reader.read<float>(); // focalLength
            const uint32_t keyCount = reader.read<uint32_t>();
            std::vector<float> times;
            std::vector<float> trs;
            reader.readFloats(times, keyCount);
            reader.readFloats(trs, size_t(keyCount) * 10);

            ecs::Entity entity = EnsurePathEntity(world, root, primPath, pathMap);
            scene::PerspectiveCameraData camData;
            camData.verticalFov = vfov;
            // Many USD exports use a meter-scale near plane of 1.0+, which clips the whole robot.
            camData.zNear = (zNear > 0.2f) ? 0.05f : std::max(zNear, 0.01f);
            if (zFar > camData.zNear)
                camData.zFar = zFar;
            scene::CameraComponent camera;
            camera.data = camData;
            world.setCamera(entity, std::move(camera));

            if (!trs.empty())
                ApplyRestTransform(world, entity, trs.data());
            if (keyCount > 1)
                AppendTransformChannels(animation, entity, times, trs);
        }

        if (!animation.channels.empty())
        {
            scene::recalculateAnimationDuration(animation);
            ecs::Entity animEntity = world.createEntity("UsdAnimation", root);
            world.setAnimation(animEntity, std::move(animation));
        }

        world.rebuildPathsFromRoot();
        caustica::info(
            "Loaded .caususd '%s' (%u meshes, %u cameras)",
            cachePath.string().c_str(),
            meshCount,
            cameraCount);
        return true;
    }
    catch (const std::exception& ex)
    {
        caustica::error("CausUsdImporter failed: %s", ex.what());
        return false;
    }
}

} // namespace caustica
