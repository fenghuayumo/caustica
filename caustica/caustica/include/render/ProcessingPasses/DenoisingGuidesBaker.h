#pragma once

#include <memory>
#include <SampleCommon/ComputePass.h>
#include <math/math.h>
#include <render/BindingCache.h>

enum class DebugViewType;

namespace caustica
{
    class ShaderFactory;
}

class RenderTargets;
class ShaderDebug;

class DenoisingGuidesBaker
{
public:

    DenoisingGuidesBaker( nvrhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory, const std::unique_ptr<RenderTargets> & renderTargets, const std::shared_ptr<ShaderDebug> & shaderDebug, nvrhi::BindingLayoutHandle globalBindingLayout );
    ~DenoisingGuidesBaker( );

public:
    void                            DenoiseSpecHitT( nvrhi::ICommandList * commandList, nvrhi::BindingSetHandle bindingSet );
    void                            ComputeAvgLayerRadiance( nvrhi::ICommandList * commandList, nvrhi::BindingSetHandle bindingSet );
    void                            RenderDebugViz( nvrhi::ICommandList * commandList, DebugViewType debugView, nvrhi::BindingSetHandle bindingSet );

    bool                            DebugGUI(float indent);


private:
    const nvrhi::DeviceHandle       m_device;
    
    ComputePass                     m_csDenoiseSpecHitT;
    ComputePass                     m_csComputeAvgLayerRadiance;
    ComputePass                     m_csDebugViz;

    nvrhi::BindingLayoutHandle      m_bindingLayout;
    caustica::BindingCache     m_bindingCache;

    const std::unique_ptr<RenderTargets> & m_renderTargets;
    const std::shared_ptr<ShaderDebug> & m_shaderDebug;
};
    
