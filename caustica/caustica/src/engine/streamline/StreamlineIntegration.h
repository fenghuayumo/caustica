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
    virtual void SetViewport(uint32_t viewportIndex) override;
    virtual void SetConstants(const Constants& consts) override;
    virtual void SetDLSSOptions(const DLSSOptions& options) override;
    virtual bool IsDLSSAvailable() const override { return m_dlssAvailable; }
    virtual void QueryDLSSOptimalSettings(const DLSSOptions& options, DLSSSettings& settings) override;
    virtual void EvaluateDLSS(nvrhi::ICommandList* commandList) override;
    virtual void CleanupDLSS(bool wfi) override;

    virtual void SetNISOptions(const NISOptions& options) override;
    virtual bool IsNISAvailable() const override { return m_nisAvailable; }
    virtual void EvaluateNIS(nvrhi::ICommandList* commandList) override;
    virtual void CleanupNIS(bool wfi) override;

    virtual void SetDeepDVCOptions(const DeepDVCOptions& options) override;
    virtual bool IsDeepDVCAvailable() const override { return m_deepdvcAvailable; }
    virtual void QueryDeepDVCState(uint64_t& estimatedVRamUsage) override;
    virtual void EvaluateDeepDVC(nvrhi::ICommandList* commandList) override;
    virtual void CleanupDeepDVC() override;

    virtual bool IsReflexAvailable() const override { return m_reflexAvailable; }
    virtual bool IsPCLAvailable() const override { return m_pclAvailable; }
    virtual void SetReflexConsts(const ReflexOptions& options) override;
    virtual void GetReflexState(ReflexState& state) const override;
    virtual void ReflexTriggerFlash(int frameNumber) override;
    virtual void ReflexTriggerPcPing(int frameNumber) override;

    virtual void GetDLSSGState(DLSSGState& state, const DLSSGOptions& options) override;
    virtual void SetDLSSGOptions(const DLSSGOptions& options) override;
    virtual bool IsDLSSGAvailable() const override { return m_dlssgAvailable; }
    virtual void CleanupDLSSG(bool wfi) override;

    virtual void SetDLSSRROptions(const DLSSRROptions& options) override;
    virtual bool IsDLSSRRAvailable() const override { return m_dlssrrAvailable; }
    virtual void QueryDLSSRROptimalSettings(const DLSSRROptions& options, DLSSRRSettings& settings) override;
    virtual void EvaluateDLSSRR(nvrhi::ICommandList* commandList) override;

    virtual void TagResourcesGeneral(
        nvrhi::ICommandList* commandList,
        const caustica::IView* view,
        nvrhi::ITexture* motionVectors,
        nvrhi::ITexture* depth,
        nvrhi::ITexture* finalColorHudless) override;

    virtual void TagResourcesDLSSNIS(
        nvrhi::ICommandList* commandList,
        const caustica::IView* view,
        nvrhi::ITexture* output,
        nvrhi::ITexture* input) override;

    virtual void TagResourcesDLSSFG(
        nvrhi::ICommandList* commandList,
        bool validViewportExtent = false,
        const Extent &backBufferExtent = {}) override;

    virtual void TagResourcesDeepDVC(
        nvrhi::ICommandList* commandList,
        const caustica::IView* view,
        nvrhi::ITexture* output) override;

    virtual void UnTagResourcesDeepDVC() override;

    // * If roughness != nullptr, normalsAndOptionalRoughness contains only normals. If roughness == nullptr then the 
    //   roughness value should be in .a channel of the normalsAndOptionalRoughness and normalRoughnessMode in 
    //   StreamlineInterface::DLSSRROptions must be set to DLSSRRNormalRoughnessMode::ePacked.
    // * Either specHitDist or specMotionVectors should be provided but not both nor neither. Refer to DLSS-RR 
    //   documentation for more detail.
    virtual void TagResourcesDLSSRR(
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
    void UpdateFeatureAvailable();
    uint32_t CheckNumSupportedFeatures(const sl::AdapterInfo& adapterInfo);

    nvrhi::Object GetNativeCommandList(nvrhi::ICommandList* commandList);
    void WaitForDLSSGInputsProcessing();

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

    static sl::Resource AllocateResourceCallback(const sl::ResourceAllocationDesc* resDesc, void* device);
    static void ReleaseResourceCallback(sl::Resource* resource, void* device);
    static bool UpgradeInterface(IUnknown*& interfacePointer);

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

    void SimStart(GpuDevice& manager) override;
    void SimEnd(GpuDevice& manager) override;
    void RenderStart(GpuDevice& manager) override;
    void RenderEnd(GpuDevice& manager) override;
    void PresentStart(GpuDevice& manager) override;
    void PresentEnd(GpuDevice& manager) override;

    // Captures the Streamline frame token for the current sim frame (call after SimEnd on the main thread).
    void* getFrameTokenForRender() const { return m_currentFrame; }

    class RenderFrameTokenScope
    {
    public:
        explicit RenderFrameTokenScope(void* frameToken);
        ~RenderFrameTokenScope();
        RenderFrameTokenScope(const RenderFrameTokenScope&) = delete;
        RenderFrameTokenScope& operator=(const RenderFrameTokenScope&) = delete;
    };

    bool InitializePreDevice(nvrhi::GraphicsAPI api, int appId, const bool checkSig = true, const bool enableLog = false);
#if CAUSTICA_WITH_DX11 || CAUSTICA_WITH_DX12
    bool SetD3DDevice(IUnknown* nativeDevice);
    bool InitializeDeviceDX(nvrhi::IDevice *device, AdapterInfo::LUID* pAdapterIdDx11 = nullptr);
    // In-place slUpgradeInterface of a raw pointer
    template<typename T>
    static inline bool UpgradeInterface(T*& interfacePointer)
    {
        // Ensure that T derives from IUnknown
        static_assert(std::is_base_of<IUnknown, T>::value);
        return UpgradeInterface((IUnknown*&)interfacePointer);
    }
    // In-place slUpgradeInterface of a nvrhi::RefCountPtr
    template<typename T>
    static inline bool UpgradeInterface(nvrhi::RefCountPtr<T>& interfacePointer)
    {
        // Ensure that T derives from IUnknown
        static_assert(std::is_base_of<IUnknown, T>::value);
        T* rawPointer = interfacePointer; // We need to pass a raw pointer to UpgradeInterface
        bool result = UpgradeInterface(rawPointer);
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
    bool InitializeDeviceVK(nvrhi::IDevice* device, const VulkanInfo& vulkanInfo);
#endif

    void Shutdown();

#if CAUSTICA_WITH_DX11 || CAUSTICA_WITH_DX12
    uint32_t FindBestAdapterDX();
#endif

#if CAUSTICA_WITH_VULKAN
    uint32_t FindBestAdapterVulkan(const std::vector <vk::PhysicalDevice>& vkDevices);
#endif
};

} // namespace caustica

#endif
