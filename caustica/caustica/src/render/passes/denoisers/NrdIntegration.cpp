#include <render/passes/denoisers/NrdIntegration.h>

#if WITH_NRD

static_assert(NRD_VERSION_MAJOR >= 4 && NRD_VERSION_MINOR >= 15, "Unsupported NRD version!");

#include <render/core/RenderTargets.h>
#include <rhi/utils.h>
#include <math/math.h>
#include <scene/View.h>
#include <assets/loader/ShaderFactory.h>
#include <sstream>
#include <core/log.h>

static void* NrdAllocate(void* userArg, size_t size, size_t alignment)
{
    return malloc(size);
}

static void* NrdReallocate(void* userArg, void* memory, size_t size, size_t alignment)
{
    return realloc(memory, size);
}

static void NrdFree(void* userArg, void* memory)
{
    free(memory);
}

static caustica::rhi::Format GetRhiFormat(nrd::Format format)
{
    switch (format)
    {
    case nrd::Format::R8_UNORM:             return caustica::rhi::Format::R8_UNORM;
    case nrd::Format::R8_SNORM:             return caustica::rhi::Format::R8_SNORM;
    case nrd::Format::R8_UINT:              return caustica::rhi::Format::R8_UINT;
    case nrd::Format::R8_SINT:              return caustica::rhi::Format::R8_SINT;
    case nrd::Format::RG8_UNORM:            return caustica::rhi::Format::RG8_UNORM;
    case nrd::Format::RG8_SNORM:            return caustica::rhi::Format::RG8_SNORM;
    case nrd::Format::RG8_UINT:             return caustica::rhi::Format::RG8_UINT;
    case nrd::Format::RG8_SINT:             return caustica::rhi::Format::RG8_SINT;
    case nrd::Format::RGBA8_UNORM:          return caustica::rhi::Format::RGBA8_UNORM;
    case nrd::Format::RGBA8_SNORM:          return caustica::rhi::Format::RGBA8_SNORM;
    case nrd::Format::RGBA8_UINT:           return caustica::rhi::Format::RGBA8_UINT;
    case nrd::Format::RGBA8_SINT:           return caustica::rhi::Format::RGBA8_SINT;
    case nrd::Format::RGBA8_SRGB:           return caustica::rhi::Format::SRGBA8_UNORM;
    case nrd::Format::R16_UNORM:            return caustica::rhi::Format::R16_UNORM;
    case nrd::Format::R16_SNORM:            return caustica::rhi::Format::R16_SNORM;
    case nrd::Format::R16_UINT:             return caustica::rhi::Format::R16_UINT;
    case nrd::Format::R16_SINT:             return caustica::rhi::Format::R16_SINT;
    case nrd::Format::R16_SFLOAT:           return caustica::rhi::Format::R16_FLOAT;
    case nrd::Format::RG16_UNORM:           return caustica::rhi::Format::RG16_UNORM;
    case nrd::Format::RG16_SNORM:           return caustica::rhi::Format::RG16_SNORM;
    case nrd::Format::RG16_UINT:            return caustica::rhi::Format::RG16_UINT;
    case nrd::Format::RG16_SINT:            return caustica::rhi::Format::RG16_SINT;
    case nrd::Format::RG16_SFLOAT:          return caustica::rhi::Format::RG16_FLOAT;
    case nrd::Format::RGBA16_UNORM:         return caustica::rhi::Format::RGBA16_UNORM;
    case nrd::Format::RGBA16_SNORM:         return caustica::rhi::Format::RGBA16_SNORM;
    case nrd::Format::RGBA16_UINT:          return caustica::rhi::Format::RGBA16_UINT;
    case nrd::Format::RGBA16_SINT:          return caustica::rhi::Format::RGBA16_SINT;
    case nrd::Format::RGBA16_SFLOAT:        return caustica::rhi::Format::RGBA16_FLOAT;
    case nrd::Format::R32_UINT:             return caustica::rhi::Format::R32_UINT;
    case nrd::Format::R32_SINT:             return caustica::rhi::Format::R32_SINT;
    case nrd::Format::R32_SFLOAT:           return caustica::rhi::Format::R32_FLOAT;
    case nrd::Format::RG32_UINT:            return caustica::rhi::Format::RG32_UINT;
    case nrd::Format::RG32_SINT:            return caustica::rhi::Format::RG32_SINT;
    case nrd::Format::RG32_SFLOAT:          return caustica::rhi::Format::RG32_FLOAT;
    case nrd::Format::RGB32_UINT:           return caustica::rhi::Format::RGB32_UINT;
    case nrd::Format::RGB32_SINT:           return caustica::rhi::Format::RGB32_SINT;
    case nrd::Format::RGB32_SFLOAT:         return caustica::rhi::Format::RGB32_FLOAT;
    case nrd::Format::RGBA32_UINT:          return caustica::rhi::Format::RGBA32_UINT;
    case nrd::Format::RGBA32_SINT:          return caustica::rhi::Format::RGBA32_SINT;
    case nrd::Format::RGBA32_SFLOAT:        return caustica::rhi::Format::RGBA32_FLOAT;
    case nrd::Format::R10_G10_B10_A2_UNORM: return caustica::rhi::Format::R10G10B10A2_UNORM;
    case nrd::Format::R10_G10_B10_A2_UINT:  return caustica::rhi::Format::UNKNOWN; // not representable and not used
    case nrd::Format::R11_G11_B10_UFLOAT:   return caustica::rhi::Format::R11G11B10_FLOAT;
    case nrd::Format::R9_G9_B9_E5_UFLOAT:   return caustica::rhi::Format::UNKNOWN; // not representable and not used
    default:                                return caustica::rhi::Format::UNKNOWN;
    }
}

NrdIntegration::NrdIntegration(caustica::rhi::IDevice* device, nrd::Denoiser denoiser)
    : m_device(device)
    , m_initialized(false)
    , m_instance(nullptr)
    , m_denoiser(denoiser)
    , m_bindingCache(device)
    , m_identifier(0)
{
}

NrdIntegration::~NrdIntegration()
{
    if (m_initialized)
    {
        nrd::DestroyInstance(*m_instance);
    }
}

bool NrdIntegration::initialize(uint32_t width, uint32_t height, caustica::ShaderFactory& shaderFactory)
{
    const nrd::LibraryDesc& libraryDesc = nrd::GetLibraryDesc();

    const nrd::DenoiserDesc denoiserDescs[] =
    {
        { m_identifier, m_denoiser }
    };

    nrd::InstanceCreationDesc instanceCreationDesc;
    instanceCreationDesc.allocationCallbacks.Allocate = NrdAllocate;
    instanceCreationDesc.allocationCallbacks.Reallocate = NrdReallocate;
    instanceCreationDesc.allocationCallbacks.Free = NrdFree;
    instanceCreationDesc.denoisers = denoiserDescs;
    instanceCreationDesc.denoisersNum = dim(denoiserDescs);

    nrd::Result res = nrd::CreateInstance(instanceCreationDesc, m_instance);
    if (res != nrd::Result::SUCCESS)
        return false;

    const nrd::InstanceDesc& instanceDesc = nrd::GetInstanceDesc(*m_instance); 

    constexpr uint32_t maxInFlightFrames = 4;
    const caustica::rhi::BufferDesc constantBufferDesc = caustica::rhi::utils::CreateVolatileConstantBufferDesc(
        instanceDesc.constantBufferMaxDataSize,
        "NrdConstantBuffer", 
        instanceDesc.descriptorPoolDesc.setsMaxNum * maxInFlightFrames);

    m_constantBuffer = m_device->createBuffer(constantBufferDesc);

    for (uint32_t samplerIndex = 0; samplerIndex < instanceDesc.samplersNum; samplerIndex++)
    {
        const nrd::Sampler& samplerMode = instanceDesc.samplers[samplerIndex];

        caustica::rhi::SamplerAddressMode addressMode = caustica::rhi::SamplerAddressMode::Wrap;
        bool filter = false;

        switch (samplerMode)
        {
        case nrd::Sampler::NEAREST_CLAMP:
            addressMode = caustica::rhi::SamplerAddressMode::Clamp;
            filter = false;
            break;
        case nrd::Sampler::LINEAR_CLAMP:
            addressMode = caustica::rhi::SamplerAddressMode::Clamp;
            filter = true;
            break;
        default:
            assert(!"Unknown NRD sampler mode");
            break;
        }

        auto samplerDesc = caustica::rhi::SamplerDesc()
            .setAllAddressModes(addressMode)
            .setAllFilters(filter);

        const caustica::rhi::SamplerHandle sampler = m_device->createSampler(samplerDesc);

        if (!sampler)
        {
            assert(!"Cannot create an NRD sampler");
            return false;
        }

        m_samplers.push_back(sampler);
    }

    for (uint32_t pipelineIndex = 0; pipelineIndex < instanceDesc.pipelinesNum; pipelineIndex++)
    {
        const nrd::PipelineDesc& nrdPipelineDesc = instanceDesc.pipelines[pipelineIndex];

        std::string fileName = std::string("nrd/Source/") + nrdPipelineDesc.shaderFileName;
        std::vector<caustica::ShaderMacro> macros = { {"NRD_COMPILER_DXC", "1"}, {"NRD_NORMAL_ENCODING", "2"}, {"NRD_ROUGHNESS_ENCODING", "1"} };
        
        NrdPipeline pipeline;
        pipeline.Shader = shaderFactory.createShader(fileName.c_str(), "main", &macros, caustica::rhi::ShaderType::Compute);

        if (!pipeline.Shader)
        {
            assert(!"Cannot create an NRD shader");
            return false;
        }

        // NRD shaders are compiled by caustica_compile_shaders (caustica/CMakeLists.txt); ignore libraryDesc.spirvBindingOffsets.
        // see CAUSTICA_RHI_DEFAULT_VK_REGISTER_OFFSETS in External/cmake/compileshaders.cmake
		caustica::rhi::VulkanBindingOffsets cNRDBindingOffsets; // = { .shaderResource = libraryDesc.spirvBindingOffsets.textureOffset, .sampler = libraryDesc.spirvBindingOffsets.samplerOffset, .constantBuffer = libraryDesc.spirvBindingOffsets.constantBufferOffset, .unorderedAccess = libraryDesc.spirvBindingOffsets.storageTextureAndBufferOffset };
        
        pipeline.ResourcesBindingLayout = createResourcesBindingLayout(instanceDesc, nrdPipelineDesc);
        pipeline.ConstantsAndSamplersBindingLayout = createConstantsAndSamplersBindingLayout(instanceDesc);

        if (!pipeline.ResourcesBindingLayout || !pipeline.ConstantsAndSamplersBindingLayout)
        {
            assert(!"Cannot create an NRD binding layout");
            return false;
        }

        caustica::rhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.bindingLayouts = { pipeline.ResourcesBindingLayout, pipeline.ConstantsAndSamplersBindingLayout };
        pipelineDesc.CS = pipeline.Shader;
        pipeline.Pipeline = m_device->createComputePipeline(pipelineDesc);

        if (!pipeline.Pipeline)
        {
            assert(!"Cannot create an NRD pipeline");
            return false;
        }

        m_pipelines.push_back(pipeline);
    }

    const uint32_t poolSize = instanceDesc.permanentPoolSize + instanceDesc.transientPoolSize;

    for (uint32_t i = 0; i < poolSize; i++)
    {
        const bool isPermanent = (i < instanceDesc.permanentPoolSize);

        const nrd::TextureDesc& nrdTextureDesc = isPermanent 
            ? instanceDesc.permanentPool[i]
            : instanceDesc.transientPool[i - instanceDesc.permanentPoolSize];

        const caustica::rhi::Format format = GetRhiFormat(nrdTextureDesc.format);

        if (format == caustica::rhi::Format::UNKNOWN)
        {
            assert(!"Unknown or unsupported NRD format");
            return false;
        }

        std::stringstream ss;
        ss << "NRD " << (isPermanent ? "Permanent" : "Transient") << "Texture [" << (isPermanent ? i : i - instanceDesc.permanentPoolSize) << "]";

        caustica::rhi::TextureDesc textureDesc;
        textureDesc.width = std::max(width / nrdTextureDesc.downsampleFactor, 1u);
        textureDesc.height = std::max(height / nrdTextureDesc.downsampleFactor, 1u);
        textureDesc.format = format;
        textureDesc.mipLevels = 1;
        textureDesc.dimension = caustica::rhi::TextureDimension::Texture2D;
        textureDesc.initialState = caustica::rhi::ResourceStates::ShaderResource;
        textureDesc.keepInitialState = true;
        textureDesc.isUAV = true;
        textureDesc.debugName = ss.str();

        const caustica::rhi::TextureHandle texture = m_device->createTexture(textureDesc);

        if (!texture)
        {
            assert(!"Cannot create an NRD texture");
            return false;
        }

        if (isPermanent)
            m_permanentTextures.push_back(texture);
        else
            m_transientTextures.push_back(texture);
    }

    m_initialized = true;
    return true;
}

caustica::rhi::BindingLayoutHandle NrdIntegration::createConstantsAndSamplersBindingLayout(const nrd::InstanceDesc& instanceDesc)
{
    caustica::rhi::BindingLayoutDesc layoutDesc;
    layoutDesc.registerSpace = instanceDesc.rootSpaceIndex;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.visibility = caustica::rhi::ShaderType::Compute;

    caustica::rhi::BindingLayoutItem constantBufferItem = {};
    constantBufferItem.size = 1;
    constantBufferItem.type = caustica::rhi::ResourceType::VolatileConstantBuffer;
    constantBufferItem.slot = instanceDesc.constantBufferRegisterIndex;
    layoutDesc.bindings.push_back(constantBufferItem);

    for (uint32_t samplerIndex = 0; samplerIndex < instanceDesc.samplersNum; samplerIndex++)
    {
        caustica::rhi::BindingLayoutItem samplerItem = {};
        samplerItem.size = 1;
        samplerItem.type = caustica::rhi::ResourceType::Sampler;
        samplerItem.slot = instanceDesc.samplersBaseRegisterIndex + samplerIndex;
        layoutDesc.bindings.push_back(samplerItem);
    }

    const auto bindingLayout = m_device->createBindingLayout(layoutDesc);

    if (!bindingLayout)
    {
        assert(!"Cannot create an NRD samplers binding layout");
    }

    return bindingLayout;
}

caustica::rhi::BindingLayoutHandle NrdIntegration::createResourcesBindingLayout(const nrd::InstanceDesc& instanceDesc, const nrd::PipelineDesc& nrdPipelineDesc)
{
    caustica::rhi::BindingLayoutDesc layoutDesc;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.visibility = caustica::rhi::ShaderType::Compute;

    for (uint32_t descriptorRangeIndex = 0; descriptorRangeIndex < nrdPipelineDesc.resourceRangesNum; descriptorRangeIndex++)
    {
        const nrd::ResourceRangeDesc& nrdDescriptorRange = nrdPipelineDesc.resourceRanges[descriptorRangeIndex];

        caustica::rhi::BindingLayoutItem resourceItem = {};
        switch (nrdDescriptorRange.descriptorType)
        {
        case nrd::DescriptorType::TEXTURE:
            resourceItem.type = caustica::rhi::ResourceType::Texture_SRV;
            break;
        case nrd::DescriptorType::STORAGE_TEXTURE:
            resourceItem.type = caustica::rhi::ResourceType::Texture_UAV;
            break;
        default:
            assert(!"Unknown NRD descriptor type");
            break;
        }

        for (uint32_t descriptorOffset = 0; descriptorOffset < nrdDescriptorRange.descriptorsNum; descriptorOffset++)
        {
            resourceItem.size = 1;
            resourceItem.slot = instanceDesc.resourcesBaseRegisterIndex + descriptorOffset;
            layoutDesc.bindings.push_back(resourceItem);
        }
    }

    const auto bindingLayout = m_device->createBindingLayout(layoutDesc);

    if (!bindingLayout)
    {
        assert(!"Cannot create an NRD resources binding layout");
    }

    return bindingLayout;
}

bool NrdIntegration::isAvailable() const
{
    return m_initialized;
}

static inline void MatrixToNrd(float* dest, const dm::float4x4& m)
{
    dm::float4x4 tm = dm::transpose(m);
    memcpy(dest, &m, sizeof(m));
}

void NrdIntegration::runDenoiserPasses(
    caustica::rhi::ICommandList* commandList,
    const RenderTargets& renderTargets,
    int pass,
    const caustica::PlanarView& view, 
    const caustica::PlanarView& viewPrev, 
    uint32_t frameIndex,
    float disocclusionThreshold,
    float disocclusionThresholdAlternate,
    bool useDisocclusionThresholdAlternateMix,
    float timeDeltaBetweenFrames,
    bool enableValidation,
    bool resetHistory,
    const void* methodSettings)
{
    if (methodSettings)
    {
        nrd::SetDenoiserSettings(*m_instance, m_identifier, methodSettings);
    }

    nrd::CommonSettings commonSettings;
    MatrixToNrd(commonSettings.worldToViewMatrix, dm::affineToHomogeneous(view.getViewMatrix()));           //MatrixToNrd(commonSettings.worldToViewRotationMatrix, dm::affineToHomogeneous(view.getViewMatrix()));
    MatrixToNrd(commonSettings.worldToViewMatrixPrev, dm::affineToHomogeneous(viewPrev.getViewMatrix()));   //MatrixToNrd(commonSettings.worldToViewRotationMatrixPrev, dm::affineToHomogeneous(viewPrev.getViewMatrix()));
    MatrixToNrd(commonSettings.viewToClipMatrix, view.getProjectionMatrix(false));
    MatrixToNrd(commonSettings.viewToClipMatrixPrev, viewPrev.getProjectionMatrix(false));

    dm::float2 pixelOffset = view.getPixelOffset();
    dm::float2 prevPixelOffset = viewPrev.getPixelOffset();
    commonSettings.isMotionVectorInWorldSpace = false;
    commonSettings.motionVectorScale[0] = (commonSettings.isMotionVectorInWorldSpace)?(1.f):(1.f / view.getViewExtent().width());
    commonSettings.motionVectorScale[1] = (commonSettings.isMotionVectorInWorldSpace)?(1.f):(1.f / view.getViewExtent().height());
    commonSettings.motionVectorScale[2] = 1.0f;
    commonSettings.cameraJitter[0] = pixelOffset.x;
    commonSettings.cameraJitter[1] = pixelOffset.y;
    commonSettings.cameraJitterPrev[0] = prevPixelOffset.x;
    commonSettings.cameraJitterPrev[1] = prevPixelOffset.y;
    commonSettings.frameIndex = frameIndex;
    commonSettings.denoisingRange = kMaxSceneDistance * 2;   // with various bounces (in non-primary planes or with PSR) the virtual view Z can be much longer, so adding 2x!
    commonSettings.enableValidation = enableValidation && renderTargets.denoiserOutValidation != nullptr;
    commonSettings.disocclusionThreshold = disocclusionThreshold;
    commonSettings.disocclusionThresholdAlternate = disocclusionThresholdAlternate;
    commonSettings.isDisocclusionThresholdMixAvailable = useDisocclusionThresholdAlternateMix;
    commonSettings.timeDeltaBetweenFrames = timeDeltaBetweenFrames;
    commonSettings.accumulationMode = (resetHistory)?(nrd::AccumulationMode::CLEAR_AND_RESTART):(nrd::AccumulationMode::CONTINUE);
    
    auto const& textureDesc = renderTargets.denoiserNormalRoughness->getDesc();
    commonSettings.resourceSize[0] = textureDesc.width;
    commonSettings.resourceSize[1] = textureDesc.height;
    commonSettings.resourceSizePrev[0] = textureDesc.width;
    commonSettings.resourceSizePrev[1] = textureDesc.height;
    commonSettings.rectSize[0] = textureDesc.width;
    commonSettings.rectSize[1] = textureDesc.height;
    commonSettings.rectSizePrev[0] = textureDesc.width;
    commonSettings.rectSizePrev[1] = textureDesc.height;
    

    nrd::SetCommonSettings(*m_instance, commonSettings);

    const nrd::DispatchDesc* dispatchDescs = nullptr;
    uint32_t dispatchDescNum = 0;
    nrd::GetComputeDispatches(*m_instance, &m_identifier, 1, dispatchDescs, dispatchDescNum);

    const nrd::InstanceDesc& instanceDesc = nrd::GetInstanceDesc(*m_instance);

    for (uint32_t dispatchIndex = 0; dispatchIndex < dispatchDescNum; dispatchIndex++)
    {
        const nrd::DispatchDesc& dispatchDesc = dispatchDescs[dispatchIndex];

        if (dispatchDesc.name)
        {
            commandList->beginMarker(dispatchDesc.name);
        }

        assert(m_constantBuffer);
        commandList->writeBuffer(m_constantBuffer, dispatchDesc.constantBufferData, dispatchDesc.constantBufferDataSize);

        caustica::rhi::BindingSetDesc constantsAndSampelrsSetDesc;
        constantsAndSampelrsSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::ConstantBuffer(instanceDesc.constantBufferRegisterIndex, m_constantBuffer));

        for (uint32_t samplerIndex = 0; samplerIndex < instanceDesc.samplersNum; samplerIndex++)
        {
            assert(m_samplers[samplerIndex]);
            constantsAndSampelrsSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::Sampler(instanceDesc.samplersBaseRegisterIndex + samplerIndex, m_samplers[samplerIndex]));
        }

        caustica::rhi::BindingSetDesc resourcesSetDesc;
        const nrd::PipelineDesc& nrdPipelineDesc = instanceDesc.pipelines[dispatchDesc.pipelineIndex];
        uint32_t resourceIndex = 0;

        for (uint32_t descriptorRangeIndex = 0; descriptorRangeIndex < nrdPipelineDesc.resourceRangesNum; descriptorRangeIndex++)
        {
            const nrd::ResourceRangeDesc& nrdDescriptorRange = nrdPipelineDesc.resourceRanges[descriptorRangeIndex];

            for (uint32_t descriptorOffset = 0; descriptorOffset < nrdDescriptorRange.descriptorsNum; descriptorOffset++)
            {
                assert(resourceIndex < dispatchDesc.resourcesNum);
                const nrd::ResourceDesc& resource = dispatchDesc.resources[resourceIndex];

                caustica::rhi::TextureHandle texture;
                switch (resource.type)
                {
                case nrd::ResourceType::IN_MV:
                    texture = renderTargets.denoiserMotionVectors;
                    break;
                case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
                    texture = renderTargets.denoiserNormalRoughness;
                    break;
                case nrd::ResourceType::IN_VIEWZ:
                    texture = renderTargets.denoiserViewspaceZ;
                    break;
                case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
                    texture = renderTargets.denoiserSpecRadianceHitDist;
                    break;
                case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
                    texture = renderTargets.denoiserDiffRadianceHitDist;
                    break;
                case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
                    texture = renderTargets.denoiserOutSpecRadianceHitDist[pass];
                    break;
                case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
                    texture = renderTargets.denoiserOutDiffRadianceHitDist[pass];
                    break;
                case nrd::ResourceType::OUT_VALIDATION:
                    texture = renderTargets.denoiserOutValidation;
                    break;
                //case nrd::ResourceType::IN_DIFF_HIT:
                //    texture = renderTargets.DiffuseLighting;
                //    break;
                //case nrd::ResourceType::IN_SPEC_HIT:
                //    texture = renderTargets.SpecularLighting;
                //    break;
                //case nrd::ResourceType::OUT_DIFF_HIT:
                //    texture = renderTargets.DenoisedDiffuseLighting;
                //    break;
                //case nrd::ResourceType::OUT_SPEC_HIT:
                //    texture = renderTargets.DenoisedSpecularLighting;
                //    break;
                case nrd::ResourceType::IN_DISOCCLUSION_THRESHOLD_MIX:
                    texture = renderTargets.denoiserDisocclusionThresholdMix;
                    break;
                case nrd::ResourceType::TRANSIENT_POOL:
                    texture = m_transientTextures[resource.indexInPool];
                    break;
                case nrd::ResourceType::PERMANENT_POOL:
                    texture = m_permanentTextures[resource.indexInPool];
                    break;
                default:
                    assert(!"Unavailable resource type");
                    break;
                }

                assert(texture);

                caustica::rhi::TextureSubresourceSet subresources = caustica::rhi::AllSubresources;

                caustica::rhi::BindingSetItem setItem = caustica::rhi::BindingSetItem::None();
                setItem.resourceHandle = texture;
                setItem.slot = instanceDesc.resourcesBaseRegisterIndex + descriptorOffset;
                setItem.subresources = subresources;
                setItem.type = (nrdDescriptorRange.descriptorType == nrd::DescriptorType::TEXTURE)
                    ? caustica::rhi::ResourceType::Texture_SRV
                    : caustica::rhi::ResourceType::Texture_UAV;

                resourcesSetDesc.bindings.push_back(setItem);

                resourceIndex++;
            }
        }

        assert(resourceIndex == dispatchDesc.resourcesNum);

        const NrdPipeline& pipeline = m_pipelines[dispatchDesc.pipelineIndex];

        caustica::rhi::BindingSetHandle resourcesBindingSet = m_bindingCache.getOrCreateBindingSet(resourcesSetDesc, pipeline.ResourcesBindingLayout);
        caustica::rhi::BindingSetHandle constantsAndSamplersBindingSet = m_bindingCache.getOrCreateBindingSet(constantsAndSampelrsSetDesc, pipeline.ConstantsAndSamplersBindingLayout);

        caustica::rhi::ComputeState state;
        state.bindings = { resourcesBindingSet, constantsAndSamplersBindingSet };
        state.pipeline = pipeline.Pipeline;
        commandList->setComputeState(state);

        commandList->dispatch(dispatchDesc.gridWidth, dispatchDesc.gridHeight);

        if (dispatchDesc.name)
        {
            commandList->endMarker();
        }
    }
}

#endif
