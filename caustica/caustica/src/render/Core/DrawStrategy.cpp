#include <render/Core/DrawStrategy.h>
#include <render/Core/GeometryPasses.h>
#include <scene/SceneGraph.h>
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

void InstancedOpaqueDrawStrategy::FillChunk()
{
    m_InstanceChunk.resize(m_ChunkSize);

    DrawItem* writePtr = m_InstanceChunk.data();
    size_t itemCount = 0;

    while (m_Walker && itemCount < m_ChunkSize)
    {
        auto relevantContentFlags = SceneContentFlags::OpaqueMeshes | SceneContentFlags::AlphaTestedMeshes;
        bool subgraphContentRelevant = (m_Walker->GetSubgraphContentFlags() & relevantContentFlags) != 0;
        bool nodeContentsRelevant = (m_Walker->GetLeafContentFlags() & relevantContentFlags) != 0;

        bool nodeVisible = false;
        if (subgraphContentRelevant)
        {
            nodeVisible = m_ViewFrustum.intersectsWith(m_Walker->GetGlobalBoundingBox());

            if (nodeVisible && nodeContentsRelevant)
            {
                auto meshInstance = dynamic_cast<MeshInstance*>(m_Walker->GetLeaf().get());
                if (meshInstance)
                {
                    const caustica::MeshInfo* mesh = meshInstance->GetMesh().get();

                    size_t requiredChunkSize = itemCount + mesh->geometries.size();
                    if (m_InstanceChunk.size() < requiredChunkSize)
                    {
                        m_InstanceChunk.resize(requiredChunkSize);
                        writePtr = m_InstanceChunk.data() + itemCount;
                    }

                    for (const auto& geometry : mesh->geometries)
                    {
                        auto domain = geometry->material->domain;
                        if (domain != MaterialDomain::Opaque && domain != MaterialDomain::AlphaTested)
                            continue;
                        
                        if (mesh->geometries.size() > 1 && !mesh->skinPrototype)
                        {
                            dm::box3 geometryGlobalBoundingBox = geometry->objectSpaceBounds * m_Walker->GetLocalToWorldTransformFloat();
                            if (!m_ViewFrustum.intersectsWith(geometryGlobalBoundingBox))
                                continue;
                        }

                        DrawItem& item = *writePtr;
                        item.instance = meshInstance;
                        item.mesh = mesh;
                        item.geometry = geometry.get();
                        item.material = geometry->material.get();
                        item.buffers = item.mesh->buffers.get();
                        item.cullMode = (item.material->doubleSided) ? nvrhi::RasterCullMode::None : nvrhi::RasterCullMode::Back;
                        item.distanceToCamera = 0; // don't care
                        
                        ++writePtr;
                        ++itemCount;
                    }
                }
            }
        }

        m_Walker.Next(nodeVisible);
    }

    m_InstanceChunk.resize(itemCount);
    m_InstancePtrChunk.resize(itemCount);

    for (size_t i = 0; i < itemCount; i++)
    {
        m_InstancePtrChunk[i] = &m_InstanceChunk[i];
    }
    
    if (itemCount > 1)
    {
        std::sort(m_InstancePtrChunk.data(), m_InstancePtrChunk.data() + m_InstancePtrChunk.size(), CompareDrawItemsOpaque);
    }

    m_ReadPtr = 0;
}

void caustica::render::InstancedOpaqueDrawStrategy::PrepareForView(const std::shared_ptr<caustica::SceneGraphNode>& rootNode, const caustica::IView& view)
{
    m_Walker = SceneGraphWalker(rootNode.get());
    m_ViewFrustum = view.GetViewFrustum();
    m_InstanceChunk.clear();
    m_ReadPtr = 0;
}

const DrawItem* InstancedOpaqueDrawStrategy::GetNextItem()
{
    if (m_ReadPtr >= m_InstancePtrChunk.size())
        FillChunk();

    if (m_InstancePtrChunk.empty())
        return nullptr;

    return m_InstancePtrChunk[m_ReadPtr++];
}


static int CompareDrawItemsTransparent(const DrawItem* a, const DrawItem* b)
{
    if (a->instance == b->instance)
        return a->cullMode > b->cullMode;

    return a->distanceToCamera > b->distanceToCamera;
}

void TransparentDrawStrategy::PrepareForView(const std::shared_ptr<caustica::SceneGraphNode>& rootNode, const IView& view)
{
    m_ReadPtr = 0;

    m_InstancesToDraw.clear();
    m_InstancePtrsToDraw.clear();

    float3 viewOrigin = view.GetViewOrigin();
    auto viewFrustum = view.GetViewFrustum();

    SceneGraphWalker walker(rootNode.get());
    while (walker)
    {
        auto relevantContentFlags = SceneContentFlags::BlendedMeshes;
        bool subgraphContentRelevant = (walker->GetSubgraphContentFlags() & relevantContentFlags) != 0;
        bool nodeContentsRelevant = (walker->GetLeafContentFlags() & relevantContentFlags) != 0;

        bool nodeVisible = false;
        if (subgraphContentRelevant)
        {
            nodeVisible = viewFrustum.intersectsWith(walker->GetGlobalBoundingBox());

            if (nodeVisible && nodeContentsRelevant)
            {
                auto meshInstance = dynamic_cast<MeshInstance*>(walker->GetLeaf().get());
                if (meshInstance)
                {
                    const caustica::MeshInfo* mesh = meshInstance->GetMesh().get();
                    for (const auto& geometry : mesh->geometries)
                    {
                        const auto& material = geometry->material;
                        if (material->domain == MaterialDomain::Opaque || material->domain == MaterialDomain::AlphaTested)
                            continue;

                        dm::box3 geometryGlobalBoundingBox;
                        if (mesh->geometries.size() > 1 && mesh->skinPrototype.use_count() != 0)
                        {
                            geometryGlobalBoundingBox = geometry->objectSpaceBounds * walker->GetLocalToWorldTransformFloat();
                            if (!viewFrustum.intersectsWith(geometryGlobalBoundingBox))
                                continue;
                        }
                        else
                        {
                            geometryGlobalBoundingBox = walker->GetGlobalBoundingBox();
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
                }
            }
        }

        walker.Next(nodeVisible);
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
