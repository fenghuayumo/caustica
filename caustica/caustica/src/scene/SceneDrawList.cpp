#include <scene/SceneDrawList.h>
#include <scene/SceneRenderData.h>
#include <scene/SceneContent.h>
#include <scene/SceneTypes.h>
#include <scene/View.h>

#include <algorithm>

using namespace caustica::math;

namespace caustica::scene
{

namespace
{

bool CompareOpaqueDrawCommands(const DrawCommand& a, const DrawCommand& b)
{
    if (a.material != b.material)
        return a.material < b.material;
    if (a.meshId != b.meshId)
        return a.meshId.getValue() < b.meshId.getValue();
    if (a.meshEntity != b.meshEntity)
        return a.meshEntity < b.meshEntity;
    return a.instanceIndex < b.instanceIndex;
}

bool CompareTransparentDrawCommands(const DrawCommand& a, const DrawCommand& b)
{
    if (a.meshEntity == b.meshEntity)
        return a.cullMode > b.cullMode;
    return a.distanceToCamera > b.distanceToCamera;
}

void AppendOpaqueGeometryCommands(
    const MeshInstanceRenderProxy& proxy,
    const dm::frustum& viewFrustum,
    std::vector<DrawCommand>& out)
{
    if (!proxy.meshShared)
        return;

    const caustica::MeshInfo* mesh = proxy.meshShared.get();
    for (const auto& geometry : mesh->geometries)
    {
        const MaterialDomain domain = geometry->material->domain;
        if (domain != MaterialDomain::Opaque && domain != MaterialDomain::AlphaTested)
            continue;

        if (proxy.geometryCount > 1 && !proxy.hasSkinPrototype)
        {
            const dm::box3 geometryGlobalBoundingBox = geometry->objectSpaceBounds * proxy.transformFloat;
            if (!viewFrustum.intersectsWith(geometryGlobalBoundingBox))
                continue;
        }

        DrawCommand& item = out.emplace_back();
        item.meshEntity = proxy.entity;
        item.meshId = proxy.meshId;
        item.instanceIndex = proxy.instanceIndex;
        item.mesh = mesh;
        item.geometry = geometry.get();
        item.material = geometry->material.get();
        item.cullMode = item.material->doubleSided ? caustica::rhi::RasterCullMode::None : caustica::rhi::RasterCullMode::Back;
        item.distanceToCamera = 0.f;
    }
}

void AppendTransparentGeometryCommands(
    const MeshInstanceRenderProxy& proxy,
    const dm::frustum& viewFrustum,
    const float3& viewOrigin,
    const DrawListBuildOptions& options,
    std::vector<DrawCommand>& out)
{
    if (!proxy.meshShared)
        return;

    const caustica::MeshInfo* mesh = proxy.meshShared.get();
    for (const auto& geometry : mesh->geometries)
    {
        const auto& material = geometry->material;
        if (material->domain == MaterialDomain::Opaque || material->domain == MaterialDomain::AlphaTested)
            continue;

        dm::box3 geometryGlobalBoundingBox;
        if (proxy.geometryCount > 1 && proxy.hasSkinPrototype)
        {
            geometryGlobalBoundingBox = geometry->objectSpaceBounds * proxy.transformFloat;
            if (!viewFrustum.intersectsWith(geometryGlobalBoundingBox))
                continue;
        }
        else
        {
            geometryGlobalBoundingBox = proxy.globalBounds;
        }

        DrawCommand item{};
        item.meshEntity = proxy.entity;
        item.meshId = proxy.meshId;
        item.instanceIndex = proxy.instanceIndex;
        item.mesh = mesh;
        item.geometry = geometry.get();
        item.material = material.get();
        item.distanceToCamera = length(geometryGlobalBoundingBox.center() - viewOrigin);

        if (material->doubleSided)
        {
            if (options.drawDoubleSidedMaterialsSeparately)
            {
                item.cullMode = caustica::rhi::RasterCullMode::Front;
                out.push_back(item);
                item.cullMode = caustica::rhi::RasterCullMode::Back;
                out.push_back(item);
            }
            else
            {
                item.cullMode = caustica::rhi::RasterCullMode::None;
                out.push_back(item);
            }
        }
        else
        {
            item.cullMode = caustica::rhi::RasterCullMode::Back;
            out.push_back(item);
        }
    }
}

} // namespace

void buildOpaqueDrawList(
    const SceneRenderData& renderData,
    const caustica::IView& view,
    std::vector<DrawCommand>& out)
{
    out.clear();

    const dm::frustum viewFrustum = view.getViewFrustum();
    const auto relevantContentFlags = SceneContentFlags::OpaqueMeshes | SceneContentFlags::AlphaTestedMeshes;

    for (const MeshInstanceRenderProxy& proxy : renderData.meshInstances)
    {
        if ((proxy.leafContent & relevantContentFlags) == 0)
            continue;
        if (!viewFrustum.intersectsWith(proxy.globalBounds))
            continue;

        AppendOpaqueGeometryCommands(proxy, viewFrustum, out);
    }

    if (out.size() > 1)
        std::sort(out.begin(), out.end(), CompareOpaqueDrawCommands);
}

void buildTransparentDrawList(
    const SceneRenderData& renderData,
    const caustica::IView& view,
    std::vector<DrawCommand>& out,
    const DrawListBuildOptions& options)
{
    out.clear();

    const dm::frustum viewFrustum = view.getViewFrustum();
    const float3 viewOrigin = view.getViewOrigin();
    const auto relevantContentFlags = SceneContentFlags::BlendedMeshes;

    for (const MeshInstanceRenderProxy& proxy : renderData.meshInstances)
    {
        if ((proxy.leafContent & relevantContentFlags) == 0)
            continue;
        if (!viewFrustum.intersectsWith(proxy.globalBounds))
            continue;

        AppendTransparentGeometryCommands(proxy, viewFrustum, viewOrigin, options, out);
    }

    if (out.size() > 1)
        std::sort(out.begin(), out.end(), CompareTransparentDrawCommands);
}

void buildViewDrawLists(
    const SceneRenderData& renderData,
    const caustica::IView& view,
    ViewDrawLists& out,
    const DrawListBuildOptions& options)
{
    buildOpaqueDrawList(renderData, view, out.opaque);
    buildTransparentDrawList(renderData, view, out.transparent, options);
}

} // namespace caustica::scene
