#pragma once

#include <rhi/rhi_types.h>
#include <memory>
#include <unordered_map>

namespace caustica::rhi
{
    struct BufferStateExtension
    {
        const BufferDesc& descRef;
        ResourceStates permanentState = ResourceStates::Unknown;

        explicit BufferStateExtension(const BufferDesc& desc)
            : descRef(desc)
        { }
    };

    struct TextureStateExtension
    {
        const TextureDesc& descRef;
        ResourceStates permanentState = ResourceStates::Unknown;
        bool stateInitialized = false;
        bool isSamplerFeedback = false;

        explicit TextureStateExtension(const TextureDesc& desc)
            : descRef(desc)
        { }
    };

    struct TextureState
    {
        std::vector<ResourceStates> subresourceStates;
        ResourceStates state = ResourceStates::Unknown;
        bool enableUavBarriers = true;
        bool firstUavBarrierPlaced = false;
        bool permanentTransition = false;
    };

    struct BufferState
    {
        ResourceStates state = ResourceStates::Unknown;
        bool enableUavBarriers = true;
        bool firstUavBarrierPlaced = false;
        bool permanentTransition = false;
    };

    struct TextureBarrier
    {
        TextureStateExtension* texture = nullptr;
        MipLevel mipLevel = 0;
        ArraySlice arraySlice = 0;
        bool entireTexture = false;
        ResourceStates stateBefore = ResourceStates::Unknown;
        ResourceStates stateAfter = ResourceStates::Unknown;
    };

    struct BufferBarrier
    {
        BufferStateExtension* buffer = nullptr;
        ResourceStates stateBefore = ResourceStates::Unknown;
        ResourceStates stateAfter = ResourceStates::Unknown;
    };

    class CommandListResourceStateTracker
    {
    public:
        explicit CommandListResourceStateTracker(IMessageCallback* messageCallback)
            : m_MessageCallback(messageCallback)
        { }

        // ICommandList-like interface

        void setEnableUavBarriersForTexture(TextureStateExtension* texture, bool enableBarriers);
        void setEnableUavBarriersForBuffer(BufferStateExtension* buffer, bool enableBarriers);

        void beginTrackingTextureState(TextureStateExtension* texture, TextureSubresourceSet subresources, ResourceStates stateBits);
        void beginTrackingBufferState(BufferStateExtension* buffer, ResourceStates stateBits);

        void setPermanentTextureState(TextureStateExtension* texture, TextureSubresourceSet subresources, ResourceStates stateBits);
        void setPermanentBufferState(BufferStateExtension* buffer, ResourceStates stateBits);

        ResourceStates getTextureSubresourceState(TextureStateExtension* texture, ArraySlice arraySlice, MipLevel mipLevel);
        ResourceStates getBufferState(BufferStateExtension* buffer);

        // Internal interface
        
        void requireTextureState(TextureStateExtension* texture, TextureSubresourceSet subresources, ResourceStates state);
        void requireBufferState(BufferStateExtension* buffer, ResourceStates state);

        void keepBufferInitialStates();
        void keepTextureInitialStates();
        void commandListSubmitted();

        [[nodiscard]] const std::vector<TextureBarrier>& getTextureBarriers() const { return m_TextureBarriers; }
        [[nodiscard]] const std::vector<BufferBarrier>& getBufferBarriers() const { return m_BufferBarriers; }
        void clearBarriers() { m_TextureBarriers.clear(); m_BufferBarriers.clear(); }

    private:
        IMessageCallback* m_MessageCallback;

        std::unordered_map<TextureStateExtension*, std::unique_ptr<TextureState>> m_TextureStates;
        std::unordered_map<BufferStateExtension*, std::unique_ptr<BufferState>> m_BufferStates;

        // Deferred transitions of textures and buffers to permanent states.
        // They are executed only when the command list is executed, not when the app calls setPermanentTextureState or setPermanentBufferState.
        std::vector<std::pair<TextureStateExtension*, ResourceStates>> m_PermanentTextureStates;
        std::vector<std::pair<BufferStateExtension*, ResourceStates>> m_PermanentBufferStates;

        std::vector<TextureBarrier> m_TextureBarriers;
        std::vector<BufferBarrier> m_BufferBarriers;

        TextureState* getTextureStateTracking(TextureStateExtension* texture, bool allowCreate);
        BufferState* getBufferStateTracking(BufferStateExtension* buffer, bool allowCreate);
    };

    bool verifyPermanentResourceState(ResourceStates permanentState, ResourceStates requiredState, bool isTexture, const std::string& debugName, IMessageCallback* messageCallback);

} // namespace caustica::rhi