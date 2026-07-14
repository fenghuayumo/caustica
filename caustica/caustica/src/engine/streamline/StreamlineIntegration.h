#if CAUSTICA_WITH_STREAMLINE
#include <backend/StreamlineInterface.h>
#include <backend/GpuDevice.h>

// Streamline Core
#include <sl.h>

#if CAUSTICA_WITH_VULKAN
#include <vulkan/vulkan.hpp>
#endif

namespace caustica
{

// Implementation for StreamlineInterface interface, so that sl types are not exposed to the rest of the app
class StreamlineIntegration : public StreamlineInterface
{
public:
    virtual void setViewport(uint32_t viewportIndex) override;
    virtual void setConstants(const Constants& consts) override;
    virtual void setDLSSOptions(const DLSSOptions& options) override;
    virtual bool isDLSSAvailable() const override { return m_dlssAvailable; }
    virtual void queryDLSSOptimalSettings(const DLSSOptions& options, DLSSSettings& settings) override;
    virtual void evaluateDLSS(nvrhi::ICommandList* commandList) override;
    virtual void cleanupDLSS(bool wfi) override;

    virtual void setNISOptions(const NISOptions& options) override;
    virtual bool isNISAvailable() const override { return m_nisAvailable; }
    virtual void evaluateNIS(nvrhi::ICommandList* commandList) override;
    virtual void cleanupNIS(bool wfi) override;

    virtual void setDeepDVCOptions(const DeepDVCOptions& options) override;
    virtual bool isDeepDVCAvailable() const override { return m_deepdvcAvailable; }
    virtual void queryDeepDVCState(uint64_t& estimatedVRamUsage) override;
    virtual void evaluateDeepDVC(nvrhi::ICommandList* commandList) override;
    virtual void cleanupDeepDVC() override;

    virtual bool isReflexAvailable() const override { return m_reflexAvailable; }
    virtual bool isPCLAvailable() const override { return m_pclAvailable; }
    virtual void setReflexConsts(const ReflexOptions& options) override;
    virtual void getReflexState(ReflexState& state) const override;
    virtual void reflexTriggerFlash(int frameNumber) override;
    virtual void reflexTriggerPcPing(int frameNumber) override;

    virtual void getDLSSGState(DLSSGState& state, const DLSSGOptions& options) override;
    virtual void setDLSSGOptions(const DLSSGOptions& options) override;
    virtual bool isDLSSGAvailable() const override { return m_dlssgAvailable; }
    virtual void cleanupDLSSG(bool wfi) override;

    virtual void setDLSSRROptions(const DLSSRROptions& options) override;
    virtual bool isDLSSRRAvailable() const override { return m_dlssrrAvailable; }
    virtual void queryDLSSRROptimalSettings(const DLSSRROptions& options, DLSSRRSettings& settings) override;
    virtual void evaluateDLSSRR(nvrhi::ICommandList* commandList) override;
    virtual void cleanupDLSSRR(bool wfi) override;

    virtual void tagResourcesGeneral(
        nvrhi::ICommandList* commandList,
        const caustica::IView* view,
        nvrhi::ITexture* motionVectors,
        nvrhi::ITexture* depth,
        nvrhi::ITexture* finalColorHudless) override;

    virtual void tagResourcesDLSSNIS(
        nvrhi::ICommandList* commandList,
        const caustica::IView* view,
        nvrhi::ITexture* output,
        nvrhi::ITexture* input) override;

    virtual void tagResourcesDLSSFG(
        nvrhi::ICommandList* commandList,
        bool validViewportExtent = false,
        const Extent &backBufferExtent = {}) override;

    virtual void tagResourcesDeepDVC(
        nvrhi::ICommandList* commandList,
        const caustica::IView* view,
        nvrhi::ITexture* output) override;

    virtual void unTagResourcesDeepDVC() override;

    // * If roughness != nullptr, normalsAndOptionalRoughness contains only normals. If roughness == nullptr then the 
    //   roughness value should be in .a channel of the normalsAndOptionalRoughness and normalRoughnessMode in 
    //   StreamlineInterface::DLSSRROptions must be set to DLSSRRNormalRoughnessMode::ePacked.
    // * Either specHitDist or specMotionVectors should be provided but not both nor neither. Refer to DLSS-RR 
    //   documentation for more detail.
    virtual void tagResourcesDLSSRR(
        nvrhi::ICommandList* commandList,
        const caustica::IView* view,
        dm::int2 renderSize,
        dm::int2 displaySize,
        nvrhi::ITexture* inputColor,
        nvrhi::ITexture* diffuseAlbedo,
        nvrhi::ITexture* specAlbedo,
        nvrhi::ITexture* normalsAndOptionalRoughness,
        nvrhi::ITexture* roughness,
        nvrhi::ITexture* specHitDist,
        nvrhi::ITexture* specMotionVectors,
        nvrhi::ITexture* outputColor
    ) override;
private:
    StreamlineIntegration() {}
    void updateFeatureAvailable();
    uint32_t checkNumSupportedFeatures(const sl::AdapterInfo& adapterInfo);

    nvrhi::Object getNativeCommandList(nvrhi::ICommandList* commandList);
    void waitForDLSSGInputsProcessing();

    bool m_slInitialized = false;
    nvrhi::GraphicsAPI m_api = nvrhi::GraphicsAPI::D3D12;
    nvrhi::IDevice* m_device = nullptr;

#ifdef CAUSTICA_WITH_DX11
    LUID m_d3d11Luid = {};
#endif

    bool m_dlssAvailable = false;
    bool m_nisAvailable = false;
    bool m_deepdvcAvailable = false;
    bool m_dlssgAvailable = false;
    bool m_dlssrrAvailable = false;
    bool m_reflexAvailable = false;
    bool m_pclAvailable = false;

    static sl::Resource allocateResourceCallback(const sl::ResourceAllocationDesc* resDesc, void* device);
    static void releaseResourceCallback(sl::Resource* resource, void* device);
    static bool upgradeInterface(IUnknown*& interfacePointer);

    sl::FrameToken* m_currentFrame = nullptr;
    sl::ViewportHandle m_viewport = {0};
    void* m_dlssgInputsProcessingCompletionFence = nullptr;
    uint64_t m_dlssgLastPresentInputsProcessingCompletionFenceValue = 0;

    void bindRenderFrameToken(void* frameToken);
    void unbindRenderFrameToken();
    [[nodiscard]] sl::FrameToken* activeFrameToken() const;

public:
    // Interface for device manager
    static StreamlineIntegration& Get();
    StreamlineIntegration(const StreamlineIntegration&) = delete;
    StreamlineIntegration(StreamlineIntegration&&) = delete;
    StreamlineIntegration& operator=(const StreamlineIntegration&) = delete;
    StreamlineIntegration& operator=(StreamlineIntegration&&) = delete;

    void simStart(GpuDevice& manager) override;
    void simEnd(GpuDevice& manager) override;
    void renderStart(GpuDevice& manager) override;
    void renderEnd(GpuDevice& manager) override;
    void presentStart(GpuDevice& manager) override;
    void presentEnd(GpuDevice& manager) override;

    // Captures the Streamline frame token for the current sim frame (call after simEnd on the main thread).
    void* getFrameTokenForRender() const { return m_currentFrame; }

    class RenderFrameTokenScope
    {
    public:
        explicit RenderFrameTokenScope(void* frameToken);
        ~RenderFrameTokenScope();
        RenderFrameTokenScope(const RenderFrameTokenScope&) = delete;
        RenderFrameTokenScope& operator=(const RenderFrameTokenScope&) = delete;
    };

    bool initializePreDevice(nvrhi::GraphicsAPI api, int appId, const bool checkSig = true, const bool enableLog = false);
#if CAUSTICA_WITH_DX11 || CAUSTICA_WITH_DX12
    bool setD3DDevice(IUnknown* nativeDevice);
    bool initializeDeviceDX(nvrhi::IDevice *device, AdapterInfo::LUID* pAdapterIdDx11 = nullptr);
    // In-place slUpgradeInterface of a raw pointer
    template<typename T>
    static inline bool upgradeInterface(T*& interfacePointer)
    {
        // Ensure that T derives from IUnknown
        static_assert(std::is_base_of<IUnknown, T>::value);
        return upgradeInterface((IUnknown*&)interfacePointer);
    }
    // In-place slUpgradeInterface of a nvrhi::RefCountPtr
    template<typename T>
    static inline bool upgradeInterface(nvrhi::RefCountPtr<T>& interfacePointer)
    {
        // Ensure that T derives from IUnknown
        static_assert(std::is_base_of<IUnknown, T>::value);
        T* rawPointer = interfacePointer; // We need to pass a raw pointer to upgradeInterface
        bool result = upgradeInterface(rawPointer);
        interfacePointer = rawPointer;
        return result;
    }
#endif


#if CAUSTICA_WITH_VULKAN
    // see sl::VulkanInfo in sl_helpers_vk.h
    struct VulkanInfo
    {
        void *vkDevice{};
        void *vkInstance{};
        void *vkPhysicalDevice{};
        
        uint32_t computeQueueIndex{};
        uint32_t computeQueueFamily{};
        uint32_t graphicsQueueIndex{};
        uint32_t graphicsQueueFamily{};
        uint32_t opticalFlowQueueIndex{};
        uint32_t opticalFlowQueueFamily{};
        bool useNativeOpticalFlowMode = false;
        uint32_t computeQueueCreateFlags{};
        uint32_t graphicsQueueCreateFlags{};
        uint32_t opticalFlowQueueCreateFlags{};
    };
    bool initializeDeviceVK(nvrhi::IDevice* device, const VulkanInfo& vulkanInfo);
#endif

    void shutdown();

#if CAUSTICA_WITH_DX11 || CAUSTICA_WITH_DX12
    uint32_t findBestAdapterDX();
#endif

#if CAUSTICA_WITH_VULKAN
    uint32_t findBestAdapterVulkan(const std::vector <vk::PhysicalDevice>& vkDevices);
#endif
};

} // namespace caustica

#endif
