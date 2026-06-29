#include <render/Passes/Lighting/Distant/EnvMapImportanceSamplingCache.h>

#include <assets/loader/ShaderFactory.h>
#include <render/Core/FramebufferFactory.h>
#include <render/Core/CommonRenderPasses.h>
#include <assets/loader/TextureLoader.h>
#include <render/Passes/Geometry/MipMapGenPass.h>

#include <rhi/utils.h>

#include <imgui/imgui_renderer.h>

#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <render/Core/ScopedPerfMarker.h>
#include <render/Core/TextureUtils.h>

using namespace caustica::math;
using namespace caustica;

EnvMapImportanceSamplingCache::EnvMapImportanceSamplingCache( nvrhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory )
    : m_device(device), m_shaderFactory(shaderFactory)
{
}

EnvMapImportanceSamplingCache::~EnvMapImportanceSamplingCache()
{
}

void EnvMapImportanceSamplingCache::CreateRenderPasses()
{
    // Samplers
    {
        nvrhi::SamplerDesc samplerDesc;
        samplerDesc.setBorderColor(nvrhi::Color(0.f));
        samplerDesc.setAllFilters(true);
        samplerDesc.setMipFilter(true);
        samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
        m_linearWrapSampler = m_device->createSampler(samplerDesc);

        samplerDesc.setAllFilters(false);
        samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
        m_pointClampSampler = m_device->createSampler(samplerDesc);
    }

    {
        nvrhi::BufferDesc constBufferDesc;
        constBufferDesc.byteSize = sizeof(EnvMapImportanceSamplingCacheConstants);
        constBufferDesc.debugName = "EnvMapImportanceSamplingCacheConstants";
        constBufferDesc.isConstantBuffer = true;
        constBufferDesc.isVolatile = true;
        constBufferDesc.maxVersions = 16;
        m_builderConstants = m_device->createBuffer(constBufferDesc);
    }   

    //Create importance map (for MIP descent) builder shader and resources
    {
        m_importanceMapComputeShader = m_shaderFactory->CreateShader("caustica/shaders/render/Lighting/Distant/EnvMapImportanceSamplingCache.hlsl", "BuildMIPDescentImportanceMapCS", nullptr, nvrhi::ShaderType::Compute);
        assert(m_importanceMapComputeShader);

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Compute;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Texture_UAV(0),
            nvrhi::BindingLayoutItem::Texture_UAV(1),
            nvrhi::BindingLayoutItem::Sampler(0),
            nvrhi::BindingLayoutItem::Sampler(1)
        };
        m_importanceMapBindingLayout = m_device->createBindingLayout(layoutDesc);

        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.setComputeShader(m_importanceMapComputeShader);
        pipelineDesc.addBindingLayout(m_importanceMapBindingLayout);
        m_importanceMapPipeline = m_device->createComputePipeline(pipelineDesc);
        
        m_importanceMapBindingSet = nullptr;
    }

    // Pre-sampling builder shader and resources
#if 0
    {
        // Stuff for presampling goes below
        m_presamplingCS = m_shaderFactory->CreateShader("caustica/shaders/render/Lighting/Distant/EnvMapImportanceSamplingCache.hlsl", "PreSampleCS", nullptr, nvrhi::ShaderType::Compute);
        assert(m_presamplingCS);

        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Compute;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Texture_SRV(1),
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(0),
            nvrhi::BindingLayoutItem::Sampler(0),
            nvrhi::BindingLayoutItem::Sampler(1)
        };
        m_presamplingBindingLayout = m_device->createBindingLayout(layoutDesc);

        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.setComputeShader(m_presamplingCS);
        pipelineDesc.addBindingLayout(m_presamplingBindingLayout);
        m_presamplingPipeline = m_device->createComputePipeline(pipelineDesc);

        // buffer that stores pre-generated samples which get updated once per frame
        nvrhi::BufferDesc buffDesc;
        buffDesc.byteSize = sizeof(uint32_t) * 2 * std::max(ENVMAP_PRESAMPLED_COUNT, 1u); // RG32_UINT (2 UINTs) per element
        buffDesc.format = nvrhi::Format::RG32_UINT;
        buffDesc.canHaveTypedViews = true;
        buffDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        buffDesc.keepInitialState = true;
        buffDesc.debugName = "PresampledEnvironmentSamples";
        buffDesc.canHaveUAVs = true;
        m_presampledBuffer = m_device->createBuffer(buffDesc);
        assert(m_presampledBuffer);

        m_presamplingBindingSet = nullptr;
    }
#endif

    memset( &m_envMapImportanceSamplingParams, 0, sizeof(m_envMapImportanceSamplingParams) );

    m_importanceMapTexture = nullptr;
    m_MIPMapPass = nullptr;
}

int EnvMapImportanceSamplingCache::GetImportanceMapResolution()
{
    return EMISB_IMPORTANCE_MAP_DIM;
}

int EnvMapImportanceSamplingCache::GetImportanceMapMIPLevels()
{
    const uint32_t dimensions = GetImportanceMapResolution();
    uint32_t mips = (uint32_t)log2(dimensions) + 1;
    assert((1u << (mips - 1)) == dimensions);
    assert(mips > 1 && mips <= 12);
    return mips;
}

void EnvMapImportanceSamplingCache::CreateImportanceMap()
{
    const uint32_t dimensions = GetImportanceMapResolution();
    const uint32_t samples = EMISB_IMPORTANCE_SAMPLES_PER_PIXEL;

    assert(ispow2(dimensions) && ispow2(samples));

    uint32_t mips = GetImportanceMapMIPLevels();

    nvrhi::TextureDesc texDesc;
    texDesc.format = nvrhi::Format::R32_FLOAT;
    texDesc.width = dimensions;
    texDesc.height = dimensions;
    texDesc.mipLevels = mips;
    texDesc.isRenderTarget = true;
    texDesc.isUAV = true;
    texDesc.debugName = "EnvImportanceMap";
    texDesc.setInitialState(nvrhi::ResourceStates::UnorderedAccess);
    texDesc.keepInitialState = true;
    m_importanceMapTexture = m_device->createTexture(texDesc);

    texDesc.format = nvrhi::Format::RGBA16_FLOAT;
    texDesc.debugName = "EnvRadianceMap";
    m_radianceMapTexture = m_device->createTexture(texDesc);

    m_MIPMapPass = std::make_unique<caustica::render::MipMapGenPass>(m_device, m_shaderFactory, m_importanceMapTexture, caustica::render::MipMapGenPass::MODE_COLOR);
    m_MIPMapPassRad = std::make_unique<caustica::render::MipMapGenPass>(m_device, m_shaderFactory, m_radianceMapTexture, caustica::render::MipMapGenPass::MODE_COLOR);
}

void EnvMapImportanceSamplingCache::FillCacheConsts(EnvMapImportanceSamplingCacheConstants & constants, nvrhi::TextureHandle sourceCubemap, int sampleIndex)
{
    const uint32_t dimensions = EMISB_IMPORTANCE_MAP_DIM;
    const uint32_t samples = EMISB_IMPORTANCE_SAMPLES_PER_PIXEL;
    uint32_t samplesX = std::max(1u, (uint32_t)std::sqrt(samples));
    uint32_t samplesY = samples / samplesX;

    constants.SourceCubeDim = sourceCubemap->getDesc().width;
    constants.SourceCubeMIPCount = sourceCubemap->getDesc().mipLevels;
    constants.ImportanceMapDim = dimensions;
    constants.ImportanceMapDimInSamples = uint2(dimensions * samplesX, dimensions * samplesY);
    constants.ImportanceMapNumSamples = uint2(samplesX, samplesY);
    constants.ImportanceMapInvSamples = 1.f / (samplesX * samplesY);
    constants.ImportanceMapBaseMip = m_importanceMapTexture->getDesc().mipLevels - 1;
    constants.SampleIndex = sampleIndex;
}

void EnvMapImportanceSamplingCache::PreUpdate(nvrhi::TextureHandle sourceCubemap, bool newSource)
{
    assert(sourceCubemap);

    if (m_importanceMapTexture == nullptr)
        CreateImportanceMap();

    if (m_importanceMapBindingSet == nullptr || newSource)
    {
        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_builderConstants),
            nvrhi::BindingSetItem::Texture_SRV(0, sourceCubemap),
            nvrhi::BindingSetItem::Texture_UAV(0, m_importanceMapTexture),
            nvrhi::BindingSetItem::Texture_UAV(1, m_radianceMapTexture),
            nvrhi::BindingSetItem::Sampler(0, m_pointClampSampler),
            nvrhi::BindingSetItem::Sampler(1, m_linearWrapSampler),
        };
        m_importanceMapBindingSet = m_device->createBindingSet(bindingSetDesc, m_importanceMapBindingLayout);
    }
}

void EnvMapImportanceSamplingCache::GenerateImportanceMap(nvrhi::CommandListHandle commandList, nvrhi::TextureHandle sourceCubemap)
{
    nvrhi::ComputeState state;
    state.pipeline = m_importanceMapPipeline;
    state.bindings = { m_importanceMapBindingSet };

    uint32_t groupCount = (EMISB_IMPORTANCE_MAP_DIM+EMISB_NUM_COMPUTE_THREADS_PER_DIM-1) / EMISB_NUM_COMPUTE_THREADS_PER_DIM;

    EnvMapImportanceSamplingCacheConstants constants;
    FillCacheConsts( constants, sourceCubemap, -1 );    // sampleIndex not relevant during importance map generation

    {
        RAII_SCOPE(commandList->beginMarker("GenIM"); , commandList->endMarker(); );
        commandList->writeBuffer(m_builderConstants, &constants, sizeof(EnvMapImportanceSamplingCacheConstants));
        commandList->setComputeState(state);
        commandList->dispatch(groupCount, groupCount);
    }

    {
        //RAII_SCOPE(commandList->beginMarker("GenMIPs");, commandList->endMarker(); );
        m_MIPMapPass->Dispatch(commandList);
        m_MIPMapPassRad->Dispatch(commandList);
    }

    commandList->setTextureState(m_importanceMapTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    m_envMapImportanceSamplingParams.ImportanceBaseMip = constants.ImportanceMapBaseMip;
    m_envMapImportanceSamplingParams.ImportanceInvDim = 1.f / float2((float)m_importanceMapTexture->getDesc().width, (float)m_importanceMapTexture->getDesc().height);
    m_envMapImportanceSamplingParams.padding0 = 0;
}

void EnvMapImportanceSamplingCache::Update(nvrhi::CommandListHandle commandList, nvrhi::TextureHandle sourceCubemap)
{
    RAII_SCOPE( commandList->beginMarker("ISBake");, commandList->endMarker(); );

    GenerateImportanceMap(commandList, sourceCubemap);
}

void EnvMapImportanceSamplingCache::ExecutePresampling(nvrhi::CommandListHandle commandList, nvrhi::TextureHandle sourceCubemap, int sampleIndex)
{
    assert( false );
#if 0
    assert(m_presampledBuffer);

    if (!m_presamplingBindingSet)
    {
        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_builderConstants),
            nvrhi::BindingSetItem::Texture_SRV(0, sourceCubemap),
            nvrhi::BindingSetItem::Texture_SRV(1, m_importanceMapTexture),
            nvrhi::BindingSetItem::TypedBuffer_UAV(0, m_presampledBuffer),
            nvrhi::BindingSetItem::Sampler(0, m_pointClampSampler),
            nvrhi::BindingSetItem::Sampler(1, m_linearWrapSampler),
        };
        m_presamplingBindingSet = m_device->createBindingSet(bindingSetDesc, m_presamplingBindingLayout);
    }

    EnvMapImportanceSamplingCacheConstants constants;
    FillCacheConsts(constants, sourceCubemap, sampleIndex);    // sampleIndex not relevant during importance map generation

    {
        RAII_SCOPE(commandList->beginMarker("Pre-sampling");, commandList->endMarker(); );
        commandList->writeBuffer(m_builderConstants, &constants, sizeof(EnvMapImportanceSamplingCacheConstants));

        nvrhi::ComputeState state;
        state.pipeline = m_presamplingPipeline;
        state.bindings = { m_presamplingBindingSet };
        commandList->setComputeState(state);
        static_assert((ENVMAP_PRESAMPLED_COUNT % 256) == 0);
        uint32_t groupCount = ENVMAP_PRESAMPLED_COUNT / 256;
        commandList->dispatch(groupCount, groupCount);
    }
#endif
}


bool EnvMapImportanceSamplingCache::DebugGUI(float indent)
{
    return false;
}
