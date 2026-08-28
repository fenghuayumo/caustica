#pragma once

#include <rhi/rhi_types.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace caustica::rhi
{
    enum class AdapterType : uint8_t
    {
        Unknown,
        Discrete,
        Integrated,
        Virtual,
        Software
    };

    struct AdapterDesc
    {
        using UUID = std::array<uint8_t, 16>;
        using LUID = std::array<uint8_t, 8>;

        uint32_t index = 0;
        std::string name;
        GraphicsAPI api = GraphicsAPI::D3D12;
        AdapterType type = AdapterType::Unknown;
        uint32_t vendorID = 0;
        uint32_t deviceID = 0;
        uint64_t dedicatedVideoMemory = 0;
        // Backend-provided, monotonic score used only for automatic selection.
        // It combines compute-oriented limits, required rendering features and
        // memory capacity; it is not a benchmark result.
        uint64_t selectionScore = 0;
        bool supportsRayTracingPipeline = false;
        bool supportsRayQuery = false;
        bool suitable = true;
        bool software = false;

        std::optional<UUID> uuid;
        std::optional<LUID> luid;
    };

    struct AdapterSelector
    {
        enum class Mode : uint8_t
        {
            Auto,
            Index,
            Name,
            UUID,
            LUID
        };

        Mode mode = Mode::Auto;
        int index = -1;
        std::string value;
        bool allowFallback = false;

        static AdapterSelector automatic();
        static AdapterSelector byIndex(int index);
        static AdapterSelector byName(std::string adapterName);
        static AdapterSelector byUuid(std::string adapterUuid);
        static AdapterSelector byLuid(std::string adapterLuid);
    };

    struct AdapterSelectionResult
    {
        int index = -1;
        std::string error;

        [[nodiscard]] explicit operator bool() const { return index >= 0; }
    };

    struct DeviceFactoryCreateResult
    {
        bool success = false;
        std::optional<AdapterDesc> adapter;
        std::string error;

        [[nodiscard]] explicit operator bool() const { return success; }
    };

    // Parses: auto, index:N, name:text, uuid:hex, luid:hex.
    // A bare integer is treated as an index and any other bare string as a name.
    CAUSTICA_RHI_API bool parseAdapterSelector(
        const std::string& text,
        AdapterSelector& outSelector,
        std::string* outError = nullptr);

    CAUSTICA_RHI_API std::string adapterSelectorToString(const AdapterSelector& selector);

    CAUSTICA_RHI_API AdapterSelectionResult selectAdapter(
        const std::vector<AdapterDesc>& adapters,
        const AdapterSelector& selector);

    CAUSTICA_RHI_API std::string adapterUuidToString(const AdapterDesc::UUID& uuid);
    CAUSTICA_RHI_API std::string adapterLuidToString(const AdapterDesc::LUID& luid);
    CAUSTICA_RHI_API const char* adapterTypeToString(AdapterType type);

    // Backend-neutral orchestration for adapter discovery, explicit selection and
    // native-device creation. Native API calls stay in backend callbacks, keeping
    // the core RHI independent of GLFW, DXGI presentation, and Streamline.
    class CAUSTICA_RHI_API DeviceFactory
    {
    public:
        using EnumerateCallback = std::function<bool(std::vector<AdapterDesc>&, std::string&)>;
        using CreateCallback = std::function<bool(int index, std::string&)>;
        using SelectedIndexCallback = std::function<int()>;

        DeviceFactory(
            EnumerateCallback enumerateCallback,
            CreateCallback createCallback,
            SelectedIndexCallback selectedIndexCallback = {});

        bool refreshAdapters(std::string* outError = nullptr);
        DeviceFactoryCreateResult createDevice(const AdapterSelector& selector);

        [[nodiscard]] const std::vector<AdapterDesc>& adapters() const { return m_adapters; }
        [[nodiscard]] const std::optional<AdapterDesc>& selectedAdapter() const { return m_selectedAdapter; }

    private:
        EnumerateCallback m_enumerateCallback;
        CreateCallback m_createCallback;
        SelectedIndexCallback m_selectedIndexCallback;
        std::vector<AdapterDesc> m_adapters;
        std::optional<AdapterDesc> m_selectedAdapter;
    };
}
