#include <rhi/RenderDevice.h>

#include <render/Core/CommonRenderPasses.h>
#include <assets/loader/ShaderFactory.h>

namespace caustica::rhi
{

RenderDevice::RenderDevice(nvrhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory)
    : m_device(device)
    , m_commonPasses(std::make_shared<caustica::CommonRenderPasses>(device, std::move(shaderFactory)))
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
