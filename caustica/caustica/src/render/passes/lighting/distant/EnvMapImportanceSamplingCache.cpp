#include <render/passes/lighting/distant/EnvMapImportanceSamplingCache.h>

#include <assets/loader/ShaderFactory.h>
#include <render/core/FramebufferFactory.h>
#include <assets/loader/TextureLoader.h>
#include <render/passes/geometry/MipMapGenPass.h>

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
#include <render/core/ScopedPerfMarker.h>
#include <render/core/TextureUtils.h>

using namespace caustica::math;
using namespace caustica;

EnvMapImportanceSamplingCache::EnvMapImportanceSamplingCache( caustica::rhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory )
    : m_device(device), m_shaderFactory(shaderFactory)
{
}

EnvMapImportanceSamplingCache::~EnvMapImportanceSamplingCache()
{
}

void EnvMapImportanceSamplingCache::createRenderPasses()
{
    // Samplers
    {
        caustica::rhi::SamplerDesc samplerDesc;
        samplerDesc.setBorderColor(caustica::rhi::Color(0.f));
        samplerDesc.setAllFilters(true);
        samplerDesc.setMipFilter(true);
        samplerDesc.setAllAddressModes(caustica::rhi::SamplerAddressMode::Wrap);
        m_linearWrapSampler = m_device->createSampler(samplerDesc);

        samplerDesc.setAllFilters(false);
        samplerDesc.setAllAddressModes(caustica::rhi::SamplerAddressMode::Clamp);
        m_pointClampSampler = m_device->createSampler(samplerDesc);
    }

    {
        caustica::rhi::BufferDesc constBufferDesc;
        constBufferDesc.byteSize = sizeof(EnvMapImportanceSamplingCacheConstants);
        constBufferDesc.debugName = "EnvMapImportanceSamplingCacheConstants";
        constBufferDesc.isConstantBuffer = true;
        constBufferDesc.isVolatile = true;
        constBufferDesc.maxVersions = 16;
        m_builderConstants = m_device->createBuffer(constBufferDesc);
    }   

    //create importance map (for MIP descent) builder shader and resources
    {
        m_importanceMapComputeShader = m_shaderFactory->createShader("caustica/shaders/render/lighting/distant/EnvMapImportanceSamplingCache.hlsl", "BuildMIPDescentImportanceMapCS", nullptr, caustica::rhi::ShaderType::Compute);
        assert(m_importanceMapComputeShader);

        caustica::rhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = caustica::rhi::ShaderType::Compute;
        layoutDesc.bindings = {
            caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0),
            caustica::rhi::BindingLayoutItem::Texture_SRV(0),
            caustica::rhi::BindingLayoutItem::Texture_UAV(0),
            caustica::rhi::BindingLayoutItem::Texture_UAV(1),
            caustica::rhi::BindingLayoutItem::Sampler(0),
            caustica::rhi::BindingLayoutItem::Sampler(1)
        };
        m_importanceMapBindingLayout = m_device->createBindingLayout(layoutDesc);

        caustica::rhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.setComputeShader(m_importanceMapComputeShader);
        pipelineDesc.addBindingLayout(m_importanceMapBindingLayout);
        m_importanceMapPipeline = m_device->createComputePipeline(pipelineDesc);
        
        m_importanceMapBindingSet = nullptr;
    }

    // Pre-sampling builder shader and resources
#if 0
    {
        // Stuff for presampling goes below
        m_presamplingCS = m_shaderFactory->createShader("caustica/shaders/render/lighting/distant/EnvMapImportanceSamplingCache.hlsl", "PreSampleCS", nullptr, caustica::rhi::ShaderType::Compute);
        assert(m_presamplingCS);

        caustica::rhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = caustica::rhi::ShaderType::Compute;
        layoutDesc.bindings = {
            caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0),
            caustica::rhi::BindingLayoutItem::Texture_SRV(0),
            caustica::rhi::BindingLayoutItem::Texture_SRV(1),
            caustica::rhi::BindingLayoutItem::TypedBuffer_UAV(0),
            caustica::rhi::BindingLayoutItem::Sampler(0),
            caustica::rhi::BindingLayoutItem::Sampler(1)
        };
        m_presamplingBindingLayout = m_device->createBindingLayout(layoutDesc);

        caustica::rhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.setComputeShader(m_presamplingCS);
        pipelineDesc.addBindingLayout(m_presamplingBindingLayout);
        m_presamplingPipeline = m_device->createComputePipeline(pipelineDesc);

        // buffer that stores pre-generated samples which get updated once per frame
        caustica::rhi::BufferDesc buffDesc;
        buffDesc.byteSize = sizeof(uint32_t) * 2 * std::max(ENVMAP_PRESAMPLED_COUNT, 1u); // RG32_UINT (2 UINTs) per element
        buffDesc.format = caustica::rhi::Format::RG32_UINT;
        buffDesc.canHaveTypedViews = true;
        buffDesc.initialState = caustica::rhi::ResourceStates::ShaderResource;
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

int EnvMapImportanceSamplingCache::getImportanceMapResolution()
{
    return EMISB_IMPORTANCE_MAP_DIM;
}

int EnvMapImportanceSamplingCache::getImportanceMapMIPLevels()
{
    const uint32_t dimensions = getImportanceMapResolution();
    uint32_t mips = (uint32_t)log2(dimensions) + 1;
    assert((1u << (mips - 1)) == dimensions);
    assert(mips > 1 && mips <= 12);
    return mips;
}

void EnvMapImportanceSamplingCache::createImportanceMap()
{
    const uint32_t dimensions = getImportanceMapResolution();
    const uint32_t samples = EMISB_IMPORTANCE_SAMPLES_PER_PIXEL;

    assert(ispow2(dimensions) && ispow2(samples));

    uint32_t mips = getImportanceMapMIPLevels();

    caustica::rhi::TextureDesc texDesc;
    texDesc.format = caustica::rhi::Format::R32_FLOAT;
    texDesc.width = dimensions;
    texDesc.height = dimensions;
    texDesc.mipLevels = mips;
    texDesc.isRenderTarget = true;
    texDesc.isUAV = true;
    texDesc.debugName = "EnvImportanceMap";
    texDesc.setInitialState(caustica::rhi::ResourceStates::UnorderedAccess);
    texDesc.keepInitialState = true;
    m_importanceMapTexture = m_device->createTexture(texDesc);

    texDesc.format = caustica::rhi::Format::RGBA16_FLOAT;
    texDesc.debugName = "EnvRadianceMap";
    m_radianceMapTexture = m_device->createTexture(texDesc);

    m_MIPMapPass = std::make_unique<caustica::render::MipMapGenPass>(m_device, m_shaderFactory, m_importanceMapTexture, caustica::render::MipMapGenPass::MODE_COLOR);
    m_MIPMapPassRad = std::make_unique<caustica::render::MipMapGenPass>(m_device, m_shaderFactory, m_radianceMapTexture, caustica::render::MipMapGenPass::MODE_COLOR);
}

void EnvMapImportanceSamplingCache::fillCacheConsts(EnvMapImportanceSamplingCacheConstants & constants, caustica::rhi::TextureHandle sourceCubemap, int sampleIndex)
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

void EnvMapImportanceSamplingCache::preUpdate(caustica::rhi::TextureHandle sourceCubemap, bool newSource)
{
    assert(sourceCubemap);

    if (m_importanceMapTexture == nullptr)
        createImportanceMap();

    if (m_importanceMapBindingSet == nullptr || newSource)
    {
        caustica::rhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            caustica::rhi::BindingSetItem::ConstantBuffer(0, m_builderConstants),
            caustica::rhi::BindingSetItem::Texture_SRV(0, sourceCubemap),
            caustica::rhi::BindingSetItem::Texture_UAV(0, m_importanceMapTexture),
            caustica::rhi::BindingSetItem::Texture_UAV(1, m_radianceMapTexture),
            caustica::rhi::BindingSetItem::Sampler(0, m_pointClampSampler),
            caustica::rhi::BindingSetItem::Sampler(1, m_linearWrapSampler),
        };
        m_importanceMapBindingSet = m_device->createBindingSet(bindingSetDesc, m_importanceMapBindingLayout);
    }
}

void EnvMapImportanceSamplingCache::generateImportanceMap(caustica::rhi::CommandListHandle commandList, caustica::rhi::TextureHandle sourceCubemap)
{
    caustica::rhi::ComputeState state;
    state.pipeline = m_importanceMapPipeline;
    state.bindings = { m_importanceMapBindingSet };

    uint32_t groupCount = (EMISB_IMPORTANCE_MAP_DIM+EMISB_NUM_COMPUTE_THREADS_PER_DIM-1) / EMISB_NUM_COMPUTE_THREADS_PER_DIM;

    EnvMapImportanceSamplingCacheConstants constants;
    fillCacheConsts( constants, sourceCubemap, -1 );    // sampleIndex not relevant during importance map generation

    {
        RAII_SCOPE(commandList->beginMarker("GenIM"); , commandList->endMarker(); );
        commandList->writeBuffer(m_builderConstants, &constants, sizeof(EnvMapImportanceSamplingCacheConstants));
        commandList->setComputeState(state);
        commandList->dispatch(groupCount, groupCount);
    }

    {
        //RAII_SCOPE(commandList->beginMarker("GenMIPs");, commandList->endMarker(); );
        m_MIPMapPass->dispatch(commandList);
        m_MIPMapPassRad->dispatch(commandList);
    }

    commandList->setTextureState(m_importanceMapTexture, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    m_envMapImportanceSamplingParams.ImportanceBaseMip = constants.ImportanceMapBaseMip;
    m_envMapImportanceSamplingParams.ImportanceInvDim = 1.f / float2((float)m_importanceMapTexture->getDesc().width, (float)m_importanceMapTexture->getDesc().height);
    m_envMapImportanceSamplingParams.padding0 = 0;
}

void EnvMapImportanceSamplingCache::update(caustica::rhi::CommandListHandle commandList, caustica::rhi::TextureHandle sourceCubemap)
{
    RAII_SCOPE( commandList->beginMarker("ISBake");, commandList->endMarker(); );

    generateImportanceMap(commandList, sourceCubemap);
}

void EnvMapImportanceSamplingCache::executePresampling(caustica::rhi::CommandListHandle commandList, caustica::rhi::TextureHandle sourceCubemap, int sampleIndex)
{
    assert( false );
#if 0
    assert(m_presampledBuffer);

    if (!m_presamplingBindingSet)
    {
        caustica::rhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            caustica::rhi::BindingSetItem::ConstantBuffer(0, m_builderConstants),
            caustica::rhi::BindingSetItem::Texture_SRV(0, sourceCubemap),
            caustica::rhi::BindingSetItem::Texture_SRV(1, m_importanceMapTexture),
            caustica::rhi::BindingSetItem::TypedBuffer_UAV(0, m_presampledBuffer),
            caustica::rhi::BindingSetItem::Sampler(0, m_pointClampSampler),
            caustica::rhi::BindingSetItem::Sampler(1, m_linearWrapSampler),
        };
        m_presamplingBindingSet = m_device->createBindingSet(bindingSetDesc, m_presamplingBindingLayout);
    }

    EnvMapImportanceSamplingCacheConstants constants;
    fillCacheConsts(constants, sourceCubemap, sampleIndex);    // sampleIndex not relevant during importance map generation

    {
        RAII_SCOPE(commandList->beginMarker("Pre-sampling");, commandList->endMarker(); );
        commandList->writeBuffer(m_builderConstants, &constants, sizeof(EnvMapImportanceSamplingCacheConstants));

        caustica::rhi::ComputeState state;
        state.pipeline = m_presamplingPipeline;
        state.bindings = { m_presamplingBindingSet };
        commandList->setComputeState(state);
        static_assert((ENVMAP_PRESAMPLED_COUNT % 256) == 0);
        uint32_t groupCount = ENVMAP_PRESAMPLED_COUNT / 256;
        commandList->dispatch(groupCount, groupCount);
    }
#endif
}


bool EnvMapImportanceSamplingCache::debugGUI(float indent)
{
    return false;
}
