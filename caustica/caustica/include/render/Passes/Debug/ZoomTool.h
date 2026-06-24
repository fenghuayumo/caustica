#pragma once

#include <memory>
#include <render/Core/ComputePass.h>
#include <math/math.h>
#include <render/Core/BindingCache.h>

namespace caustica
{
    class ShaderFactory;
}

class ZoomTool
{
public:
    struct ZoomSettings
    {
        bool                Enabled     = false;
        int                 ZoomFactor  = 4;
        caustica::math::int2   BoxPos      = { 400, 300 };
        caustica::math::int2   BoxSize     = { 128, 96 };
    };

    ZoomTool( nvrhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory );
    ~ZoomTool( );

public:
    ZoomSettings &                  Settings( )         { return m_settings; }

     void                           Render( nvrhi::ICommandList * commandList, nvrhi::TextureHandle colorInOut );

     bool                           KeyboardUpdate(int key, int scancode, int action, int mods);
     void                           MousePosUpdate(double xpos, double ypos);
     bool                           MouseButtonUpdate(int button, int action, int mods);

     bool                           Enabled( ) const    { return m_settings.Enabled; }

     bool                           DebugGUI(float indent);

private:
    const nvrhi::DeviceHandle       m_device;
    
    ZoomSettings                    m_settings;
    nvrhi::BufferHandle             m_constantBuffer;
    ComputePass                     m_CSZoomTool;

    nvrhi::BindingLayoutHandle      m_bindingLayout;
    caustica::BindingCache     m_bindingCache;

    caustica::math::float2             m_lastMousePos;
};
    
