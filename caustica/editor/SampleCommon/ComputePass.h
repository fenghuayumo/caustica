#pragma once

#include <rhi/nvrhi.h>

namespace caustica
{
    class ShaderFactory;
    struct ShaderMacro;
}


class ComputePass
{
public:
    bool Init(
        nvrhi::IDevice* device,
        caustica::ShaderFactory& shaderFactory,
        const char* fileName,
        const char* entry, 
        const std::vector<caustica::ShaderMacro>& macros,
        nvrhi::IBindingLayout* bindingLayout,
        nvrhi::IBindingLayout* extraBindingLayout = nullptr,
        nvrhi::IBindingLayout* bindlessLayout = nullptr);

    bool Init(
        nvrhi::IDevice* device,
        caustica::ShaderFactory& shaderFactory,
        const char* fileName,
        const char* entry,
        const std::vector<caustica::ShaderMacro>& macros,
        nvrhi::BindingLayoutVector & bindingLayouts );

    void Execute(
        nvrhi::ICommandList* commandList,
        int width,
        int height,
        int depth,
        nvrhi::IBindingSet* bindingSet,
        nvrhi::IBindingSet* extraBindingSet = nullptr,
        nvrhi::IDescriptorTable* descriptorTable = nullptr,
        const void* pushConstants = nullptr,
        size_t pushConstantSize = 0);

    void Execute(
        nvrhi::ICommandList* commandList,
        int width,
        int height,
        int depth,
        const nvrhi::BindingSetVector & bindings);

private:
    nvrhi::ShaderHandle             m_computeShader;
    nvrhi::ComputePipelineHandle    m_computePipeline;
    nvrhi::ShaderLibraryHandle      m_shaderLibrary;
};