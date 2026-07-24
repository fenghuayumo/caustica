#pragma once

#include <memory>
#include <render/core/ComputePass.h>
#include <math/math.h>
#include <render/core/BindingCache.h>

enum class DebugViewType;

namespace caustica
{
    class ShaderFactory;
}

class RenderTargets;
class ShaderDebug;

class DenoisingGuidesPass
{
public:

    DenoisingGuidesPass( caustica::rhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory, const std::unique_ptr<RenderTargets> & renderTargets, const std::shared_ptr<ShaderDebug> & shaderDebug, caustica::rhi::BindingLayoutHandle globalBindingLayout );
    ~DenoisingGuidesPass( );

public:
    void                            denoiseSpecHitT( caustica::rhi::ICommandList * commandList, caustica::rhi::BindingSetHandle bindingSet );
    void                            computeAvgLayerRadiance( caustica::rhi::ICommandList * commandList, caustica::rhi::BindingSetHandle bindingSet );
    void                            renderDebugViz( caustica::rhi::ICommandList * commandList, DebugViewType debugView, caustica::rhi::BindingSetHandle bindingSet );

    bool                            debugGUI(float indent);


private:
    const caustica::rhi::DeviceHandle       m_device;
    
    ComputePass                     m_csDenoiseSpecHitT;
    ComputePass                     m_csComputeAvgLayerRadiance;
    ComputePass                     m_csDebugViz;

    caustica::rhi::BindingLayoutHandle      m_bindingLayout;
    caustica::BindingCache     m_bindingCache;

    const std::unique_ptr<RenderTargets> & m_renderTargets;
    const std::shared_ptr<ShaderDebug> & m_shaderDebug;
};
    
