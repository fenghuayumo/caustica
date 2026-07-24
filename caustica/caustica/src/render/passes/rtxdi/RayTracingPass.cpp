#include <render/passes/rtxdi/RayTracingPass.h>

#include <assets/loader/ShaderFactory.h>
#include <math/math.h>
#include <core/log.h>
#include <rhi/utils.h>

using namespace caustica;


bool RayTracingPass::init(
    caustica::rhi::Device* device,
    caustica::ShaderFactory& shaderFactory,
    const char* shaderName,
    const std::vector<caustica::ShaderMacro>& extraMacros,
    bool useRayQuery,
    uint32_t computeGroupSize,
    caustica::rhi::BindingLayout* bindingLayout,
    caustica::rhi::BindingLayout* extraBindingLayout,
    caustica::rhi::BindingLayout* bindlessLayout)
{
    caustica::debug("Initializing RayTracingPass %s...", shaderName);

    ComputeGroupSize = computeGroupSize;

    std::vector<caustica::ShaderMacro> macros = { { "USE_RAY_QUERY", "1" } };

    macros.insert(macros.end(), extraMacros.begin(), extraMacros.end());

    if (useRayQuery)
    {
#if CAUSTICA_RHI_D3D12_WITH_DXR12_OPACITY_MICROMAP
        if (device->getGraphicsAPI() == caustica::rhi::GraphicsAPI::D3D12)
            macros.push_back({ "CAUSTICA_RHI_D3D12_WITH_DXR12_OPACITY_MICROMAP", "1" });
#endif // CAUSTICA_RHI_D3D12_WITH_DXR12_OPACITY_MICROMAP

        ComputeShader = shaderFactory.createShader(shaderName, "main", &macros, caustica::rhi::ShaderType::Compute);
        if (!ComputeShader)
            return false;    

        caustica::rhi::ComputePipelineDesc pipelineDesc;
        if(extraBindingLayout)
            pipelineDesc.bindingLayouts.push_back(extraBindingLayout);
        if (bindlessLayout)
            pipelineDesc.bindingLayouts.push_back(bindlessLayout);
        if (bindingLayout)
            pipelineDesc.bindingLayouts.push_back(bindingLayout);
        pipelineDesc.CS = ComputeShader;
        ComputePipeline = device->createComputePipeline(pipelineDesc);

        if (!ComputePipeline)
            return false;

        return true;
    }

    macros[0].definition = "0"; // USE_RAY_QUERY
    ShaderLibrary = shaderFactory.createShaderLibrary(shaderName, &macros);
    if (!ShaderLibrary)
        return false;

    caustica::rhi::rt::PipelineDesc rtPipelineDesc;
    rtPipelineDesc.globalBindingLayouts = { extraBindingLayout, bindlessLayout };
    if (bindingLayout)
        rtPipelineDesc.globalBindingLayouts.push_back(bindingLayout);
    rtPipelineDesc.shaders = {
        { "", ShaderLibrary->getShader("RayGen", caustica::rhi::ShaderType::RayGeneration), nullptr },
        { "", ShaderLibrary->getShader("Miss", caustica::rhi::ShaderType::Miss), nullptr }
    };

    rtPipelineDesc.hitGroups = {
        {
            "HitGroup",
            ShaderLibrary->getShader("ClosestHit", caustica::rhi::ShaderType::ClosestHit),
            ShaderLibrary->getShader("AnyHit", caustica::rhi::ShaderType::AnyHit),
            nullptr, // intersectionShader
            nullptr, // localBindingLayout
            false // isProceduralPrimitive
        },
    };

    rtPipelineDesc.maxAttributeSize = 8;
    rtPipelineDesc.maxPayloadSize = 40;
    rtPipelineDesc.maxRecursionDepth = 1;

    RayTracingPipeline = device->createRayTracingPipeline(rtPipelineDesc);
    if (!RayTracingPipeline)
        return false;

    ShaderTable = RayTracingPipeline->createShaderTable();
    if (!ShaderTable)
        return false;

    ShaderTable->setRayGenerationShader("RayGen");
    ShaderTable->addMissShader("Miss");
    ShaderTable->addHitGroup("HitGroup");

    return true;
}

void RayTracingPass::execute(
    caustica::rhi::CommandList* commandList,
    int width,
    int height,
    caustica::rhi::BindingSet* bindingSet,
    caustica::rhi::BindingSet* extraBindingSet,
    caustica::rhi::DescriptorTable* descriptorTable,
    const void* pushConstants,
    const size_t pushConstantSize)
{
    if (ComputePipeline)
    {
        caustica::rhi::ComputeState state;
        state.bindings = { extraBindingSet };
        if (descriptorTable)
            state.bindings.push_back(descriptorTable);
        if (bindingSet)
            state.bindings.push_back(bindingSet);
        state.pipeline = ComputePipeline;
        commandList->setComputeState(state);

        if (pushConstants)
            commandList->setPushConstants(pushConstants, pushConstantSize);

        commandList->dispatch(dm::div_ceil(width, ComputeGroupSize), dm::div_ceil(height, ComputeGroupSize), 1);
    }
    else
    {
        caustica::rhi::rt::State state;
        state.bindings = { extraBindingSet };
        if (descriptorTable)
            state.bindings.push_back(descriptorTable);
        if (bindingSet)
            state.bindings.push_back(bindingSet);
        state.shaderTable = ShaderTable;
        commandList->setRayTracingState(state);

        if (pushConstants)
            commandList->setPushConstants(pushConstants, pushConstantSize);

        caustica::rhi::rt::DispatchRaysArguments args;
        args.width = width;
        args.height = height;
        args.depth = 1;
        commandList->dispatchRays(args);
    }
}
