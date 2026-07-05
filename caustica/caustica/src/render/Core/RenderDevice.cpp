#include <render/Core/RenderDevice.h>

#include <assets/loader/ShaderFactory.h>

namespace caustica::render
{

RenderDevice::RenderDevice(nvrhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory)
    : m_device(device)
    , m_builtins(std::make_unique<BuiltinTextures>(device))
    , m_samplers(std::make_unique<StandardSamplers>(device))
    , m_blit(std::make_unique<FullscreenBlitPass>(device, shaderFactory, *m_samplers))
{
}

RenderDevice::~RenderDevice() = default;

} // namespace caustica::render
