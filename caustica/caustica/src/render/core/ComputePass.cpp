#include <render/core/ComputePass.h>

#include <assets/loader/ShaderFactory.h>
#include <math/math.h>
#include <core/log.h>
#include <rhi/utils.h>

using namespace caustica;


bool ComputePass::init(
	caustica::rhi::Device* device, 
	caustica::ShaderFactory& shaderFactory, 
	const char* fileName, 
    const char* entry, 
	const std::vector<caustica::ShaderMacro>& macros, 
	caustica::rhi::BindingLayout* bindingLayout,
	caustica::rhi::BindingLayout* extraBindingLayout /*= nullptr*/, 
	caustica::rhi::BindingLayout* bindlessLayout /*= nullptr*/)
{
	m_computeShader = shaderFactory.createShader(fileName, entry, &macros, caustica::rhi::ShaderType::Compute);
	if (!m_computeShader)
		return false;

	caustica::rhi::ComputePipelineDesc pipelineDesc;
	if(extraBindingLayout)
		pipelineDesc.bindingLayouts.push_back(extraBindingLayout);
	if (bindlessLayout)
		pipelineDesc.bindingLayouts.push_back(bindlessLayout);
	if (bindingLayout)
		pipelineDesc.bindingLayouts.push_back(bindingLayout);
	pipelineDesc.CS = m_computeShader;
	m_computePipeline = device->createComputePipeline(pipelineDesc);

	if (!m_computePipeline)
		return false;

	return true;
}

bool ComputePass::init(
    caustica::rhi::Device* device,
    caustica::ShaderFactory& shaderFactory,
    const char* fileName,
    const char* entry,
    const std::vector<caustica::ShaderMacro>& macros,
    caustica::rhi::BindingLayoutVector & bindingLayouts )
{
    m_computeShader = shaderFactory.createShader(fileName, entry, &macros, caustica::rhi::ShaderType::Compute);
    if (!m_computeShader)
        return false;

    caustica::rhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = bindingLayouts;
    pipelineDesc.CS = m_computeShader;
    m_computePipeline = device->createComputePipeline(pipelineDesc);

    if (!m_computePipeline)
        return false;

    return true;
}


void ComputePass::execute(
	caustica::rhi::CommandList* commandList, 
	int width, 
	int height, 
	int depth, 
	caustica::rhi::BindingSet* bindingSet, 
	caustica::rhi::BindingSet* extraBindingSet /*= nullptr*/, 
	caustica::rhi::DescriptorTable* descriptorTable /*= nullptr*/, 
	const void* pushConstants /*= nullptr*/, 
	size_t pushConstantSize /*= 0*/)
{
	caustica::rhi::ComputeState state;
	state.bindings;
    if (extraBindingSet)
        state.bindings.push_back(extraBindingSet);
	if (descriptorTable)
		state.bindings.push_back(descriptorTable);
	if (bindingSet)
		state.bindings.push_back(bindingSet);
	state.pipeline = m_computePipeline;
	commandList->setComputeState(state);

	if (pushConstants)
		commandList->setPushConstants(pushConstants, pushConstantSize);

	commandList->dispatch(width, height, depth);
}

void ComputePass::execute(
    caustica::rhi::CommandList* commandList,
    int width,
    int height,
    int depth,
    const caustica::rhi::BindingSetVector & bindings)
{
    caustica::rhi::ComputeState state;
    state.bindings = bindings;
    state.pipeline = m_computePipeline;
    commandList->setComputeState(state);

    commandList->dispatch(width, height, depth);
}
