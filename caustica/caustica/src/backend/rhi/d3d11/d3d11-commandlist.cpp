#include "d3d11-backend.h"
#include <rhi/utils.h>

namespace caustica::rhi::d3d11
{
    CommandList::CommandList(const Context& context, Device* device, const CommandListParameters& params)
        : m_Context(context)
        , m_Device(device)
        , m_Desc(params)
    {
        m_Context.immediateContext->QueryInterface(IID_PPV_ARGS(&m_UserDefinedAnnotation));
#if CAUSTICA_RHI_WITH_AFTERMATH
        if (m_Device->isAftermathEnabled())
            m_Device->getAftermathCrashDumpHelper().registerAftermathMarkerTracker(&m_AftermathTracker);
#endif
    }

    CommandList::~CommandList()
    {
#if CAUSTICA_RHI_WITH_AFTERMATH
        if (m_Device->isAftermathEnabled())
            m_Device->getAftermathCrashDumpHelper().unRegisterAftermathMarkerTracker(&m_AftermathTracker);
#endif
    }

    Object CommandList::getNativeObject(ObjectType objectType)
    {
        switch (objectType)
        {
        case ObjectTypes::D3D11_DeviceContext:
            return Object(m_Context.immediateContext);
        default:
            return nullptr;
        }
    }

    void CommandList::open()
    {
        clearState();
    }

    void CommandList::close()
    {
        while (m_NumUAVOverlapCommands > 0)
            leaveUAVOverlapSection();

        clearState();
    }

    void CommandList::clearState()
    {
        m_Context.immediateContext->ClearState();

#if CAUSTICA_RHI_D3D11_WITH_NVAPI
        if (m_CurrentGraphicsStateValid && m_CurrentSinglePassStereoState.enabled)
        {
            NvAPI_D3D_SetSinglePassStereoMode(m_Context.immediateContext, 1, 0, 0);
        }
#endif

        m_CurrentGraphicsStateValid = false;
        m_CurrentComputeStateValid = false;

        // Release the strong references to pipeline objects
        m_CurrentGraphicsPipeline = nullptr;
        m_CurrentFramebuffer = nullptr;
        m_CurrentBindings.resize(0);
        m_CurrentVertexBuffers.resize(0);
        m_CurrentIndexBuffer = nullptr;
        m_CurrentComputePipeline = nullptr;
        m_CurrentIndirectBuffer = nullptr;
        m_CurrentBlendConstantColor = Color{};
    }

    void CommandList::setEnableUavBarriersForTexture(rhi::Texture* texture, bool enableBarriers)
    {
        (void)texture;

        if (enableBarriers)
            leaveUAVOverlapSection();
        else
            enterUAVOverlapSection();
    }

    void CommandList::setEnableUavBarriersForBuffer(rhi::Buffer* buffer, bool enableBarriers)
    {
        (void)buffer;

        if (enableBarriers)
            leaveUAVOverlapSection();
        else
            enterUAVOverlapSection();
    }

    void CommandList::enterUAVOverlapSection()
    {
#if CAUSTICA_RHI_D3D11_WITH_NVAPI
        if (m_NumUAVOverlapCommands == 0)
            NvAPI_D3D11_BeginUAVOverlap(m_Context.immediateContext);
#endif

        m_NumUAVOverlapCommands += 1;
    }

    void CommandList::leaveUAVOverlapSection()
    {
#if CAUSTICA_RHI_D3D11_WITH_NVAPI
        if (m_NumUAVOverlapCommands == 1)
            NvAPI_D3D11_EndUAVOverlap(m_Context.immediateContext);
#endif

        m_NumUAVOverlapCommands = std::max(0, m_NumUAVOverlapCommands - 1);
    }

    void CommandList::beginMarker(const char* name)
    {
        if (m_UserDefinedAnnotation)
        {
            wchar_t bufW[1024];
            MultiByteToWideChar(CP_ACP, MB_PRECOMPOSED, name, -1, bufW, ARRAYSIZE(bufW));

            m_UserDefinedAnnotation->BeginEvent(bufW);
        }
#if CAUSTICA_RHI_WITH_AFTERMATH
        if (m_Device->isAftermathEnabled())
        {
            const size_t aftermathMarker = m_AftermathTracker.pushEvent(name);
            GFSDK_Aftermath_SetEventMarker(m_Context.aftermathContext, (const void*)aftermathMarker, 0);
        }
#endif
    }

    void CommandList::endMarker()
    {
        if (m_UserDefinedAnnotation)
        {
            m_UserDefinedAnnotation->EndEvent();
        }
#if CAUSTICA_RHI_WITH_AFTERMATH
        if (m_Device->isAftermathEnabled())
            m_AftermathTracker.popEvent();
#endif
    }
    
    static char g_PushConstantPaddingBuffer[c_MaxPushConstantSize] = {};

    void CommandList::setPushConstants(const void* data, size_t byteSize)
    {
        if (byteSize > c_MaxPushConstantSize)
            return;

        memcpy(g_PushConstantPaddingBuffer, data, byteSize);

        m_Context.immediateContext->UpdateSubresource(
            m_Context.pushConstantBuffer, 0, nullptr, 
            g_PushConstantPaddingBuffer, 0, 0);
    }

    void CommandList::setMeshletState(const MeshletState&)
    {
        utils::NotSupported();
    }

    void CommandList::dispatchMesh(uint32_t, uint32_t, uint32_t)
    {
        utils::NotSupported();
    }

    void CommandList::setRayTracingState(const rt::State&)
    {
        utils::NotSupported();
    }

    void CommandList::dispatchRays(const rt::DispatchRaysArguments&)
    {
        utils::NotSupported();
    }

    void CommandList::buildOpacityMicromap(rt::OpacityMicromap* , const rt::OpacityMicromapDesc& )
    {
        utils::NotSupported();
    }

    void CommandList::buildBottomLevelAccelStruct(rt::AccelStruct*, const rt::GeometryDesc*, size_t, rt::AccelStructBuildFlags)
    {
        utils::NotSupported();
    }

    void CommandList::compactBottomLevelAccelStructs()
    {
        utils::NotSupported();
    }

    void CommandList::buildTopLevelAccelStruct(rt::AccelStruct*, const rt::InstanceDesc*, size_t, rt::AccelStructBuildFlags)
    {
        utils::NotSupported();
    }

    void CommandList::buildTopLevelAccelStructFromBuffer(rt::AccelStruct*, caustica::rhi::Buffer*, uint64_t, size_t, rt::AccelStructBuildFlags)
    {
        utils::NotSupported();
    }

    void CommandList::executeMultiIndirectClusterOperation(const rt::cluster::OperationDesc&)
    {
        utils::NotSupported();
    }

    void CommandList::convertCoopVecMatrices(coopvec::ConvertMatrixLayoutDesc const*, size_t)
    {
        utils::NotSupported();
    }

} // namespace caustica::rhi::d3d11
