#pragma once

#include <mutex>
#include <rhi/rhi_types.h>

namespace caustica::rhi::utils
{
    CAUSTICA_RHI_API BlendState::RenderTarget CreateAddBlendState(
        BlendFactor srcBlend,
        BlendFactor dstBlend);


    CAUSTICA_RHI_API BufferDesc CreateStaticConstantBufferDesc(
        uint32_t byteSize,
        const char* debugName);

    CAUSTICA_RHI_API BufferDesc CreateVolatileConstantBufferDesc(
        uint32_t byteSize,
        const char* debugName,
        uint32_t maxVersions);

    CAUSTICA_RHI_API bool CreateBindingSetAndLayout(
        Device* device, 
        caustica::rhi::ShaderType visibility,
        uint32_t registerSpace,
        const BindingSetDesc& bindingSetDesc, 
        BindingLayoutHandle& bindingLayout, 
        BindingSetHandle& bindingSet,
        bool registerSpaceIsDescriptorSet = false);

    CAUSTICA_RHI_API void ClearColorAttachment(
        CommandList* commandList,
        Framebuffer* framebuffer,
        uint32_t attachmentIndex,
        Color color
    );

    CAUSTICA_RHI_API void ClearDepthStencilAttachment(
        CommandList* commandList,
        Framebuffer* framebuffer,
        float depth,
        uint32_t stencil
    );

    CAUSTICA_RHI_API void BuildBottomLevelAccelStruct(
        CommandList* commandList,
        rt::AccelStruct* as,
        const rt::AccelStructDesc& desc
    );

    // Places a UAV barrier on the provided texture.
    // Useful when doing multiple consecutive dispatch calls with the same resources but different constants.
    // Ignored if there was a call to setEnableUavBarriersForTexrure(..., false) on this texture.
    CAUSTICA_RHI_API void TextureUavBarrier(
        CommandList* commandList,
        Texture* texture);

    // Places a UAV barrier on the provided buffer.
    // Useful when doing multiple consecutive dispatch calls with the same resources but different constants.
    // Ignored if there was a call to setEnableUavBarriersForBuffer(..., false) on this buffer.
    CAUSTICA_RHI_API void BufferUavBarrier(
        CommandList* commandList,
        Buffer* buffer);

    // Selects a format from the supplied list that supports all the required features on the given device.
    // The formats are tested in the same order they're provided, and the first matching one is returned.
    // If no formats are matching, Format::UNKNOWN is returned.
    CAUSTICA_RHI_API Format ChooseFormat(
        Device* device,
        caustica::rhi::FormatSupport requiredFeatures,
        const caustica::rhi::Format* requestedFormats,
        size_t requestedFormatCount);
    
    CAUSTICA_RHI_API const char* GraphicsAPIToString(GraphicsAPI api);
    CAUSTICA_RHI_API const char* TextureDimensionToString(TextureDimension dimension);
    CAUSTICA_RHI_API const char* DebugNameToString(const std::string& debugName);
    CAUSTICA_RHI_API const char* ShaderStageToString(ShaderType stage);
    CAUSTICA_RHI_API const char* ResourceTypeToString(ResourceType type);
    CAUSTICA_RHI_API const char* FormatToString(Format format);
    CAUSTICA_RHI_API const char* CommandQueueToString(CommandQueue queue);

    std::string GenerateHeapDebugName(const HeapDesc& desc);
    std::string GenerateTextureDebugName(const TextureDesc& desc);
    std::string GenerateBufferDebugName(const BufferDesc& desc);

    void NotImplemented();
    void NotSupported();
    void InvalidEnum();

    class BitSetAllocator
    {
    public:
        explicit BitSetAllocator(size_t capacity, bool multithreaded);

        int allocate();
        void release(int index);
        [[nodiscard]] size_t getCapacity() const { return m_Allocated.size(); }

    private:
        int m_NextAvailable = 0;
        std::vector<bool> m_Allocated;
        bool m_MultiThreaded;
        std::mutex m_Mutex;
    };

    // Automatic begin/end marker for command list
    class ScopedMarker
    {
    public:
        CommandList* m_commandList;
        ScopedMarker(CommandList* commandList, const char* markerName) : m_commandList(commandList)
        {
            m_commandList->beginMarker(markerName);
        }

        ScopedMarker(CommandListHandle* commandList, const char* markerName) :
            ScopedMarker(commandList->Get(), markerName)
        {}

        ~ScopedMarker()
        {
            m_commandList->endMarker();
        }
    };

}

