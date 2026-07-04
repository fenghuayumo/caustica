#include <render/Passes/Geometry/DeferredLightingPass.h>
#include <render/Core/GBuffer.h>
#include <render/Core/FramebufferFactory.h>
#include <assets/loader/ShaderFactory.h>
#include <render/Core/ShadowMap.h>
#include <scene/Scene.h>
#include <scene/SceneTypes.h>
#include <scene/SceneObjects.h>
#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>
#include <render/Core/RenderPassConstants.h>
#include <rhi/RenderDevice.h>
#include <scene/View.h>
#include <core/log.h>
#include <utility>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/passes/deferred_lighting_cs.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/passes/deferred_lighting_cs.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/passes/deferred_lighting_cs.spirv.h"
#endif
#endif

using namespace caustica::math;
#include <shaders/deferred_lighting_cb.h>

using namespace caustica;
using namespace caustica::render;

void DeferredLightingPass::Inputs::SetGBuffer(const GBufferRenderTargets& targets)
{
    depth = targets.Depth;
    gbufferNormals = targets.GBufferNormals;
    gbufferDiffuse = targets.GBufferDiffuse;
    gbufferSpecular = targets.GBufferSpecular;
    gbufferEmissive = targets.GBufferEmissive;
}

DeferredLightingPass::DeferredLightingPass(
    nvrhi::IDevice* device,
    rhi::RenderDevice& renderDevice)
    : m_Device(device)
    , m_BindingSets(device)
    , m_renderDevice(&renderDevice)
{
}

void caustica::render::DeferredLightingPass::Init(const std::shared_ptr<caustica::ShaderFactory>& shaderFactory)
{
    auto samplerDesc = nvrhi::SamplerDesc()
        .setAllAddressModes(nvrhi::SamplerAddressMode::Border)
        .setBorderColor(1.0f);
    m_ShadowSampler = m_Device->createSampler(samplerDesc);

    samplerDesc.setReductionType(nvrhi::SamplerReductionType::Comparison);
    m_ShadowSamplerComparison = m_Device->createSampler(samplerDesc);

    nvrhi::BufferDesc constantBufferDesc;
    constantBufferDesc.byteSize = sizeof(DeferredLightingConstants);
    constantBufferDesc.debugName = "DeferredLightingConstants";
    constantBufferDesc.isConstantBuffer = true;
    constantBufferDesc.isVolatile = true;
    constantBufferDesc.maxVersions = c_MaxRenderPassConstantBufferVersions;
    m_DeferredLightingCB = m_Device->createBuffer(constantBufferDesc);
    
    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::Compute;
        layoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0),
            nvrhi::BindingLayoutItem::Texture_SRV(1),
            nvrhi::BindingLayoutItem::Texture_SRV(2),
            nvrhi::BindingLayoutItem::Texture_SRV(3),
            nvrhi::BindingLayoutItem::Texture_SRV(8),
            nvrhi::BindingLayoutItem::Texture_SRV(9),
            nvrhi::BindingLayoutItem::Texture_SRV(10),
            nvrhi::BindingLayoutItem::Texture_SRV(11),
            nvrhi::BindingLayoutItem::Texture_SRV(12),
            nvrhi::BindingLayoutItem::Texture_SRV(14),
            nvrhi::BindingLayoutItem::Texture_SRV(15),
            nvrhi::BindingLayoutItem::Texture_SRV(16),
            nvrhi::BindingLayoutItem::Texture_SRV(17),
            nvrhi::BindingLayoutItem::Texture_UAV(0),
            nvrhi::BindingLayoutItem::Sampler(0),
            nvrhi::BindingLayoutItem::Sampler(1),
            nvrhi::BindingLayoutItem::Sampler(2),
            nvrhi::BindingLayoutItem::Sampler(3)
        };
        m_BindingLayout = m_Device->createBindingLayout(layoutDesc);
        
        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.CS = CreateComputeShader(*shaderFactory);
        pipelineDesc.bindingLayouts = { m_BindingLayout };
        
        m_Pso = m_Device->createComputePipeline(pipelineDesc);
    }
}

nvrhi::ShaderHandle DeferredLightingPass::CreateComputeShader(ShaderFactory& shaderFactory)
{
    return shaderFactory.CreateAutoShader("engine/passes/deferred_lighting_cs.hlsl", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_deferred_lighting_cs), nullptr, nvrhi::ShaderType::Compute);
}

void DeferredLightingPass::Render(
    nvrhi::ICommandList* commandList,
    const ICompositeView& compositeView,
    const Inputs& inputs,
    dm::float2 randomOffset)
{
    assert(inputs.depth);
    assert(inputs.gbufferNormals);
    assert(inputs.gbufferDiffuse);
    assert(inputs.gbufferSpecular);
    assert(inputs.gbufferEmissive);
    assert(inputs.output);

    commandList->beginMarker("DeferredLighting");

    DeferredLightingConstants deferredConstants = {};
    deferredConstants.randomOffset = randomOffset;
    deferredConstants.noisePattern[0] = float4(0.059f, 0.529f, 0.176f, 0.647f);
    deferredConstants.noisePattern[1] = float4(0.765f, 0.294f, 0.882f, 0.412f);
    deferredConstants.noisePattern[2] = float4(0.235f, 0.706f, 0.118f, 0.588f);
    deferredConstants.noisePattern[3] = float4(0.941f, 0.471f, 0.824f, 0.353f);
    deferredConstants.ambientColorTop = float4(inputs.ambientColorTop, 0.f);
    deferredConstants.ambientColorBottom = float4(inputs.ambientColorBottom, 0.f);
    deferredConstants.enableAmbientOcclusion = (inputs.ambientOcclusion != nullptr);
    deferredConstants.indirectDiffuseScale = 1.f;
    deferredConstants.indirectSpecularScale = 1.f;

    nvrhi::ITexture* shadowMapTexture = nullptr;

    int numShadows = 0;

    if (inputs.scene)
    {
        const auto* ew = inputs.scene->GetEntityWorld();
        if (ew)
        {
            for (ecs::Entity entity : inputs.scene->GetLightEntities())
            {
                const auto* lightComp = ew->world().get<scene::LightComponent>(entity);
                if (!lightComp) continue;
                const auto* globalComp = ew->world().get<scene::GlobalTransformComponent>(entity);
                if (!globalComp) continue;

                if (lightComp->shadowMap)
                {
                    if (!shadowMapTexture)
                    {
                        shadowMapTexture = lightComp->shadowMap->GetTexture();
                        deferredConstants.shadowMapTextureSize = float2(lightComp->shadowMap->GetTextureSize());
                    }
                    else
                    {
                        if (shadowMapTexture != lightComp->shadowMap->GetTexture())
                        {
                            caustica::error("All lights submitted to DeferredLightingPass::Render(...) must use the same shadow map textures");
                            return;
                        }
                    }
                }

                if (deferredConstants.numLights >= DEFERRED_MAX_LIGHTS)
                {
                    caustica::warning("Maximum number of active lights (%d) exceeded in DeferredLightingPass",
                        DEFERRED_MAX_LIGHTS);
                    break;
                }

                LightConstants& lightConstants = deferredConstants.lights[deferredConstants.numLights];
                scene::FillLightConstants(*lightComp, globalComp->transform, lightConstants);

                if (lightComp->shadowMap)
                {
                    for (uint32_t cascade = 0; cascade < lightComp->shadowMap->GetNumberOfCascades(); cascade++)
                    {
                        if (numShadows < DEFERRED_MAX_SHADOWS)
                        {
                            lightComp->shadowMap->GetCascade(cascade)->FillShadowConstants(deferredConstants.shadows[numShadows]);
                            lightConstants.shadowCascades[cascade] = numShadows;
                            ++numShadows;
                        }
                    }

                    for (uint32_t perObjectShadow = 0; perObjectShadow < lightComp->shadowMap->GetNumberOfPerObjectShadows(); perObjectShadow++)
                    {
                        if (numShadows < DEFERRED_MAX_SHADOWS)
                        {
                            lightComp->shadowMap->GetPerObjectShadow(perObjectShadow)->FillShadowConstants(deferredConstants.shadows[numShadows]);
                            lightConstants.perObjectShadows[perObjectShadow] = numShadows;
                            ++numShadows;
                        }
                    }
                }

                ++deferredConstants.numLights;
            }
        }
    }

    nvrhi::ITexture* lightProbeDiffuse = nullptr;
    nvrhi::ITexture* lightProbeSpecular = nullptr;
    nvrhi::ITexture* lightProbeEnvironmentBrdf = nullptr;

    if (inputs.lightProbes)
    {
        for (const auto& probe : *inputs.lightProbes)
        {
            if (!probe->IsActive())
                continue;

            LightProbeConstants& lightProbeConstants = deferredConstants.lightProbes[deferredConstants.numLightProbes];
            probe->FillLightProbeConstants(lightProbeConstants);

            ++deferredConstants.numLightProbes;

            if (deferredConstants.numLightProbes >= DEFERRED_MAX_LIGHT_PROBES)
            {
                caustica::warning("Maximum number of active light probes (%d) exceeded in DeferredLightingPass",
                    DEFERRED_MAX_LIGHT_PROBES);
                break;
            }

            if (lightProbeDiffuse == nullptr || lightProbeSpecular == nullptr || lightProbeEnvironmentBrdf == nullptr)
            {
                lightProbeDiffuse = probe->diffuseMap;
                lightProbeSpecular = probe->specularMap;
                lightProbeEnvironmentBrdf = probe->environmentBrdf;
            }
            else
            {
                if (lightProbeDiffuse != probe->diffuseMap || lightProbeSpecular != probe->specularMap || lightProbeEnvironmentBrdf != probe->environmentBrdf)
                {
                    caustica::error("All light probes submitted to DeferredLightingPass::Render(...) must use the same set of textures");
                    return;
                }
            }
        }
    }


    for (uint viewIndex = 0; viewIndex < compositeView.GetNumChildViews(ViewType::PLANAR); viewIndex++)
    {
        const IView* view = compositeView.GetChildView(ViewType::PLANAR, viewIndex);
        auto viewSubresources = view->GetSubresources();

        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_DeferredLightingCB),
            nvrhi::BindingSetItem::Texture_SRV(0, shadowMapTexture ? shadowMapTexture : m_renderDevice->builtins().blackDepthStencilTexture2DArray().Get()),
            nvrhi::BindingSetItem::Texture_SRV(1, lightProbeDiffuse ? lightProbeDiffuse : m_renderDevice->builtins().blackCubeMapArray().Get()),
            nvrhi::BindingSetItem::Texture_SRV(2, lightProbeSpecular ? lightProbeSpecular : m_renderDevice->builtins().blackCubeMapArray().Get()),
            nvrhi::BindingSetItem::Texture_SRV(3, lightProbeEnvironmentBrdf ? lightProbeEnvironmentBrdf : m_renderDevice->builtins().blackTexture().Get()),
            nvrhi::BindingSetItem::Texture_SRV(8, inputs.depth, nvrhi::Format::UNKNOWN, viewSubresources),
            nvrhi::BindingSetItem::Texture_SRV(9, inputs.gbufferDiffuse, nvrhi::Format::UNKNOWN, viewSubresources),
            nvrhi::BindingSetItem::Texture_SRV(10, inputs.gbufferSpecular, nvrhi::Format::UNKNOWN, viewSubresources),
            nvrhi::BindingSetItem::Texture_SRV(11, inputs.gbufferNormals, nvrhi::Format::UNKNOWN, viewSubresources),
            nvrhi::BindingSetItem::Texture_SRV(12, inputs.gbufferEmissive, nvrhi::Format::UNKNOWN, viewSubresources),
            nvrhi::BindingSetItem::Texture_SRV(14, inputs.indirectDiffuse ? inputs.indirectDiffuse : m_renderDevice->builtins().blackTexture().Get(), nvrhi::Format::UNKNOWN, viewSubresources),
            nvrhi::BindingSetItem::Texture_SRV(15, inputs.indirectSpecular ? inputs.indirectSpecular : m_renderDevice->builtins().blackTexture().Get(), nvrhi::Format::UNKNOWN, viewSubresources),
            nvrhi::BindingSetItem::Texture_SRV(16, inputs.shadowChannels ? inputs.shadowChannels : m_renderDevice->builtins().blackTexture().Get()),
            nvrhi::BindingSetItem::Texture_SRV(17, inputs.ambientOcclusion ? inputs.ambientOcclusion : m_renderDevice->builtins().whiteTexture().Get(), nvrhi::Format::UNKNOWN, viewSubresources),
            nvrhi::BindingSetItem::Texture_UAV(0, inputs.output, nvrhi::Format::UNKNOWN, viewSubresources),
            nvrhi::BindingSetItem::Sampler(0, m_ShadowSampler),
            nvrhi::BindingSetItem::Sampler(1, m_ShadowSamplerComparison),
            nvrhi::BindingSetItem::Sampler(2, m_renderDevice->samplers().linearWrap()),
            nvrhi::BindingSetItem::Sampler(3, m_renderDevice->samplers().linearClamp())
        };

        nvrhi::BindingSetHandle bindingSet = m_BindingSets.GetOrCreateBindingSet(bindingSetDesc, m_BindingLayout);
    
        view->FillPlanarViewConstants(deferredConstants.view);
        commandList->writeBuffer(m_DeferredLightingCB, &deferredConstants, sizeof(deferredConstants));

        nvrhi::ComputeState state;
        state.pipeline = m_Pso;
        state.bindings = { bindingSet };
        commandList->setComputeState(state);

        auto viewExtent = view->GetViewExtent();
        commandList->dispatch(
            dm::div_ceil(viewExtent.width(), 16),
            dm::div_ceil(viewExtent.height(), 16));
    }

    commandList->endMarker();
}

void DeferredLightingPass::ResetBindingCache()
{
    m_BindingSets.Clear();
}
