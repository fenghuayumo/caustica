#include <rhi/RenderDevice.h>

#include <render/Core/CommonRenderPasses.h>
#include <assets/loader/ShaderFactory.h>

namespace caustica::rhi
{

RenderDevice::RenderDevice(nvrhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory)
    : m_device(device)
    , m_builtins(std::make_unique<BuiltinTextures>(device))
    , m_samplers(std::make_unique<StandardSamplers>(device))
    , m_blit(std::make_unique<FullscreenBlitPass>(device, shaderFactory, *m_samplers))
    , m_commonPasses(std::make_shared<caustica::CommonRenderPasses>(*m_builtins, *m_samplers, *m_blit))
{
}

RenderDevice::~RenderDevice() = default;

caustica::CommonRenderPasses& RenderDevice::commonPasses()
{
    return *m_commonPasses;
}

const caustica::CommonRenderPasses& RenderDevice::commonPasses() const
{
    return *m_commonPasses;
}

} // namespace caustica::rhi
