#pragma once

#include <rhi/rhi.h>
#include <memory>
#include <math/math.h>

#include <shaders/Libraries/ShaderDebug/ShaderDebug.hlsl>
#include <render/core/RenderDevice.h>

// HOW TO USE
//
// 1.) In your render pass C++ side, #include "../../ShaderDebug.h"
// 2.) You also need access to the global shared_ptr<ShaderDebug> 
// 3.) Add " caustica::rhi::BindingLayoutItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX), " to your binding layout(s) and
// 4.) Add " caustica::rhi::BindingSetItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX, m_shaderPrintf->getGPUWriteBuffer()), " to your binding set(s)
// 5.) 

class ShaderDebug
{
private:
    caustica::rhi::DeviceHandle             m_device;

    static constexpr int            c_swapchainCount = 3;
    caustica::rhi::BufferHandle             m_bufferGPU;                    // write
    caustica::rhi::BufferHandle             m_indirectDrawBufferGPU;
    caustica::rhi::BufferHandle             m_bufferCPU[c_swapchainCount];  // readback
    unsigned int                    m_currentBufferIndex = 0;

    caustica::rhi::ShaderHandle             m_trianglesVertexShader;
    caustica::rhi::ShaderHandle             m_trianglesPixelShader;
    caustica::rhi::GraphicsPipelineHandle   m_trianglesPipeline;
    caustica::rhi::ShaderHandle             m_linesVertexShader;
    caustica::rhi::ShaderHandle             m_linesPixelShader;
    caustica::rhi::GraphicsPipelineHandle   m_linesPipeline;
    caustica::rhi::BindingLayoutHandle      m_geometryBindingLayout;
    caustica::rhi::BindingSetHandle         m_geometryBindingSet;

    caustica::rhi::TextureHandle            m_debugVizOutput;
    bool                            m_hasReadbackHistory = false;

    caustica::rhi::ShaderHandle             m_blendDebugVizPS;
    caustica::rhi::GraphicsPipelineHandle   m_blendDebugVizPSO;


    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
    caustica::render::RenderDevice& m_renderDevice;

    std::array<char, SHADER_DEBUG_HEADER_IN_BYTES>              m_initHeader;
    std::array<char, SHADER_DEBUG_BUFFER_IN_BYTES_NO_TRIANGLES> m_lastBuffer;

public:
    ShaderDebug( caustica::rhi::Device* device, caustica::rhi::CommandList* commandList, std::shared_ptr<caustica::ShaderFactory> shaderFactory, caustica::render::RenderDevice& renderDevice );

    void                    createRenderPasses( caustica::rhi::Framebuffer * frameBuffer, caustica::rhi::TextureHandle depthBuffer );

    void                    beginFrame( caustica::rhi::CommandList* commandList, const caustica::math::float4x4 & matWorldToClip );
    void                    clearDebugVizTexture(caustica::rhi::CommandListHandle commandList); // not merged with beginFrame since sometimes you want it to persist between frames

    void                    endFrameAndOutput( caustica::rhi::CommandList* commandList, caustica::rhi::Framebuffer * frameBuffer, caustica::rhi::TextureHandle depthBuffer, const caustica::rhi::Viewport & viewport );

    caustica::rhi::BufferHandle     getGPUWriteBuffer()         { return m_bufferGPU; };
    caustica::rhi::TextureHandle    getDebugVizTexture()        { return m_debugVizOutput; };

    //void                    DrawLastBuffer( view? Frame buffer? Depth buffer? See line drawing! )

private:
    void                    outputLastBufferPrints();
    void                    drawCurrentBufferGeometry(caustica::rhi::CommandList* commandList, caustica::rhi::Framebuffer * frameBuffer, caustica::rhi::TextureHandle depthBuffer, const caustica::rhi::Viewport & viewport);
};
