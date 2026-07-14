#pragma once

#include <memory>
#include <render/core/ComputePass.h>
#include <math/math.h>
#include <render/core/BindingCache.h>

namespace caustica
{
    class ShaderFactory;
}

class ZoomTool
{
public:
    struct ZoomSettings
    {
        bool                enabled     = false;
        int                 ZoomFactor  = 4;
        caustica::math::int2   BoxPos      = { 400, 300 };
        caustica::math::int2   BoxSize     = { 128, 96 };
    };

    ZoomTool( nvrhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory );
    ~ZoomTool( );

public:
    ZoomSettings &                  settings( )         { return m_settings; }

     void                           render( nvrhi::ICommandList * commandList, nvrhi::TextureHandle colorInOut );

     bool                           keyboardUpdate(int key, int scancode, int action, int mods);
     void                           mousePosUpdate(double xpos, double ypos);
     bool                           mouseButtonUpdate(int button, int action, int mods);

     bool                           enabled( ) const    { return m_settings.enabled; }

     bool                           debugGUI(float indent);

private:
    const nvrhi::DeviceHandle       m_device;
    
    ZoomSettings                    m_settings;
    nvrhi::BufferHandle             m_constantBuffer;
    ComputePass                     m_CSZoomTool;

    nvrhi::BindingLayoutHandle      m_bindingLayout;
    caustica::BindingCache     m_bindingCache;

    caustica::math::float2             m_lastMousePos;
};
    
