/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// See https://github.com/fstrugar/XeGTAO/blob/master/Source/Rendering/Misc/vaZoomTool.cpp
// Copyright (C) 2016-2021, Intel Corporation, 
// SPDX-License-Identifier: MIT, Author(s):  Filip Strugar (filip.strugar@intel.com)
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#pragma once

#include <memory>
#include <SampleCommon/ComputePass.h>
#include <math/math.h>
#include <engine/BindingCache.h>

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
    
