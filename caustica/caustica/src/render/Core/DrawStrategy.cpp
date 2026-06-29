#include <render/Core/DrawStrategy.h>
#include <render/Core/GeometryPasses.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <scene/View.h>

using namespace caustica::math;
using namespace caustica;
using namespace caustica::render;

const DrawItem* PassthroughDrawStrategy::GetNextItem()
{
    if (m_Count > 0)
    {
        --m_Count;
        return m_Data++;
    }

    m_Data = nullptr;
    return nullptr;
}

void PassthroughDrawStrategy::SetData(const DrawItem* data, size_t count)
{
    m_Data = data;
    m_Count = count;
}

static int CompareDrawItemsOpaque(const DrawItem* a, const DrawItem* b)
{
    if (a->material != b->material)
        return a->material < b->material;

    if (a->buffers != b->buffers)
        return a->buffers < b->buffers;

    if (a->mesh != b->mesh)
        return a->mesh < b->mesh;

    return a->instance < b->instance;
}

void InstancedOpaqueDrawStrategy::PrepareForView(const caustica::Scene& scene, const caustica::IView& view)
{
    m_ViewFrustum = view.GetViewFrustum();
    m_Items.clear();
    m_ItemPtrs.clear();
    m_ReadPtr = 0;

    const auto relevantContentFlags = SceneContentFlags::OpaqueMeshes | SceneContentFlags::AlphaTestedMeshes;

    if (auto* entityWorld = scene.GetEntityWorld())
    {
        auto& world = entityWorld->world();
        world.each<scene::MeshInstanceComponent, scene::GlobalTransformComponent,
                   scene::BoundsComponent, scene::SceneContentComponent>(
            [&](ecs::Entity, scene::MeshInstanceComponent& mc,
                scene::GlobalTransformComponent& gtx, scene::BoundsComponent& bounds,
                scene::SceneContentComponent& content)
        {
            if ((content.leafContent & relevantContentFlags) == 0)
                return;

            if (!m_ViewFrustum.intersectsWith(bounds.globalBounds))
                return;

            auto meshInstance = dynamic_cast<MeshInstance*>(mc.instance.get());
            if (!meshInstance)
                return;

            const caustica::MeshInfo* mesh = meshInstance->GetMesh().get();

            for (const auto& geometry : mesh->geometries)
            {
                auto domain = geometry->material->domain;
                if (domain != MaterialDomain::Opaque && domain != MaterialDomain::AlphaTested)
                    continue;

                if (mesh->geometries.size() > 1 && !mesh->skinPrototype)
                {
                    dm::box3 geometryGlobalBoundingBox = geometry->objectSpaceBounds * gtx.transformFloat;
                    if (!m_ViewFrustum.intersectsWith(geometryGlobalBoundingBox))
                        continue;
                }

                DrawItem& item = m_Items.emplace_back();
                item.instance = meshInstance;
                item.mesh = mesh;
                item.geometry = geometry.get();
                item.material = geometry->material.get();
                item.buffers = item.mesh->buffers.get();
                item.cullMode = (item.material->doubleSided) ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;
                item.distanceToCamera = 0; // don't care
            }
        });
    }

    m_ItemPtrs.resize(m_Items.size());
    for (size_t i = 0; i < m_Items.size(); i++)
        m_ItemPtrs[i] = &m_Items[i];

    if (m_ItemPtrs.size() > 1)
    {
        std::sort(m_ItemPtrs.data(), m_ItemPtrs.data() + m_ItemPtrs.size(), CompareDrawItemsOpaque);
    }
}

const DrawItem* InstancedOpaqueDrawStrategy::GetNextItem()
{
    if (m_ReadPtr >= m_ItemPtrs.size())
        return nullptr;

    return m_ItemPtrs[m_ReadPtr++];
}


static int CompareDrawItemsTransparent(const DrawItem* a, const DrawItem* b)
{
    if (a->instance == b->instance)
        return a->cullMode > b->cullMode;

    return a->distanceToCamera > b->distanceToCamera;
}

void TransparentDrawStrategy::PrepareForView(const caustica::Scene& scene, const IView& view)
{
    m_ReadPtr = 0;

    m_InstancesToDraw.clear();
    m_InstancePtrsToDraw.clear();

    float3 viewOrigin = view.GetViewOrigin();
    auto viewFrustum = view.GetViewFrustum();

    const auto relevantContentFlags = SceneContentFlags::BlendedMeshes;

    if (auto* entityWorld = scene.GetEntityWorld())
    {
        auto& world = entityWorld->world();
        world.each<scene::MeshInstanceComponent, scene::GlobalTransformComponent,
                   scene::BoundsComponent, scene::SceneContentComponent>(
            [&](ecs::Entity, scene::MeshInstanceComponent& mc,
                scene::GlobalTransformComponent& gtx, scene::BoundsComponent& bounds,
                scene::SceneContentComponent& content)
        {
            if ((content.leafContent & relevantContentFlags) == 0)
                return;

            if (!viewFrustum.intersectsWith(bounds.globalBounds))
                return;

            auto meshInstance = dynamic_cast<MeshInstance*>(mc.instance.get());
            if (!meshInstance)
                return;

            const caustica::MeshInfo* mesh = meshInstance->GetMesh().get();
            for (const auto& geometry : mesh->geometries)
            {
                const auto& material = geometry->material;
                if (material->domain == MaterialDomain::Opaque || material->domain == MaterialDomain::AlphaTested)
                    continue;

                dm::box3 geometryGlobalBoundingBox;
                if (mesh->geometries.size() > 1 && mesh->skinPrototype.use_count() != 0)
                {
                    geometryGlobalBoundingBox = geometry->objectSpaceBounds * gtx.transformFloat;
                    if (!viewFrustum.intersectsWith(geometryGlobalBoundingBox))
                        continue;
                }
                else
                {
                    geometryGlobalBoundingBox = bounds.globalBounds;
                }

                DrawItem item{};
                item.instance = meshInstance;
                item.mesh = mesh;
                item.geometry = geometry.get();
                item.material = geometry->material.get();
                item.buffers = mesh->buffers.get();
                item.distanceToCamera = length(geometryGlobalBoundingBox.center() - viewOrigin);
                if (material->doubleSided)
                {
                    if (DrawDoubleSidedMaterialsSeparately)
                    {
                        item.cullMode = nvrhi::RasterCullMode::Front;
                        m_InstancesToDraw.push_back(item);
                        item.cullMode = nvrhi::RasterCullMode::Back;
                        m_InstancesToDraw.push_back(item);
                    }
                    else
                    {
                        item.cullMode = nvrhi::RasterCullMode::None;
                        m_InstancesToDraw.push_back(item);
                    }
                }
                else
                {
                    item.cullMode = nvrhi::RasterCullMode::Back;
                    m_InstancesToDraw.push_back(item);
                }
            }
        });
    }

    if (m_InstancesToDraw.empty())
        return;

    m_InstancePtrsToDraw.resize(m_InstancesToDraw.size());

    for (size_t i = 0; i < m_InstancesToDraw.size(); i++)
    {
        m_InstancePtrsToDraw[i] = &m_InstancesToDraw[i];
    }

    if (m_InstancePtrsToDraw.size() > 1)
    {
        std::sort(m_InstancePtrsToDraw.data(), m_InstancePtrsToDraw.data() + m_InstancePtrsToDraw.size(), CompareDrawItemsTransparent);
    }
}

const DrawItem* TransparentDrawStrategy::GetNextItem()
{
    if (m_ReadPtr >= m_InstancePtrsToDraw.size())
    {
        m_InstancesToDraw.clear();
        m_InstancePtrsToDraw.clear();
        return nullptr;
    }

    return m_InstancePtrsToDraw[m_ReadPtr++];
}
