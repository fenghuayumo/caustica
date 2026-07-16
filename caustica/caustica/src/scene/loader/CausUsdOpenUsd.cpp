#include "CausUsdOpenUsd.h"

#if CAUSTICA_WITH_OPENUSD

#include <scene/SceneImport.h>
#include <scene/SceneEcs.h>
#include <scene/SceneObjects.h>
#include <scene/SceneTypes.h>
#include <scene/SceneAnimation.h>
#include <scene/SceneAnimationAccess.h>
#include <animation/KeyframeAnimation.h>
#include <core/log.h>
#include <math/math.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/transform.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformable.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace caustica
{
namespace
{

constexpr uint32_t kMeshFlagXformAnim = 1u << 0;
constexpr uint32_t kMeshFlagPointAnim = 1u << 1;

void EnsureOpenUsdRuntimeConfigured()
{
    static bool s_done = false;
    if (s_done)
        return;
    s_done = true;

#if defined(_WIN32)
    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, modulePath, MAX_PATH) > 0)
    {
        const std::filesystem::path exeDir = std::filesystem::path(modulePath).parent_path();
        const std::filesystem::path pluginA = exeDir / "usd";
        const std::filesystem::path pluginB = exeDir / "plugin" / "usd";
        std::string pluginPath = pluginA.string();
        if (std::filesystem::exists(pluginB))
            pluginPath += ";" + pluginB.string();

        // Prefer plugins next to the executable (copied at build time).
        if (std::filesystem::exists(pluginA) || std::filesystem::exists(pluginB))
            _putenv_s("PXR_PLUGINPATH_NAME", pluginPath.c_str());

        // NVIDIA OpenUSD DLLs also need python311.dll on the search path.
        SetDllDirectoryA(exeDir.string().c_str());
    }
#endif

#if defined(CAUSTICA_USD_ROOT_STRING)
    // Fallback for developer machines where DLLs live in the SDK tree.
    if (const char* existing = std::getenv("PXR_PLUGINPATH_NAME"); !existing || !*existing)
    {
        const std::string root = CAUSTICA_USD_ROOT_STRING;
        const std::string pluginPath = root + "/lib/usd;" + root + "/plugin/usd";
#if defined(_WIN32)
        _putenv_s("PXR_PLUGINPATH_NAME", pluginPath.c_str());
        const std::string path = root + "\\lib;" + root + "\\bin;" + (std::getenv("PATH") ? std::getenv("PATH") : "");
        _putenv_s("PATH", path.c_str());
#else
        setenv("PXR_PLUGINPATH_NAME", pluginPath.c_str(), 0);
#endif
    }
#endif
}

GfVec3d ZUpToYUpPoint(const GfVec3d& p)
{
    return GfVec3d(p[0], p[2], -p[1]);
}

GfMatrix4d ZUpToYUpMatrix(const GfMatrix4d& m)
{
    // Row-vector convention (USD): p_y = p_z * B, M_y = Binv * M_z * B
    const GfMatrix4d b(1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1);
    return b.GetInverse() * m * b;
}

void ConvertPoint(const GfVec3f& p, bool convert, float out[3])
{
    GfVec3d v(p[0], p[1], p[2]);
    if (convert)
        v = ZUpToYUpPoint(v);
    out[0] = float(v[0]);
    out[1] = float(v[1]);
    out[2] = float(v[2]);
}

GfMatrix4d ConvertMatrix(const GfMatrix4d& m, bool convert)
{
    return convert ? ZUpToYUpMatrix(m) : m;
}

void DecomposeTrs(const GfMatrix4d& m, float out[10])
{
    GfTransform xf;
    xf.SetMatrix(m);
    const GfVec3d t = xf.GetTranslation();
    const GfQuatd q = xf.GetRotation().GetQuat();
    const GfVec3d imag = q.GetImaginary();
    const GfVec3d s = xf.GetScale();
    out[0] = float(t[0]);
    out[1] = float(t[1]);
    out[2] = float(t[2]);
    out[3] = float(imag[0]);
    out[4] = float(imag[1]);
    out[5] = float(imag[2]);
    out[6] = float(q.GetReal());
    out[7] = float(s[0]);
    out[8] = float(s[1]);
    out[9] = float(s[2]);
}

std::vector<uint32_t> Triangulate(const VtIntArray& counts, const VtIntArray& indices)
{
    std::vector<uint32_t> out;
    out.reserve(size_t(indices.size()));
    size_t cursor = 0;
    for (const int count : counts)
    {
        if (count < 3)
        {
            cursor += size_t(count);
            continue;
        }
        const int i0 = indices[cursor];
        for (int i = 1; i < count - 1; ++i)
        {
            out.push_back(uint32_t(i0));
            out.push_back(uint32_t(indices[cursor + size_t(i)]));
            out.push_back(uint32_t(indices[cursor + size_t(i) + 1]));
        }
        cursor += size_t(count);
    }
    return out;
}

void ComputeVertexNormals(
    const std::vector<float>& positions,
    const std::vector<uint32_t>& indices,
    std::vector<float>& normals)
{
    const size_t vertexCount = positions.size() / 3;
    normals.assign(vertexCount * 3, 0.f);
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
            continue;
        const float* p0 = positions.data() + size_t(i0) * 3;
        const float* p1 = positions.data() + size_t(i1) * 3;
        const float* p2 = positions.data() + size_t(i2) * 3;
        const float e0x = p1[0] - p0[0], e0y = p1[1] - p0[1], e0z = p1[2] - p0[2];
        const float e1x = p2[0] - p0[0], e1y = p2[1] - p0[1], e1z = p2[2] - p0[2];
        const float nx = e0y * e1z - e0z * e1y;
        const float ny = e0z * e1x - e0x * e1z;
        const float nz = e0x * e1y - e0y * e1x;
        for (uint32_t idx : { i0, i1, i2 })
        {
            normals[size_t(idx) * 3 + 0] += nx;
            normals[size_t(idx) * 3 + 1] += ny;
            normals[size_t(idx) * 3 + 2] += nz;
        }
    }
    for (size_t i = 0; i < vertexCount; ++i)
    {
        float* n = normals.data() + i * 3;
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1e-20f)
        {
            n[0] /= len;
            n[1] /= len;
            n[2] /= len;
        }
        else
        {
            n[0] = 0.f;
            n[1] = 1.f;
            n[2] = 0.f;
        }
    }
}

dm::float3 MeshDisplayColor(const UsdPrim& prim)
{
    UsdGeomPrimvar pv = UsdGeomGprim(prim).GetDisplayColorPrimvar();
    if (pv && pv.HasValue())
    {
        VtArray<GfVec3f> vals;
        if (pv.Get(&vals) && !vals.empty())
            return dm::float3(vals[0][0], vals[0][1], vals[0][2]);
    }
    return dm::float3(0.75f);
}

bool XformHasSamples(const UsdPrim& prim)
{
    UsdGeomXformable xf(prim);
    bool resets = false;
    for (const UsdGeomXformOp& op : xf.GetOrderedXformOps(&resets))
    {
        if (op.GetAttr().GetNumTimeSamples() > 0)
            return true;
    }
    return false;
}

void GatherXformKeys(
    const UsdPrim& prim,
    double fps,
    bool convert,
    std::vector<float>& timesSeconds,
    std::vector<float>& trs)
{
    timesSeconds.clear();
    trs.clear();

    UsdGeomXformable xf(prim);
    std::vector<double> times;
    bool resets = false;
    for (const UsdGeomXformOp& op : xf.GetOrderedXformOps(&resets))
    {
        std::vector<double> samples;
        op.GetAttr().GetTimeSamples(&samples);
        times.insert(times.end(), samples.begin(), samples.end());
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());

    if (times.empty())
    {
        float key[10] = {};
        DecomposeTrs(ConvertMatrix(xf.ComputeLocalToWorldTransform(UsdTimeCode::Default()), convert), key);
        timesSeconds.push_back(0.f);
        trs.insert(trs.end(), key, key + 10);
        return;
    }

    timesSeconds.reserve(times.size());
    trs.reserve(times.size() * 10);
    for (double tc : times)
    {
        float key[10] = {};
        DecomposeTrs(ConvertMatrix(xf.ComputeLocalToWorldTransform(UsdTimeCode(tc)), convert), key);
        // Keep quaternion hemisphere continuous.
        if (trs.size() >= 10)
        {
            const float* prev = trs.data() + trs.size() - 10;
            const float dot =
                prev[3] * key[3] + prev[4] * key[4] + prev[5] * key[5] + prev[6] * key[6];
            if (dot < 0.f)
            {
                key[3] = -key[3];
                key[4] = -key[4];
                key[5] = -key[5];
                key[6] = -key[6];
            }
        }
        timesSeconds.push_back(float(tc / fps));
        trs.insert(trs.end(), key, key + 10);
    }
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

void ApplyRestTransform(scene::SceneEntityWorld& world, ecs::Entity entity, const float* trs10)
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
    translation->setInterpolationMode(animation::InterpolationMode::Step);
    rotation->setInterpolationMode(animation::InterpolationMode::Step);
    scaling->setInterpolationMode(animation::InterpolationMode::Step);

    for (size_t i = 0; i < times.size(); ++i)
    {
        const float* k = trs.data() + i * 10;
        animation::Keyframe tKf;
        tKf.time = times[i];
        tKf.value = dm::float4(k[0], k[1], k[2], 0.f);
        translation->addKeyframe(tKf);

        animation::Keyframe rKf;
        rKf.time = times[i];
        rKf.value = dm::float4(k[3], k[4], k[5], k[6]);
        rotation->addKeyframe(rKf);

        animation::Keyframe sKf;
        sKf.time = times[i];
        sKf.value = dm::float4(k[7], k[8], k[9], 0.f);
        scaling->addKeyframe(sKf);
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

} // namespace

bool LoadSceneFromOpenUsd(
    const std::filesystem::path& usdPath,
    SceneTypeFactory& factory,
    SceneImportResult& result,
    std::string* errorMessage)
{
    EnsureOpenUsdRuntimeConfigured();

    const std::string pathStr = usdPath.string();
    UsdStageRefPtr stage = UsdStage::Open(pathStr);
    if (!stage)
    {
        if (errorMessage)
            *errorMessage = "UsdStage::Open failed for " + pathStr;
        return false;
    }

    double fps = stage->GetTimeCodesPerSecond();
    if (fps <= 0.0)
        fps = stage->GetFramesPerSecond();
    if (fps <= 0.0)
        fps = 24.0;

    const double startTc = stage->GetStartTimeCode();
    const double endTc = stage->GetEndTimeCode();
    const TfToken upAxis = UsdGeomGetStageUpAxis(stage);
    const bool convertZUp = (upAxis == UsdGeomTokens->z);

    std::vector<UsdPrim> meshes;
    std::vector<UsdPrim> cameras;
    for (const UsdPrim& prim : stage->Traverse())
    {
        if (prim.IsA<UsdGeomMesh>())
            meshes.push_back(prim);
        else if (prim.IsA<UsdGeomCamera>())
            cameras.push_back(prim);
    }

    caustica::info(
        "OpenUSD: '%s' upAxis=%s fps=%.3f time=[%.1f, %.1f] convertYUp=%d meshes=%zu cameras=%zu",
        pathStr.c_str(),
        upAxis.GetText(),
        fps,
        startTc,
        endTc,
        int(convertZUp),
        meshes.size(),
        cameras.size());

    result.entityWorld = std::make_shared<scene::SceneEntityWorld>();
    scene::SceneEntityWorld& world = *result.entityWorld;
    ecs::Entity root = world.createEntity(usdPath.stem().string());
    result.rootEntity = root;

    std::unordered_map<std::string, ecs::Entity> pathMap;
    pathMap["/"] = root;
    pathMap[""] = root;

    scene::AnimationComponent animation;
    animation.duration = float(endTc / fps);

    uint32_t loadedMeshes = 0;
    for (const UsdPrim& prim : meshes)
    {
        UsdGeomMesh mesh(prim);
        UsdAttribute ptsAttr = mesh.GetPointsAttr();
        UsdAttribute countsAttr = mesh.GetFaceVertexCountsAttr();
        UsdAttribute indicesAttr = mesh.GetFaceVertexIndicesAttr();

        std::vector<double> pointTimes;
        if (ptsAttr)
            ptsAttr.GetTimeSamples(&pointTimes);
        const bool hasPointAnim = pointTimes.size() > 1;
        const bool hasXformAnim = XformHasSamples(prim);

        const double sampleTc = !pointTimes.empty() ? pointTimes.front() : startTc;
        VtVec3fArray points;
        VtIntArray counts;
        VtIntArray indices;
        if (!ptsAttr || !ptsAttr.Get(&points, UsdTimeCode(sampleTc)) || points.empty())
            continue;
        if (!countsAttr || !countsAttr.Get(&counts) || counts.empty())
            continue;
        if (!indicesAttr || !indicesAttr.Get(&indices) || indices.empty())
            continue;

        std::vector<float> positions(points.size() * 3);
        for (size_t i = 0; i < points.size(); ++i)
            ConvertPoint(points[i], convertZUp, positions.data() + i * 3);

        std::vector<uint32_t> tri = Triangulate(counts, indices);
        if (tri.size() < 3)
            continue;

        std::vector<float> normals;
        ComputeVertexNormals(positions, tri, normals);

        const std::string primPath = prim.GetPath().GetString();
        ecs::Entity entity = EnsurePathEntity(world, root, primPath, pathMap);
        auto meshInfo = BuildMesh(
            factory,
            primPath,
            MeshDisplayColor(prim),
            positions,
            normals,
            tri,
            hasPointAnim);
        world.setMeshInstance(entity, meshInfo);

        std::vector<float> xformTimes;
        std::vector<float> xformTrs;
        GatherXformKeys(prim, fps, convertZUp, xformTimes, xformTrs);
        if (!xformTrs.empty())
            ApplyRestTransform(world, entity, xformTrs.data());

        const bool animateXform = hasXformAnim || xformTimes.size() > 1;
        if (animateXform)
            AppendTransformChannels(animation, entity, xformTimes, xformTrs);

        if (hasPointAnim)
        {
            scene::GeometrySequenceComponent sequence;
            sequence.mesh = meshInfo;
            sequence.vertexCount = uint32_t(points.size());
            sequence.recomputeNormals = true;
            sequence.timesSeconds.reserve(pointTimes.size());
            sequence.positions.resize(pointTimes.size() * points.size() * 3);

            for (size_t fi = 0; fi < pointTimes.size(); ++fi)
            {
                VtVec3fArray framePts;
                if (!ptsAttr.Get(&framePts, UsdTimeCode(pointTimes[fi])) || framePts.size() != points.size())
                {
                    if (errorMessage)
                    {
                        *errorMessage = primPath + ": point count changed at t="
                            + std::to_string(pointTimes[fi]);
                    }
                    return false;
                }
                sequence.timesSeconds.push_back(float(pointTimes[fi] / fps));
                float* dst = sequence.positions.data() + fi * points.size() * 3;
                for (size_t vi = 0; vi < framePts.size(); ++vi)
                    ConvertPoint(framePts[vi], convertZUp, dst + vi * 3);
            }

            if (!sequence.timesSeconds.empty())
                animation.duration = std::max(animation.duration, sequence.timesSeconds.back());
            world.world().emplace<scene::GeometrySequenceComponent>(entity, std::move(sequence));
        }

        (void)kMeshFlagXformAnim;
        (void)kMeshFlagPointAnim;
        ++loadedMeshes;
    }

    uint32_t loadedCameras = 0;
    for (const UsdPrim& prim : cameras)
    {
        UsdGeomCamera cam(prim);
        float focal = 18.5f;
        float hap = 20.955f;
        float vap = hap * 9.f / 16.f;
        GfVec2f clip(0.1f, 100000.f);

        float fTmp = 0.f;
        if (cam.GetFocalLengthAttr().Get(&fTmp))
            focal = fTmp;
        if (cam.GetHorizontalApertureAttr().Get(&fTmp))
            hap = fTmp;
        if (cam.GetVerticalApertureAttr().Get(&fTmp))
            vap = fTmp;
        else
            vap = hap * 9.f / 16.f;
        cam.GetClippingRangeAttr().Get(&clip);

        const float vfov = 2.f * std::atan((0.5f * vap) / std::max(focal, 1e-6f));

        const std::string primPath = prim.GetPath().GetString();
        ecs::Entity entity = EnsurePathEntity(world, root, primPath, pathMap);

        scene::PerspectiveCameraData camData;
        camData.verticalFov = vfov;
        camData.zNear = (clip[0] > 0.2f) ? 0.05f : std::max(clip[0], 0.01f);
        if (clip[1] > camData.zNear)
            camData.zFar = clip[1];
        scene::CameraComponent camera;
        camera.data = camData;
        world.setCamera(entity, std::move(camera));

        std::vector<float> times;
        std::vector<float> trs;
        GatherXformKeys(prim, fps, convertZUp, times, trs);
        if (!trs.empty())
            ApplyRestTransform(world, entity, trs.data());
        if (times.size() > 1)
            AppendTransformChannels(animation, entity, times, trs);
        ++loadedCameras;
    }

    if (!animation.channels.empty())
    {
        scene::recalculateAnimationDuration(animation);
        ecs::Entity animEntity = world.createEntity("UsdAnimation", root);
        world.setAnimation(animEntity, std::move(animation));
    }

    world.rebuildPathsFromRoot();
    caustica::info(
        "Loaded OpenUSD '%s' (%u meshes, %u cameras)",
        pathStr.c_str(),
        loadedMeshes,
        loadedCameras);
    return true;
}

} // namespace caustica

#endif // CAUSTICA_WITH_OPENUSD
