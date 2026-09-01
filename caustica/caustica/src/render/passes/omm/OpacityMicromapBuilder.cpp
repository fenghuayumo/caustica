#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <render/core/GraphicsQueueFence.h>
#include <render/core/RenderDevice.h>
#include <render/SceneGpuResources.h>
#include <render/passes/lighting/MaterialGpuCache.h> // for StandardMaterial full definition

#include <assets/loader/ShaderFactory.h>
#include <render/core/FramebufferFactory.h>
#include <assets/loader/TextureLoader.h>

#include <core/scope.h>
#include <scene/ResourceTracker.h>

#include <rhi/utils.h>
#include <rhi/common/misc.h>

#include <utility>

#include <core/file_utils.h>
#include <core/format.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <render/core/ScopedPerfMarker.h>
#include <render/core/TextureUtils.h>

#include <render/passes/omm/OmmBuildQueue.h>

using namespace caustica::math;
using namespace caustica;

#include <shaders/Misc/OmmGeometryDebugData.hlsli>

OpacityMicromapBuilder::OpacityMicromapBuilder(caustica::rhi::DeviceHandle device,
    std::shared_ptr<caustica::DescriptorTableManager> descriptorTableManager,
    std::shared_ptr<caustica::TextureLoader> textureCache,
    std::shared_ptr<caustica::ShaderFactory> shaderFactory)
    : m_device(device)
    , m_textureCache(std::move(textureCache))
    , m_bindingCache(device)
    , m_shaderFactory(std::move(shaderFactory))
    , m_descriptorTableManager(std::move(descriptorTableManager))
{
    // Setup OMM baker.
    m_ommBuildQueue = std::make_unique<OmmBuildQueue>(device, m_descriptorTableManager, m_shaderFactory);

    // allocate dummy buffer that works even if not enabled
    caustica::rhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(GeometryDebugData) * 1024;
    bufferDesc.debugName = "BindlessGeometryDebug";
    bufferDesc.structStride = sizeof(GeometryDebugData);
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.initialState = caustica::rhi::ResourceStates::Common;
    bufferDesc.keepInitialState = true;
    m_geometryDebugBuffer = m_device->createBuffer(bufferDesc);
}

OpacityMicromapBuilder::~OpacityMicromapBuilder()
{
}

void OpacityMicromapBuilder::ensureGeometryDebugCapacity(size_t geometryCount)
{
    const size_t allocationGranularity = 1024;
    if (geometryCount <= m_geometryDebugDataPtr.size())
        return;

    m_geometryDebugDataPtr.resize(caustica::rhi::align<size_t>(geometryCount, allocationGranularity));

    caustica::rhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(GeometryDebugData) * m_geometryDebugDataPtr.size();
    bufferDesc.debugName = "BindlessGeometryDebug";
    bufferDesc.structStride = sizeof(GeometryDebugData);
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.initialState = caustica::rhi::ResourceStates::Common;
    bufferDesc.keepInitialState = true;

    m_geometryDebugBuffer = m_device->createBuffer(bufferDesc);
}

void OpacityMicromapBuilder::sceneLoaded(size_t geometryCount)
{
    // Runtime imports grow geometry count without calling sceneLoaded again.
    ensureGeometryDebugCapacity(geometryCount);
}

void OpacityMicromapBuilder::sceneUnloading()
{
    m_ommBuildQueue->cancelPendingBuilds();
}

void OpacityMicromapBuilder::createRenderPasses(
    caustica::rhi::BindingLayoutHandle bindlessLayout,
    caustica::render::RenderDevice& renderDevice)
{
    m_bindlessLayout = std::move(bindlessLayout);
    m_sceneGpuResources = renderDevice.activeSceneGpuResources();
    m_ommBuildQueue->setSceneGpuResources(m_sceneGpuResources);
}

void OpacityMicromapBuilder::setMaterialGpuCache(MaterialGpuCache* materials)
{
    m_materialGpuCache = materials;
    m_ommBuildQueue->setMaterialGpuCache(materials);
}

void OpacityMicromapBuilder::createOpacityMicromaps(
    const caustica::scene::SceneRenderData& renderData)
{
    // Always grow the debug buffer for runtime imports — AS rebuild marks DebugDataDirty
    // and update() will write per-geometry slots even when OMM baking is disabled.
    ensureGeometryDebugCapacity(renderData.staticData().geometryCount);

    m_ommBuildQueue->cancelPendingBuilds();

    m_uiData.BuildsLeftInQueue = 0;
    m_uiData.BuildsQueued = 0;

    if (!m_uiData.Enable)
    {
        m_uiData.ActiveState.reset();
        return;
    }

    m_uiData.ActiveState = m_uiData.DesiredState;

    // Queue atomically only after every eligible alpha texture is GPU-ready. This
    // avoids duplicate partial queues while still allowing a cheap retry next frame.
    for (const auto& mesh : renderData.staticData().meshSnapshots)
    {
        if (mesh.isSkinPrototype || mesh.hasSkinPrototype)
            continue;
        for (const auto& geometry : mesh.geometries)
        {
            const std::shared_ptr<StandardMaterial> material = m_materialGpuCache
                ? m_materialGpuCache->findByResourceId(geometry.materialId)
                : nullptr;
            if (material && material->enableAlphaTesting
                && material->enableBaseTexture && material->baseTexture.loaded
                && !material->baseTexture.loaded->gpu.texture)
            {
                m_waitingForMaterialTextures = true;
                m_materialStateRevision = m_materialGpuCache->materialStateRevision();
                return;
            }
        }
    }

    m_waitingForMaterialTextures = false;
    for (const auto& mesh : renderData.staticData().meshSnapshots)
    {
        if (mesh.isSkinPrototype)
            continue; // skip the skinning prototypes
        if (mesh.hasSkinPrototype)
            continue;

        OmmBuildQueue::BuildInput input;
        input.mesh = mesh;

        for (size_t i = 0; i < mesh.geometries.size(); ++i)
        {
            const auto& geometry = mesh.geometries[i];
            const std::shared_ptr<StandardMaterial> material = m_materialGpuCache
                ? m_materialGpuCache->findByResourceId(geometry.materialId)
                : nullptr;
            if (!material)
                continue;
            if (!material->enableAlphaTesting)
                continue;
            if (!material->enableBaseTexture || !material->baseTexture.loaded)
                continue;

            std::shared_ptr<ImageAsset> alphaTexture = material->baseTexture.loaded.shared();
            if (!alphaTexture || alphaTexture->gpu.texture == nullptr)
                continue;

            OmmBuildQueue::BuildInput::Geometry geom;
            geom.geometryIndexInMesh = i;
            geom.alphaTexture = alphaTexture;
            geom.alphaCutoff = material->alphaCutoff;
            geom.maxSubdivisionLevel = m_uiData.ActiveState->MaxSubdivisionLevel;
            geom.dynamicSubdivisionScale = m_uiData.ActiveState->EnableDynamicSubdivision ? m_uiData.ActiveState->DynamicSubdivisionScale : 0.f;
            geom.format = m_uiData.ActiveState->Format;
            geom.flags = m_uiData.ActiveState->Flag;
            geom.alphaCutoffGT = static_cast<caustica::omm::OpacityState>(m_uiData.ActiveState->AlphaCutoffGT);
            geom.alphaCutoffLE = static_cast<caustica::omm::OpacityState>(m_uiData.ActiveState->AlphaCutoffLE);
            geom.maxOmmArrayDataSizeInMB = m_uiData.ActiveState->MaxOmmArrayDataSizeInMB;
            geom.computeOnly = m_uiData.ActiveState->ComputeOnly;
            geom.enableLevelLineIntersection = m_uiData.ActiveState->LevelLineIntersection;
            geom.enableTexCoordDeduplication = m_uiData.ActiveState->EnableTexCoordDeduplication;
            geom.force32BitIndices = m_uiData.ActiveState->Force32BitIndices;
            geom.enableSpecialIndices = m_uiData.ActiveState->EnableSpecialIndices;
            geom.enableNsightDebugMode = m_uiData.ActiveState->EnableNsightDebugMode;

            input.geometries.push_back(geom);
        }

        if (input.geometries.size() != 0ull)
        {
            m_uiData.BuildsQueued += (uint32_t)input.geometries.size();
            m_ommBuildQueue->queueBuild(input);
        }
    }
    if (m_materialGpuCache)
        m_materialStateRevision = m_materialGpuCache->materialStateRevision();
}

void OpacityMicromapBuilder::destroyOpacityMicromaps(
    caustica::rhi::CommandList& commandList,
    const caustica::scene::SceneRenderData& renderData)
{
    commandList.close();
    m_device->executeCommandList(&commandList);
    // ADR 0002 S3: wait destroy-flush submit before clearing OMM handles.
    (void)caustica::render::syncGraphicsQueueFence(
        m_device, m_destroySyncQuery, /*runGc=*/true, "OMM destroyOpacityMicromaps");
    if (!commandList.open())
        return;

    for (const auto& mesh : renderData.staticData().meshSnapshots)
    {
        if (m_sceneGpuResources == nullptr)
            continue;
        const auto meshGpuIt = m_sceneGpuResources->meshRegistry.find(mesh.id);
        if (meshGpuIt == m_sceneGpuResources->meshRegistry.end())
            continue;
        caustica::render::MeshGpuRecord& meshGpu = meshGpuIt->second;
        meshGpu.accelStructOmm = nullptr;
        meshGpu.opacityMicromaps.clear();
        meshGpu.debugData = nullptr;
        meshGpu.geometryDebugData.clear();
        meshGpu.debugDataDirty = true;
    }
}

void OpacityMicromapBuilder::buildOpacityMicromaps(
    caustica::rhi::CommandList& commandList,
    const caustica::scene::SceneRenderData& renderData)
{
    commandList.beginMarker("OMM Updates");

    if (m_materialGpuCache
        && m_materialStateRevision != m_materialGpuCache->materialStateRevision())
    {
        m_uiData.TriggerRebuild = true;
    }

    if (!m_uiData.Enable)
    {
        m_ommBuildQueue->cancelPendingBuilds();
        m_uiData.BuildsLeftInQueue = 0;
        m_uiData.BuildsQueued = 0;
        commandList.endMarker();
        return;
    }

    if (m_uiData.TriggerRebuild)
    {
        destroyOpacityMicromaps(commandList, renderData);

        m_ommBuildQueue->cancelPendingBuilds();

        createOpacityMicromaps(renderData);

        m_uiData.TriggerRebuild = false;
    }
    else if (m_waitingForMaterialTextures)
    {
        createOpacityMicromaps(renderData);
    }

    m_ommBuildQueue->update(commandList);

    m_uiData.BuildsLeftInQueue = m_ommBuildQueue->numPendingBuilds();

    commandList.endMarker();
}

void OpacityMicromapBuilder::writeGeometryDebugBuffer(caustica::rhi::CommandList& commandList)
{
    commandList.writeBuffer(m_geometryDebugBuffer, m_geometryDebugDataPtr.data(), m_geometryDebugDataPtr.size() * sizeof(GeometryDebugData));
}

void OpacityMicromapBuilder::updateDebugGeometry(
    const caustica::scene::MeshRenderResourceSnapshot& mesh,
    const caustica::render::MeshGpuRecord& meshGpu)
{
    for (size_t geometryIndex = 0; geometryIndex < mesh.geometries.size(); ++geometryIndex)
    {
        const auto& geometry = mesh.geometries[geometryIndex];

        if (geometry.globalGeometryIndex < 0
            || static_cast<size_t>(geometry.globalGeometryIndex) >= m_geometryDebugDataPtr.size())
        {
            caustica::error(
                "OpacityMicromapBuilder: globalGeometryIndex %u out of range (debug slots=%zu); "
                "skipping debug write after runtime import.",
                geometry.globalGeometryIndex,
                m_geometryDebugDataPtr.size());
            continue;
        }

        if (const caustica::render::MeshGpuDebugData* debugData = meshGpu.debugData.get();
            debugData != nullptr && geometryIndex < meshGpu.geometryDebugData.size())
        {
            const caustica::render::MeshGeometryGpuDebugData& geometryDebug =
                meshGpu.geometryDebugData[geometryIndex];
            GeometryDebugData& dgdata = m_geometryDebugDataPtr[geometry.globalGeometryIndex];
            dgdata.ommArrayDataBufferIndex = debugData->ommArrayDataBufferDescriptor ? debugData->ommArrayDataBufferDescriptor->Get() : -1;
            dgdata.ommArrayDataBufferOffset = geometryDebug.ommArrayDataOffset;

            dgdata.ommDescArrayBufferIndex = debugData->ommDescBufferDescriptor ? debugData->ommDescBufferDescriptor->Get() : -1;
            dgdata.ommDescArrayBufferOffset = geometryDebug.ommDescBufferOffset;

            dgdata.ommIndexBufferIndex = debugData->ommIndexBufferDescriptor ? debugData->ommIndexBufferDescriptor->Get() : -1;
            dgdata.ommIndexBufferOffset = geometryDebug.ommIndexBufferOffset;
            dgdata.ommIndexBuffer16Bit = geometryDebug.ommIndexBufferFormat == caustica::rhi::Format::R16_UINT;
        }
        else
        {
            GeometryDebugData& dgdata = m_geometryDebugDataPtr[geometry.globalGeometryIndex];
            dgdata.ommArrayDataBufferIndex = -1;
            dgdata.ommArrayDataBufferOffset = 0xFFFFFFFF;
            dgdata.ommDescArrayBufferIndex = -1;
            dgdata.ommDescArrayBufferOffset = 0xFFFFFFFF;
            dgdata.ommIndexBufferIndex = -1;
            dgdata.ommIndexBufferOffset = 0xFFFFFFFF;
            dgdata.ommIndexBuffer16Bit = 0;
        }
    }
}

bool OpacityMicromapBuilder::update(
    caustica::rhi::CommandList& commandList,
    const caustica::scene::SceneRenderData& renderData)
{
    RAII_SCOPE( commandList.beginMarker("OpacityMicromapBuilder");, commandList.endMarker(); );

    // Runtime drag-drop grows geometry count without sceneLoaded(); resize before any writes.
    ensureGeometryDebugCapacity(renderData.staticData().geometryCount);

    bool anyDirty = false;
    for (const auto& mesh : renderData.staticData().meshSnapshots)
    {
        if (m_sceneGpuResources == nullptr)
            continue;
        const auto meshGpuIt = m_sceneGpuResources->meshRegistry.find(mesh.id);
        if (meshGpuIt == m_sceneGpuResources->meshRegistry.end())
            continue;
        caustica::render::MeshGpuRecord& meshGpu = meshGpuIt->second;
        if (meshGpu.debugDataDirty)
        {
            meshGpu.debugDataDirty = false;
            anyDirty = true;
            updateDebugGeometry(mesh, meshGpu);
        }
    }
    if (anyDirty)
        writeGeometryDebugBuffer(commandList);
    return anyDirty;
}

void OpacityMicromapBuilder::setGlobalShaderMacros(std::vector<caustica::ShaderMacro>& macros)
{
    if (m_uiData.DebugView == OpacityMicroMapDebugView::InWorld)
        macros.push_back( { "OMM_DEBUG_VIEW_IN_WORLD", "1" } );
    if (m_uiData.DebugView == OpacityMicroMapDebugView::Overlay)
        macros.push_back( { "OMM_DEBUG_VIEW_OVERLAY", "1" } );
}

void OpacityMicromapBuilder::collectBakeStats(
    const caustica::scene::SceneRenderData& renderData,
    std::vector<OmmBakeMeshStat>& out) const
{
    out.clear();
    if (m_sceneGpuResources == nullptr)
        return;

    for (const auto& mesh : renderData.staticData().meshSnapshots)
    {
        const auto meshGpuIt = m_sceneGpuResources->meshRegistry.find(mesh.id);
        if (meshGpuIt == m_sceneGpuResources->meshRegistry.end())
            continue;

        const auto& geometryDebugData = meshGpuIt->second.geometryDebugData;
        OmmBakeMeshStat meshStat;
        meshStat.debugName = mesh.debugName;
        for (const caustica::render::MeshGeometryGpuDebugData& debugData : geometryDebugData)
        {
            if (debugData.ommIndexBufferOffset == 0xFFFFFFFF)
                continue;
            const uint64_t known = debugData.ommStatsTotalKnown;
            const uint64_t unknown = debugData.ommStatsTotalUnknown;
            const uint64_t total = known + unknown;
            OmmBakeGeometryStat geomStat;
            geomStat.known = known;
            geomStat.unknown = unknown;
            geomStat.knownRatioPercent = total == 0 ? -1.f : 100.f * float(known) / float(total);
            meshStat.geometries.push_back(geomStat);
        }
        if (!meshStat.geometries.empty())
            out.push_back(std::move(meshStat));
    }
}
