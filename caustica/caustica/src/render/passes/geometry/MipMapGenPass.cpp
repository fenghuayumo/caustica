#include <render/passes/geometry/MipMapGenPass.h>
#include <assets/loader/ShaderFactory.h>
#include <render/core/RenderPassConstants.h>
#include <render/core/RenderDevice.h>
#include <render/core/FullscreenBlitPass.h>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/passes/mipmapgen_cs.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/passes/mipmapgen_cs.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/passes/mipmapgen_cs.spirv.h"
#endif
#endif

using namespace caustica::math;
#include <shaders/mipmapgen_cb.h>

#include <cassert>
#include <mutex>

using namespace caustica;
using namespace caustica::render;

// The compute shader reduces 'NUM_LODS' mip-levels at a time into an
// array of NUM_LODS bound UAVs. For textures that have a number
// of mip levels that is not a multiple of NUM_LODS, we need to bind
// "something" to the UAV slots : a set of static dummy NullTextures.
//
// The set of NullTextures is shared by all the MipMapGen compute pass 
// instances and ownership is thread-safe.
//

static caustica::rhi::TextureHandle createNullTexture(caustica::rhi::DeviceHandle device)
{
    caustica::rhi::TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.isRenderTarget = false;
    desc.useClearValue = false;
    desc.sampleCount = 1;
    desc.dimension = caustica::rhi::TextureDimension::Texture2D;
    desc.initialState = caustica::rhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    desc.arraySize = 1;
    desc.isUAV = true;
    desc.format = caustica::rhi::Format::RGBA8_UNORM;

    return device->createTexture(desc);
}

struct MipMapGenPass::NullTextures {

    caustica::rhi::TextureHandle lod[NUM_LODS];

    static std::shared_ptr<NullTextures> get(caustica::rhi::DeviceHandle device)
    {
        static std::mutex _mutex;
        static std::weak_ptr<NullTextures> _nullTextures;

        std::lock_guard<std::mutex> lock(_mutex);

        std::shared_ptr<NullTextures> result = _nullTextures.lock();
        if (!result)
        {
            result = std::make_shared<NullTextures>();
            for (int i = 0; i < NUM_LODS; ++i)
                result->lod[i] = createNullTexture(device);
            _nullTextures = result;
        }
        return result;
    }
};

MipMapGenPass::MipMapGenPass(
    caustica::rhi::IDevice* device,
    std::shared_ptr<ShaderFactory> shaderFactory,
    caustica::rhi::TextureHandle input, 
    Mode mode)
    : m_device(device)
    , m_Texture(input)
    , m_BindingSets(MAX_PASSES)
    , m_BindingCache(device)
{
    assert(m_Texture);

    m_NullTextures = NullTextures::get(m_device);

    uint nmipLevels = m_Texture->getDesc().mipLevels;

    // Shader
    assert(mode>=0 && mode <= MODE_MINMAX);

    std::vector<ShaderMacro> macros = { {"MODE", std::to_string(mode)} };
    m_Shader = shaderFactory->createAutoShader(
        "engine/passes/mipmapgen_cs.hlsl", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_mipmapgen_cs), &macros, caustica::rhi::ShaderType::Compute);

    // Constants
    caustica::rhi::BufferDesc constantBufferDesc;
    constantBufferDesc.byteSize = sizeof(MipmmapGenConstants);
    constantBufferDesc.isConstantBuffer = true;
    constantBufferDesc.isVolatile = true;
    constantBufferDesc.debugName = "MipMapGenPass/Constants";
    constantBufferDesc.maxVersions = c_MaxRenderPassConstantBufferVersions;
    m_ConstantBuffer = m_device->createBuffer(constantBufferDesc);

    // BindingLayout
    caustica::rhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = caustica::rhi::ShaderType::Compute;
    layoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0));
    layoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_SRV(0));
    layoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_UAV(0).setSize(NUM_LODS));
    m_BindingLayout = m_device->createBindingLayout(layoutDesc);

    // BindingSets
    m_BindingSets.resize(MAX_PASSES);
    caustica::rhi::BindingSetDesc setDesc;
    for (uint i = 0; i < (uint)m_BindingSets.size(); ++i)
    {
        // create a unique binding set for each compute pass
        if (i * NUM_LODS >= nmipLevels)
            break;

        caustica::rhi::BindingSetHandle & set = m_BindingSets[i];

        caustica::rhi::BindingSetDesc setDesc;
        setDesc.bindings.push_back(caustica::rhi::BindingSetItem::ConstantBuffer(0, m_ConstantBuffer));
        setDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_SRV(0, m_Texture, caustica::rhi::Format::UNKNOWN, caustica::rhi::TextureSubresourceSet(i*NUM_LODS, 1, 0, 1)));
        for (uint mipLevel = 1; mipLevel <= NUM_LODS; ++mipLevel)
        {   // output UAVs start after the mip-level UAV that was computed last
            if (i * NUM_LODS + mipLevel < nmipLevels)
            {
                setDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_UAV(0, m_Texture)
                    .setArrayElement(mipLevel - 1)
                    .setSubresources(caustica::rhi::TextureSubresourceSet(i*NUM_LODS + mipLevel, 1, 0, 1)));
            }
            else
            {
                setDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_UAV(0, m_NullTextures->lod[mipLevel-1])
                    .setArrayElement(mipLevel - 1));
            }
        }
        set = m_device->createBindingSet(setDesc, m_BindingLayout);
    }

    caustica::rhi::ComputePipelineDesc computePipelineDesc;
    computePipelineDesc.CS = m_Shader;
    computePipelineDesc.bindingLayouts = { m_BindingLayout };

    m_Pso = device->createComputePipeline(computePipelineDesc);
}

void MipMapGenPass::dispatch(caustica::rhi::ICommandList* commandList, int maxLOD) 
{
    assert(m_Texture);

    commandList->beginMarker("MipMapGen::dispatch");

    uint nmipLevels = m_Texture->getDesc().mipLevels;
    if (maxLOD > 0 && maxLOD < (int)nmipLevels)
        nmipLevels = maxLOD+1;

    uint width = m_Texture->getDesc().width,
         height = m_Texture->getDesc().height;

    width = (width + GROUP_SIZE - 1) / GROUP_SIZE;
    height = (height + GROUP_SIZE - 1) / GROUP_SIZE;
 
    for (uint i=0; i < MAX_PASSES; ++i)
    {
        if (i * NUM_LODS >= nmipLevels)
            break;

        MipmmapGenConstants constants = {};
        constants.numLODs = std::min(nmipLevels - i*NUM_LODS -1, (uint32_t)NUM_LODS);
        constants.dispatch = i;
        commandList->writeBuffer(m_ConstantBuffer, &constants, sizeof(constants));

        caustica::rhi::ComputeState state;
        state.pipeline = m_Pso;
        state.bindings = { m_BindingSets[i] };
        commandList->setComputeState(state);
        commandList->dispatch(width, height);
    }

    commandList->endMarker(); // "MipMapGen::dispatch"
}


void MipMapGenPass::display(caustica::render::RenderDevice& renderDevice, caustica::rhi::ICommandList* commandList, caustica::rhi::IFramebuffer* target)
{
    assert(m_Texture);
    
    commandList->beginMarker("MipMapGen::display");
    
    caustica::rhi::Viewport viewport = caustica::rhi::Viewport((float)target->getFramebufferInfo().width, (float)target->getFramebufferInfo().height);

    float2 size = { m_Texture->getDesc().width / 2.f, m_Texture->getDesc().height / 2.f };
    float2 corner = { 10.f, uint(viewport.maxY) - 10.f };
 
    for (uint level = 0; level < m_Texture->getDesc().mipLevels-1; ++level)
    {
        caustica::render::BlitParameters blitParams;
        blitParams.targetFramebuffer = target;
        blitParams.sourceTexture = m_Texture;
        blitParams.sourceMip = level + 1;
        blitParams.targetViewport = caustica::rhi::Viewport(
            corner.x,
            corner.x + size.x,
            corner.y - size.y,
            corner.y, 0.f, 1.f
        );

        renderDevice.blit().blitTexture(commandList, blitParams, &m_BindingCache);

        // spiral pattern
        switch (level % 4)
        {
            case 0: corner += float2(size.x + 10.f, 0.f); break;
            case 1: corner += float2(size.x / 2.f, -(size.y + 10.f)); break;
            case 2: corner += float2(-size.x / 2.f - 10.f, -size.y / 2.f); break;
            case 3: corner += float2(0.f, size.y); break;
        }
        size = { size.x / 2.f, size.y / 2.f };
    }
    commandList->endMarker(); // "MipMapGen::display"
}