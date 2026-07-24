#include <render/passes/omm/OmmBuildQueue.h>

#include <math/math.h>
#include <core/vfs/VFS.h>
#include <assets/loader/ShaderFactory.h>
#include <rhi/common/misc.h>
#include <rhi/utils.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>

namespace
{
    static bool IsValidBakeGeometry(
        const caustica::scene::MeshRenderResourceSnapshot& mesh,
        const caustica::render::MeshGpuRecord* meshGpu,
        const OmmBuildQueue::BuildInput::Geometry& geometry)
    {
        if (geometry.geometryIndexInMesh < 0 || size_t(geometry.geometryIndexInMesh) >= mesh.geometries.size())
            return false;

        if (!geometry.alphaTexture || !geometry.alphaTexture->gpu.texture)
            return false;

        return meshGpu && meshGpu->indexBuffer && meshGpu->vertexBuffer;
    }

    static caustica::omm::GpuBakeRhi::Input GetBakeInput(
        caustica::omm::GpuBakeRhi::Operation op,
        const caustica::scene::MeshRenderResourceSnapshot& mesh,
        const caustica::render::MeshGpuRecord& meshGpu,
        const OmmBuildQueue::BuildInput::Geometry& geometry)
    {

        using namespace caustica::math;

        assert(IsValidBakeGeometry(mesh, &meshGpu, geometry));

        const caustica::scene::GeometryRenderResourceSnapshot* meshGeometry =
            &mesh.geometries[size_t(geometry.geometryIndexInMesh)];

        const uint32_t indexOffset = mesh.indexOffset + meshGeometry->indexOffsetInMesh;
        const uint32_t vertexOffset = (mesh.vertexOffset + meshGeometry->vertexOffsetInMesh);

        caustica::omm::GpuBakeRhi::Input params;
        params.operation = op;
        params.alphaTexture = geometry.alphaTexture->gpu.texture;
        params.alphaCutoff = geometry.alphaCutoff;
        params.alphaCutoffGreater = geometry.alphaCutoffGT;
        params.alphaCutoffLessEqual = geometry.alphaCutoffLE;
        params.bilinearFilter = true;
        params.enableLevelLineIntersection = geometry.enableLevelLineIntersection;
        params.sampleMode = caustica::rhi::SamplerAddressMode::Wrap;
        params.indexBuffer = meshGpu.indexBuffer;
        params.texCoordBuffer = meshGpu.vertexBuffer;
        params.texCoordBufferOffsetInBytes = (uint)(vertexOffset * sizeof(float2)
            + meshGpu.vertexBufferRange(caustica::VertexAttribute::TexCoord1).byteOffset);
        params.texCoordStrideInBytes = sizeof(float2);
        params.indexBufferOffsetInBytes = size_t(indexOffset) * sizeof(uint32_t);
        params.numIndices = meshGeometry->numIndices;
        params.maxSubdivisionLevel = geometry.maxSubdivisionLevel;
        params.dynamicSubdivisionScale = geometry.dynamicSubdivisionScale;
        params.format = geometry.format;
        params.minimalMemoryMode = false;
        params.enableStats = true;
        params.force32BitIndices = geometry.force32BitIndices;
        params.enableSpecialIndices = geometry.enableSpecialIndices;
        params.computeOnly = geometry.computeOnly;
        params.enableNsightDebugMode = geometry.enableNsightDebugMode;
        params.enableTexCoordDeduplication = geometry.enableTexCoordDeduplication;
        params.maxOutOmmArraySize = geometry.maxOmmArrayDataSizeInMB << 20;
        
        return params;
    }

    enum class BufferConfig
    {
        RawUAV,
        RawUAVAndASBuildInput,
        Readback
    };

    static caustica::rhi::BufferHandle AllocateBuffer(caustica::rhi::DeviceHandle device, const char* name, size_t byteSize, BufferConfig cfg)
    {
        caustica::rhi::BufferDesc desc;
        desc.byteSize = byteSize;
        desc.debugName = name;
        desc.format = caustica::rhi::Format::R32_UINT;
        if (cfg == BufferConfig::RawUAV)
        {
            desc.canHaveUAVs = true;
            desc.canHaveRawViews = true;
        }
        else if (cfg == BufferConfig::RawUAVAndASBuildInput)
        {
            desc.canHaveUAVs = true;
            desc.canHaveRawViews = true;
            desc.isAccelStructBuildInput = true;
        }
        else
        {
            desc.canHaveUAVs = false;
            desc.cpuAccess = caustica::rhi::CpuAccessMode::Read;
        }
        return device->createBuffer(desc);
    }

    static caustica::rhi::rt::OpacityMicromapDesc GetOpacityMicromapDesc(
        caustica::rhi::BufferHandle ommArrayBuffer,
        size_t ommArrayBufferOffset,
        caustica::rhi::BufferHandle ommDescBuffer,
        size_t ommDescBufferOffset,
        const std::vector<caustica::rhi::rt::OpacityMicromapUsageCount>& usageDescs,
        caustica::rhi::rt::OpacityMicromapBuildFlags flags)
    {
        caustica::rhi::rt::OpacityMicromapDesc desc;
        desc.debugName = "OmmArray";
        desc.flags = flags;
        desc.counts = usageDescs;
        desc.inputBuffer = ommArrayBuffer;
        desc.inputBufferOffset = ommArrayBufferOffset;
        desc.perOmmDescs = ommDescBuffer;
        desc.perOmmDescsOffset = ommDescBufferOffset;
        return desc;
    }

    struct LinearBufferAllocator
    {
        LinearBufferAllocator()
        {
        }

        size_t allocate(size_t sizeInBytes, size_t alignment)
        {
            m_offset = caustica::rhi::align(m_offset, alignment);
            size_t offset = m_offset;
            m_offset += sizeInBytes;
            return offset;
        }

        caustica::rhi::BufferHandle CreateBuffer(caustica::rhi::DeviceHandle device, const char* name, BufferConfig config)
        {
            if (m_offset == 0)
                return nullptr;

            caustica::rhi::BufferHandle handle = AllocateBuffer(device, name, m_offset, config);
            m_offset = 0;
            return handle;
        }

    private:
        size_t m_offset = 0;
    };
}

OmmBuildQueue::OmmBuildQueue(
    caustica::rhi::DeviceHandle& device, 
    std::shared_ptr<caustica::DescriptorTableManager> descriptorTable, 
    std::shared_ptr<caustica::ShaderFactory> shaderFactory)
    : m_device(device)
    , m_descriptorTable(std::move(descriptorTable))
    , m_shaderFactory(std::move(shaderFactory))
{
    caustica::omm::GpuBakeRhi::ShaderProvider provider;

    provider.bindingOffsets = caustica::rhi::VulkanBindingOffsets { .shaderResource = 200, .sampler = 100, .constantBuffer = 300, .unorderedAccess = 400 };
    //provider.bindingOffsets = caustica::rhi::VulkanBindingOffsets { .shaderResource = UINT_MAX, .sampler = UINT_MAX, .constantBuffer = UINT_MAX, .unorderedAccess = UINT_MAX }; <- if MR gets accepted

    provider.shaders = [this](caustica::rhi::ShaderType type, const char* shaderName, const char* shaderEntryName)->caustica::rhi::ShaderHandle
    {
        std::vector<caustica::ShaderMacro> defines = { caustica::ShaderMacro("COMPILER_DXC", "1") };
        std::string shaderNameStr = std::string("omm/third_party/omm/libraries/omm-lib/shaders/") + shaderName;
        return m_shaderFactory->createShader(shaderNameStr.c_str(), shaderEntryName, &defines, type);
    };

    caustica::omm::GpuBakeRhi::MessageCallback messageCb = [](::omm::MessageSeverity severity, const char* message) {
        caustica::info("[OMM SDK]: %d %s", severity, message);
    };

    // Intialize the the internal baker, which records some buffer updates into a command list
    caustica::rhi::CommandListHandle initCommandList = m_device->createCommandList();
    initCommandList->open();
    m_baker = std::make_unique<caustica::omm::GpuBakeRhi>(m_device, initCommandList, false /*debug*/, &provider, messageCb);

    // Submit baker init command list
    initCommandList->close();
    m_device->executeCommandList(initCommandList);
    // TODO: Do we really need to wait for device iddle here, or can we just subscribe a query to ensure resources are up by the time we use them?
    m_device->waitForIdle();
}

OmmBuildQueue::~OmmBuildQueue()
{
}

caustica::render::MeshGpuRecord* OmmBuildQueue::findMeshGpu(
    const caustica::scene::MeshRenderResourceSnapshot& mesh) const
{
    if (m_sceneGpuResources == nullptr)
        return nullptr;
    const auto it = m_sceneGpuResources->meshRegistry.find(mesh.id);
    return it != m_sceneGpuResources->meshRegistry.end() ? &it->second : nullptr;
}

void OmmBuildQueue::runSetup(caustica::rhi::ICommandList& commandList, BuildTask& task)
{
    assert(task.state == BuildState::None);

    const auto& mesh = task.input.mesh;
    caustica::render::MeshGpuRecord* meshGpu = findMeshGpu(mesh);
    if (meshGpu == nullptr)
        return;

    LinearBufferAllocator ommIndexBufferAllocator;
    LinearBufferAllocator ommDescBufferAllocator;
    LinearBufferAllocator ommDescArrayHistogramBufferAllocator;
    LinearBufferAllocator ommIndexArrayHistogramBufferAllocator;
    LinearBufferAllocator ommPostBuildInfoBufferAllocator;
    LinearBufferAllocator ommReadbackBufferAllocator;

    std::vector<BufferInfo>& bufferInfos = task.bufferInfos;

    for (const OmmBuildQueue::BuildInput::Geometry& geom : task.input.geometries)
    {
        caustica::omm::GpuBakeRhi::Input input =
            GetBakeInput(caustica::omm::GpuBakeRhi::Operation::Setup, mesh, *meshGpu, geom);

        caustica::omm::GpuBakeRhi::PreDispatchInfo setupInfo;
        m_baker->GetPreDispatchInfo(input, setupInfo);

        BufferInfo info;
        info.ommIndexFormat                         = setupInfo.ommIndexFormat;
        info.ommIndexCount                          = setupInfo.ommIndexCount;
        info.ommIndexOffset                         = ommIndexBufferAllocator.allocate(setupInfo.ommIndexBufferSize, 256);
        info.ommDescArrayOffset                     = ommDescBufferAllocator.allocate(setupInfo.ommDescBufferSize, 256);
        info.ommDescArrayHistogramOffset            = ommDescArrayHistogramBufferAllocator.allocate(setupInfo.ommDescArrayHistogramSize, 256);
        info.ommDescArrayHistogramSize              = setupInfo.ommDescArrayHistogramSize;
        info.ommDescArrayHistogramReadbackOffset    = ommReadbackBufferAllocator.allocate(setupInfo.ommDescArrayHistogramSize, 256);
        info.ommIndexHistogramOffset                = ommIndexArrayHistogramBufferAllocator.allocate(setupInfo.ommIndexHistogramSize, 256);
        info.ommIndexHistogramSize                  = setupInfo.ommIndexHistogramSize;
        info.ommIndexHistogramReadbackOffset        = ommReadbackBufferAllocator.allocate(setupInfo.ommIndexHistogramSize, 256);
        info.ommPostDispatchInfoOffset              = ommPostBuildInfoBufferAllocator.allocate(setupInfo.ommPostDispatchInfoBufferSize, 256);
        info.ommPostDispatchInfoReadbackOffset      = ommReadbackBufferAllocator.allocate(setupInfo.ommPostDispatchInfoBufferSize, 256);
        info.ommArrayDataOffset                     = 0xFFFFFFFF; // Not yet known

        bufferInfos.push_back(info);
    }

    Buffers& buffers = task.buffers;
    buffers.ommIndexBuffer                  = ommIndexBufferAllocator.CreateBuffer(m_device, "OmmIndexBuffer", BufferConfig::RawUAVAndASBuildInput);
    buffers.ommDescBuffer                   = ommDescBufferAllocator.CreateBuffer(m_device, "OmmDescBuffer", BufferConfig::RawUAVAndASBuildInput);
    buffers.ommDescArrayHistogramBuffer     = ommDescArrayHistogramBufferAllocator.CreateBuffer(m_device, "OmmDescArrayHistogramBuffer", BufferConfig::RawUAV);
    buffers.ommIndexArrayHistogramBuffer    = ommIndexArrayHistogramBufferAllocator.CreateBuffer(m_device, "OmmIndexArrayHistogramBuffer", BufferConfig::RawUAV);
    buffers.ommPostDispatchInfoBuffer       = ommPostBuildInfoBufferAllocator.CreateBuffer(m_device, "OmmPostBuildInfoBuffer", BufferConfig::RawUAV);
    buffers.ommReadbackBuffer               = ommReadbackBufferAllocator.CreateBuffer(m_device, "OmmGenericReadbackBuffer", BufferConfig::Readback);

    commandList.beginTrackingBufferState(buffers.ommIndexBuffer, caustica::rhi::ResourceStates::Common);
    commandList.beginTrackingBufferState(buffers.ommDescBuffer, caustica::rhi::ResourceStates::Common);
    commandList.beginTrackingBufferState(buffers.ommDescArrayHistogramBuffer, caustica::rhi::ResourceStates::Common);
    commandList.beginTrackingBufferState(buffers.ommIndexArrayHistogramBuffer, caustica::rhi::ResourceStates::Common);
    commandList.beginTrackingBufferState(buffers.ommPostDispatchInfoBuffer, caustica::rhi::ResourceStates::Common);
    commandList.beginTrackingBufferState(buffers.ommReadbackBuffer, caustica::rhi::ResourceStates::Common);

    // dispatch all setup tasks.
    for (uint32_t i = 0; i < task.input.geometries.size(); ++i)
    {
        const BufferInfo& bufferInfo = bufferInfos[i];
        const OmmBuildQueue::BuildInput::Geometry& geom = task.input.geometries[i];

        caustica::omm::GpuBakeRhi::Input input =
            GetBakeInput(caustica::omm::GpuBakeRhi::Operation::Setup, mesh, *meshGpu, geom);

        caustica::omm::GpuBakeRhi::Buffers output;
        output.ommDescBuffer                        = buffers.ommDescBuffer;
        output.ommDescBufferOffset                  = (uint32_t)bufferInfo.ommDescArrayOffset;
        output.ommIndexBuffer                       = buffers.ommIndexBuffer;
        output.ommIndexBufferOffset                 = (uint32_t)bufferInfo.ommIndexOffset;
        output.ommDescArrayHistogramBuffer          = buffers.ommDescArrayHistogramBuffer;
        output.ommDescArrayHistogramBufferOffset    = (uint32_t)bufferInfo.ommDescArrayHistogramOffset;
        output.ommIndexHistogramBuffer              = buffers.ommIndexArrayHistogramBuffer;
        output.ommIndexHistogramBufferOffset        = (uint32_t)bufferInfo.ommIndexHistogramOffset;
        output.ommPostDispatchInfoBuffer            = buffers.ommPostDispatchInfoBuffer;
        output.ommPostDispatchInfoBufferOffset      = (uint32_t)bufferInfo.ommPostDispatchInfoOffset;

        m_baker->Dispatch(&commandList, input, output);

        caustica::omm::GpuBakeRhi::PreDispatchInfo setupInfo;
        m_baker->GetPreDispatchInfo(input, setupInfo);

        commandList.copyBuffer(buffers.ommReadbackBuffer, bufferInfo.ommDescArrayHistogramReadbackOffset,  buffers.ommDescArrayHistogramBuffer,  bufferInfo.ommDescArrayHistogramOffset,      setupInfo.ommDescArrayHistogramSize);
        commandList.copyBuffer(buffers.ommReadbackBuffer, bufferInfo.ommIndexHistogramReadbackOffset,  buffers.ommIndexArrayHistogramBuffer, bufferInfo.ommIndexHistogramOffset, setupInfo.ommIndexHistogramSize);
        commandList.copyBuffer(buffers.ommReadbackBuffer, bufferInfo.ommPostDispatchInfoReadbackOffset,   buffers.ommPostDispatchInfoBuffer,       bufferInfo.ommPostDispatchInfoOffset,  setupInfo.ommPostDispatchInfoBufferSize);
    }

    task.state = BuildState::Setup;
}

void OmmBuildQueue::allocateOMMArrayDataBuffer(BuildTask& task)
{
    LinearBufferAllocator ommArrayDataBufferAllocator;
    
    void* pReadbackData = m_device->mapBuffer(task.buffers.ommReadbackBuffer, caustica::rhi::CpuAccessMode::Read);
    for (uint32_t i = 0; i < task.input.geometries.size(); ++i)
    {
        BufferInfo& bufferInfo = task.bufferInfos[i];
            
        m_baker->ReadUsageDescBuffer((uint8_t*)pReadbackData + bufferInfo.ommDescArrayHistogramReadbackOffset, bufferInfo.ommDescArrayHistogramSize, bufferInfo.ommArrayHistogram);
            
        m_baker->ReadUsageDescBuffer((uint8_t*)pReadbackData + bufferInfo.ommIndexHistogramReadbackOffset, bufferInfo.ommIndexHistogramSize, bufferInfo.ommIndexHistogram);

        caustica::omm::GpuBakeRhi::PostDispatchInfo postDispatchInfo;
        m_baker->ReadPostDispatchInfo((uint8_t*)pReadbackData + bufferInfo.ommPostDispatchInfoReadbackOffset, sizeof(postDispatchInfo), postDispatchInfo);
        size_t ommArrayDataOffset = ommArrayDataBufferAllocator.allocate(postDispatchInfo.ommArrayBufferSize, 256);

        if ((ommArrayDataOffset) > std::numeric_limits<uint32_t>::max())
        {
            assert(false && "OH OH. Exceeding 4gb of ommArrayData");
        }

        bufferInfo.ommArrayDataOffset = (uint32_t)ommArrayDataOffset;
    }
    m_device->unmapBuffer(task.buffers.ommReadbackBuffer);

    task.buffers.ommArrayDataBuffer = ommArrayDataBufferAllocator.CreateBuffer(m_device, "OmmArrayBuffer", BufferConfig::RawUAVAndASBuildInput);
}

void OmmBuildQueue::bakeOmmArrayData(caustica::rhi::ICommandList& commandList, BuildTask& task)
{
    commandList.beginTrackingBufferState(task.buffers.ommIndexBuffer, caustica::rhi::ResourceStates::Common);
    commandList.beginTrackingBufferState(task.buffers.ommDescBuffer, caustica::rhi::ResourceStates::Common);
    commandList.beginTrackingBufferState(task.buffers.ommDescArrayHistogramBuffer, caustica::rhi::ResourceStates::Common);
    commandList.beginTrackingBufferState(task.buffers.ommIndexArrayHistogramBuffer, caustica::rhi::ResourceStates::Common);
    commandList.beginTrackingBufferState(task.buffers.ommPostDispatchInfoBuffer, caustica::rhi::ResourceStates::Common);
    commandList.beginTrackingBufferState(task.buffers.ommArrayDataBuffer, caustica::rhi::ResourceStates::Common);

    commandList.clearBufferUInt(task.buffers.ommArrayDataBuffer, 0);

    const auto& mesh = task.input.mesh;
    caustica::render::MeshGpuRecord* meshGpu = findMeshGpu(mesh);
    if (meshGpu == nullptr)
        return;

    for (uint32_t i = 0; i < task.input.geometries.size(); ++i)
    {
        const BufferInfo& bufferInfo = task.bufferInfos[i];
        const OmmBuildQueue::BuildInput::Geometry& geom = task.input.geometries[i];

        caustica::omm::GpuBakeRhi::Input input =
            GetBakeInput(caustica::omm::GpuBakeRhi::Operation::Bake, mesh, *meshGpu, geom);

        caustica::omm::GpuBakeRhi::Buffers output;
        output.ommArrayBuffer = task.buffers.ommArrayDataBuffer;
        output.ommArrayBufferOffset = (uint32_t)bufferInfo.ommArrayDataOffset;
        output.ommDescBuffer = task.buffers.ommDescBuffer;
        output.ommDescBufferOffset = (uint32_t)bufferInfo.ommDescArrayOffset;
        output.ommIndexBuffer = task.buffers.ommIndexBuffer;
        output.ommIndexBufferOffset = (uint32_t)bufferInfo.ommIndexOffset;
        output.ommDescArrayHistogramBuffer = task.buffers.ommDescArrayHistogramBuffer;
        output.ommDescArrayHistogramBufferOffset = (uint32_t)bufferInfo.ommDescArrayHistogramOffset;
        output.ommIndexHistogramBuffer = task.buffers.ommIndexArrayHistogramBuffer;
        output.ommIndexHistogramBufferOffset = (uint32_t)bufferInfo.ommIndexHistogramOffset;
        output.ommPostDispatchInfoBuffer = task.buffers.ommPostDispatchInfoBuffer;
        output.ommPostDispatchInfoBufferOffset = (uint32_t)bufferInfo.ommPostDispatchInfoOffset;

        m_baker->Dispatch(&commandList, input, output);

        caustica::omm::GpuBakeRhi::PreDispatchInfo setupInfo;
        m_baker->GetPreDispatchInfo(input, setupInfo);

        commandList.copyBuffer(task.buffers.ommReadbackBuffer, bufferInfo.ommPostDispatchInfoReadbackOffset, task.buffers.ommPostDispatchInfoBuffer, bufferInfo.ommPostDispatchInfoOffset, setupInfo.ommPostDispatchInfoBufferSize);
    }
}

std::vector<bvh::OmmAttachment> OmmBuildQueue::buildOMMAttachments(caustica::rhi::ICommandList& commandList, BuildTask& task)
{
    std::vector<bvh::OmmAttachment> ommAttachment;
    ommAttachment.resize(task.input.mesh.geometries.size());
    caustica::render::MeshGpuRecord* meshGpu = findMeshGpu(task.input.mesh);
    if (meshGpu == nullptr)
        return ommAttachment;

    // Build the OMM Arrays
    for (uint32_t i = 0; i < task.input.geometries.size(); ++i)
    {
        const OmmBuildQueue::BuildInput::Geometry& geom = task.input.geometries[i];

        const BufferInfo& bufferInfo = task.bufferInfos[i];

        caustica::rhi::rt::OpacityMicromapDesc desc = GetOpacityMicromapDesc(
            task.buffers.ommArrayDataBuffer, bufferInfo.ommArrayDataOffset,
            task.buffers.ommDescBuffer, bufferInfo.ommDescArrayOffset,
            bufferInfo.ommArrayHistogram, geom.flags);

        caustica::rhi::rt::OpacityMicromapHandle ommBuffer = m_device->createOpacityMicromap(desc);

        meshGpu->opacityMicromaps.push_back(ommBuffer);

        commandList.buildOpacityMicromap(ommBuffer, desc);

        ommAttachment[geom.geometryIndexInMesh] =
        {
            .ommBuffer = ommBuffer,
            .ommIndexFormat = bufferInfo.ommIndexFormat,
            .ommIndexHistogram = bufferInfo.ommIndexHistogram,
            .ommIndexBuffer = task.buffers.ommIndexBuffer,
            .ommIndexBufferOffset = (uint32_t)bufferInfo.ommIndexOffset,
            .ommArrayDataBuffer = task.buffers.ommArrayDataBuffer,
            .ommArrayDataBufferOffset = (uint32_t)bufferInfo.ommArrayDataOffset
        };
    }

    return ommAttachment;
}

void OmmBuildQueue::buildBLASWithOMM(caustica::rhi::ICommandList& commandList, BuildTask& task, const std::vector<bvh::OmmAttachment>& ommAttachment)
{
    caustica::render::MeshGpuRecord* meshGpu = findMeshGpu(task.input.mesh);
    if (meshGpu == nullptr)
        return;
    caustica::rhi::rt::AccelStructDesc blasDesc = bvh::getMeshBlasDesc(
        task.input.bvhCfg, task.input.mesh, *meshGpu, ommAttachment.data(), false,
        m_materialGpuCache);
    // The spec says instance Id is only 24 bits, and we already take 16 for the instance index, so we only have 8 bits for the geometry.
    // On the other side, we could completely skip this and just use DXR 1.1's GeometryIndex() directly in the shader.
    assert((int)blasDesc.bottomLevelGeometries.size() < (1 << 8)); // we can only hold 8 bits for the geometry index in the HitInfo - see GeometryInstanceID in SceneTypes.hlsli
    caustica::rhi::rt::AccelStructHandle as = m_device->createAccelStruct(blasDesc);
    caustica::rhi::utils::BuildBottomLevelAccelStruct(&commandList, as, blasDesc);
        
    // store results
    meshGpu->accelStructOmm = as;
}

void OmmBuildQueue::runBakeAndBuild(caustica::rhi::ICommandList& commandList, BuildTask& task)
{
    assert(task.state == BuildState::None || task.state == BuildState::Setup);

    allocateOMMArrayDataBuffer(task);

    bakeOmmArrayData(commandList, task); // dispatch the bake which will fill the ommArrayData

    std::vector<bvh::OmmAttachment> ommAttachment = buildOMMAttachments(commandList, task);

    buildBLASWithOMM(commandList, task, ommAttachment); // Build a BLAS with OMM Attached.

    task.state = BuildState::BakeAndBuild;
}

void OmmBuildQueue::submitAndSubscribeQuery(caustica::rhi::ICommandList& commandList)
{
    // Need to submit the command list for the event query to be relevant. Ideally, we would have the query point to the next fence value instead of the last submitted one, or we would defer query set up to happen on command list submission.
    commandList.close();
    m_device->executeCommandList(&commandList, caustica::rhi::CommandQueue::Graphics);
    commandList.open();

    // Subscribe to this submission
    m_device->setEventQuery(m_InFlightQuery, caustica::rhi::CommandQueue::Graphics);
}

void OmmBuildQueue::finalize(caustica::rhi::ICommandList& commandList, BuildTask& task)
{
    caustica::render::MeshGpuRecord* meshGpu = findMeshGpu(task.input.mesh);
    if (meshGpu == nullptr)
        return;
    std::unique_ptr<caustica::render::MeshGpuDebugData> debugData =
        std::make_unique<caustica::render::MeshGpuDebugData>();
    debugData->ommArrayDataBuffer = task.buffers.ommArrayDataBuffer;
    debugData->ommDescBuffer = task.buffers.ommDescBuffer;
    debugData->ommIndexBuffer = task.buffers.ommIndexBuffer;

    debugData->ommArrayDataBufferDescriptor =
        std::make_shared<caustica::DescriptorHandle>(m_descriptorTable->createDescriptorHandle(caustica::rhi::BindingSetItem::RawBuffer_SRV(0, task.buffers.ommArrayDataBuffer)));
    debugData->ommDescBufferDescriptor =
        std::make_shared<caustica::DescriptorHandle>(m_descriptorTable->createDescriptorHandle(caustica::rhi::BindingSetItem::RawBuffer_SRV(0, task.buffers.ommDescBuffer)));
    debugData->ommIndexBufferDescriptor =
        std::make_shared<caustica::DescriptorHandle>(m_descriptorTable->createDescriptorHandle(caustica::rhi::BindingSetItem::RawBuffer_SRV(0, task.buffers.ommIndexBuffer)));

    assert(!meshGpu->debugData);
    meshGpu->debugData = std::move(debugData);
    meshGpu->debugDataDirty = true;
    meshGpu->geometryDebugData.resize(task.input.mesh.geometries.size());

    void* pReadbackData = m_device->mapBuffer(task.buffers.ommReadbackBuffer, caustica::rhi::CpuAccessMode::Read);
    for (uint32_t i = 0; i < task.input.geometries.size(); ++i)
    {
        // Get debug info
        BufferInfo& bufferInfo = task.bufferInfos[i];
        caustica::omm::GpuBakeRhi::PostDispatchInfo postDispatchInfo;
        m_baker->ReadPostDispatchInfo((uint8_t*)pReadbackData + bufferInfo.ommPostDispatchInfoReadbackOffset, sizeof(postDispatchInfo), postDispatchInfo);

        const OmmBuildQueue::BuildInput::Geometry& geom = task.input.geometries[i];
        caustica::render::MeshGeometryGpuDebugData& geometryDebug =
            meshGpu->geometryDebugData[geom.geometryIndexInMesh];
        geometryDebug.ommArrayDataOffset = (uint32_t)bufferInfo.ommArrayDataOffset;
        geometryDebug.ommDescBufferOffset = (uint32_t)bufferInfo.ommDescArrayOffset;
        geometryDebug.ommIndexBufferOffset = (uint32_t)bufferInfo.ommIndexOffset;
        geometryDebug.ommIndexBufferFormat = bufferInfo.ommIndexFormat;
        geometryDebug.ommStatsTotalKnown =
            postDispatchInfo.ommTotalOpaqueCount + postDispatchInfo.ommTotalTransparentCount;
        geometryDebug.ommStatsTotalUnknown = postDispatchInfo.ommTotalUnknownCount;
    }
    m_device->unmapBuffer(task.buffers.ommReadbackBuffer);

    task.buffers.ommReadbackBuffer = nullptr;
    m_baker->Clear();
}

void OmmBuildQueue::BuildTask::reset()
{
    bufferInfos.clear();
}

void OmmBuildQueue::consumeOneTask(caustica::rhi::ICommandList& commandList, BuildState taskState)
{
    for (auto& task : m_pending)
    {
        if (task.state == taskState)
        {
            bool finished = executeTask(commandList, task);
            if (finished)
            {
                // Remove the task from the queue
                task.reset(); // No need to swap the whole BufferInfo vector just to pop it
                std::swap(m_pending.back(), task);
                m_pending.pop_back();
            }

            return;
        }
    }
}

bool OmmBuildQueue::executeTask(caustica::rhi::ICommandList& commandList, BuildTask& task)
{
    if (!task.input.mesh.id || findMeshGpu(task.input.mesh) == nullptr)
    {
        if (task.state != BuildState::None)
            m_baker->Clear();
        return true;
    }

    switch (task.state)
    {
    case BuildState::None:
    {
        runSetup(commandList, task);
        break;
    }
    case BuildState::Setup:
    {
        runBakeAndBuild(commandList, task);
        break;
    }
    case BuildState::BakeAndBuild:
    {
        finalize(commandList, task);
        return true;
    }
    default:
    {
        assert(false);
        break;
    }
    }

    return false;
}

bool OmmBuildQueue::readyToRecordWork()
{
    if (!m_InFlightQuery)
    {
        // create a query to check for command list completion
        m_InFlightQuery = m_device->createEventQuery();
        return true;
    }

    if (m_device->pollEventQuery(m_InFlightQuery))
    {
        return true;
    }

    return false;
}

void OmmBuildQueue::update(caustica::rhi::ICommandList& commandList)
{
    if (readyToRecordWork() && !m_pending.empty())
    {
        // Work backwards from the final state to avoid consuming consecutive states of the same task in a single frame.
        consumeOneTask(commandList, BuildState::BakeAndBuild);
        consumeOneTask(commandList, BuildState::Setup);
        consumeOneTask(commandList, BuildState::None);

        submitAndSubscribeQuery(commandList);
    }
}

void OmmBuildQueue::queueBuild(const BuildInput& input)
{
    if (!input.mesh.id)
        return;

    BuildInput filteredInput = input;
    filteredInput.geometries.clear();
    caustica::render::MeshGpuRecord* meshGpu = findMeshGpu(input.mesh);
    for (const BuildInput::Geometry& geometry : input.geometries)
    {
        if (IsValidBakeGeometry(input.mesh, meshGpu, geometry))
            filteredInput.geometries.push_back(geometry);
    }

    if (!filteredInput.geometries.empty())
        m_pending.push_back(filteredInput);
}

uint32_t OmmBuildQueue::numPendingBuilds() const
{
    return (uint32_t)m_pending.size();
}

void OmmBuildQueue::cancelPendingBuilds()
{
    m_pending.clear();
}
