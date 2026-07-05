#pragma once


#include <rhi/nvrhi.h>
#include <memory>

namespace caustica::render
{
class RenderDevice;
}

namespace caustica
{
    class ShaderFactory;
    class FramebufferFactory;
    class ICompositeView;
}


namespace caustica::render
{
    class EnvironmentMapPass
    {
    private:
        nvrhi::ShaderHandle m_PixelShader;
        nvrhi::BufferHandle m_SkyCB;
        nvrhi::BindingLayoutHandle m_RenderBindingLayout;
        nvrhi::BindingSetHandle m_RenderBindingSet;
        nvrhi::GraphicsPipelineHandle m_RenderPso;

        caustica::render::RenderDevice* m_renderDevice = nullptr;
        std::shared_ptr<caustica::FramebufferFactory> m_FramebufferFactory;

    public:
        EnvironmentMapPass(
            nvrhi::IDevice* device,
            std::shared_ptr<caustica::ShaderFactory> shaderFactory,
            caustica::render::RenderDevice& renderDevice,
            std::shared_ptr<caustica::FramebufferFactory> framebufferFactory,
            const caustica::ICompositeView& compositeView,
            nvrhi::ITexture* environmentMap);

        void Render(
            nvrhi::ICommandList* commandList,
            const caustica::ICompositeView& compositeView);
    };

}