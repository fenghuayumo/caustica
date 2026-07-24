#pragma once

#include <rhi/rhi.h>

namespace caustica
{
    class ShaderFactory;
    struct ShaderMacro;
}


class ComputePass
{
public:
    bool init(
        caustica::rhi::Device* device,
        caustica::ShaderFactory& shaderFactory,
        const char* fileName,
        const char* entry, 
        const std::vector<caustica::ShaderMacro>& macros,
        caustica::rhi::BindingLayout* bindingLayout,
        caustica::rhi::BindingLayout* extraBindingLayout = nullptr,
        caustica::rhi::BindingLayout* bindlessLayout = nullptr);

    bool init(
        caustica::rhi::Device* device,
        caustica::ShaderFactory& shaderFactory,
        const char* fileName,
        const char* entry,
        const std::vector<caustica::ShaderMacro>& macros,
        caustica::rhi::BindingLayoutVector & bindingLayouts );

    void execute(
        caustica::rhi::CommandList* commandList,
        int width,
        int height,
        int depth,
        caustica::rhi::BindingSet* bindingSet,
        caustica::rhi::BindingSet* extraBindingSet = nullptr,
        caustica::rhi::DescriptorTable* descriptorTable = nullptr,
        const void* pushConstants = nullptr,
        size_t pushConstantSize = 0);

    void execute(
        caustica::rhi::CommandList* commandList,
        int width,
        int height,
        int depth,
        const caustica::rhi::BindingSetVector & bindings);

private:
    caustica::rhi::ShaderHandle             m_computeShader;
    caustica::rhi::ComputePipelineHandle    m_computePipeline;
    caustica::rhi::ShaderLibraryHandle      m_shaderLibrary;
};