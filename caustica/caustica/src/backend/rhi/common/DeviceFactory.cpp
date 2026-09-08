#include <rhi/device_factory.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <sstream>

namespace caustica::rhi
{
    namespace
    {
        std::string toLower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return char(std::tolower(c));
            });
            return value;
        }

        std::string trim(std::string value)
        {
            const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
            value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) {
                return !isSpace(static_cast<unsigned char>(c));
            }));
            value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) {
                return !isSpace(static_cast<unsigned char>(c));
            }).base(), value.end());
            return value;
        }

        std::string normalizeHex(std::string value)
        {
            value = toLower(trim(std::move(value)));
            if (value.starts_with("0x"))
                value.erase(0, 2);

            value.erase(std::remove_if(value.begin(), value.end(), [](char c) {
                return c == '-' || c == ':' || std::isspace(static_cast<unsigned char>(c)) != 0;
            }), value.end());
            return value;
        }

        bool isValidHex(const std::string& value, size_t expectedLength)
        {
            return value.size() == expectedLength && std::all_of(value.begin(), value.end(), [](unsigned char c) {
                return std::isxdigit(c) != 0;
            });
        }

        template<size_t Size>
        std::string bytesToHex(const std::array<uint8_t, Size>& bytes)
        {
            static constexpr char digits[] = "0123456789abcdef";
            std::string result(Size * 2, '0');
            for (size_t i = 0; i < Size; ++i)
            {
                result[i * 2] = digits[bytes[i] >> 4];
                result[i * 2 + 1] = digits[bytes[i] & 0xf];
            }
            return result;
        }

        uint64_t autoScore(const AdapterDesc& adapter)
        {
            if (!adapter.suitable || adapter.software || adapter.type == AdapterType::Software)
                return 0;

            if (adapter.selectionScore != 0)
                return adapter.selectionScore;

            uint64_t score = 1;
            switch (adapter.type)
            {
            case AdapterType::Discrete: score += 4'000'000; break;
            case AdapterType::Integrated: score += 2'000'000; break;
            case AdapterType::Virtual: score += 1'000'000; break;
            case AdapterType::Software: return 0;
            case AdapterType::Unknown: break;
            }

            constexpr uint64_t oneGiB = 1024ull * 1024ull * 1024ull;
            score += std::min<uint64_t>(adapter.dedicatedVideoMemory / oneGiB, 512);
            return score;
        }

        std::string describeAdapters(const std::vector<AdapterDesc>& adapters)
        {
            std::ostringstream stream;
            for (const AdapterDesc& adapter : adapters)
            {
                if (stream.tellp() > 0)
                    stream << ", ";
                stream << adapter.index << ":" << adapter.name;
            }
            return stream.str();
        }
    }

    AdapterSelector AdapterSelector::automatic()
    {
        return {};
    }

    AdapterSelector AdapterSelector::byIndex(int index)
    {
        AdapterSelector selector;
        selector.mode = Mode::Index;
        selector.index = index;
        return selector;
    }

    AdapterSelector AdapterSelector::byName(std::string adapterName)
    {
        AdapterSelector selector;
        selector.mode = Mode::Name;
        selector.value = std::move(adapterName);
        return selector;
    }

    AdapterSelector AdapterSelector::byUuid(std::string adapterUuid)
    {
        AdapterSelector selector;
        selector.mode = Mode::UUID;
        selector.value = std::move(adapterUuid);
        return selector;
    }

    AdapterSelector AdapterSelector::byLuid(std::string adapterLuid)
    {
        AdapterSelector selector;
        selector.mode = Mode::LUID;
        selector.value = std::move(adapterLuid);
        return selector;
    }

    bool parseAdapterSelector(const std::string& text, AdapterSelector& outSelector, std::string* outError)
    {
        const std::string input = trim(text);
        const std::string lower = toLower(input);
        if (input.empty() || lower == "auto")
        {
            outSelector = AdapterSelector::automatic();
            return true;
        }

        const size_t separator = input.find(':');
        std::string kind;
        std::string value;
        if (separator == std::string::npos)
        {
            kind.clear();
            value = input;
        }
        else
        {
            kind = toLower(trim(input.substr(0, separator)));
            value = trim(input.substr(separator + 1));
        }

        const bool looksNumeric = !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        });

        if (kind == "index" || (kind.empty() && looksNumeric))
        {
            int index = -1;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), index);
            if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || index < 0)
            {
                if (outError)
                    *outError = "adapter index must be a non-negative integer";
                return false;
            }
            outSelector = AdapterSelector::byIndex(index);
            return true;
        }

        if (kind.empty() || kind == "name")
        {
            if (value.empty())
            {
                if (outError)
                    *outError = "adapter name cannot be empty";
                return false;
            }
            outSelector = AdapterSelector::byName(value);
            return true;
        }

        if (kind == "uuid")
        {
            const std::string normalized = normalizeHex(value);
            if (!isValidHex(normalized, AdapterDesc::UUID{}.size() * 2))
            {
                if (outError)
                    *outError = "adapter UUID must contain 32 hexadecimal digits";
                return false;
            }
            outSelector = AdapterSelector::byUuid(normalized);
            return true;
        }

        if (kind == "luid")
        {
            const std::string normalized = normalizeHex(value);
            if (!isValidHex(normalized, AdapterDesc::LUID{}.size() * 2))
            {
                if (outError)
                    *outError = "adapter LUID must contain 16 hexadecimal digits";
                return false;
            }
            outSelector = AdapterSelector::byLuid(normalized);
            return true;
        }

        if (outError)
            *outError = "unknown adapter selector prefix '" + kind + "'";
        return false;
    }

    std::string adapterSelectorToString(const AdapterSelector& selector)
    {
        switch (selector.mode)
        {
        case AdapterSelector::Mode::Auto:
            return "auto";
        case AdapterSelector::Mode::Index:
            return "index:" + std::to_string(selector.index);
        case AdapterSelector::Mode::Name:
            return "name:" + selector.value;
        case AdapterSelector::Mode::UUID:
            return "uuid:" + normalizeHex(selector.value);
        case AdapterSelector::Mode::LUID:
            return "luid:" + normalizeHex(selector.value);
        }
        return "auto";
    }

    AdapterSelectionResult selectAdapter(const std::vector<AdapterDesc>& adapters, const AdapterSelector& selector)
    {
        AdapterSelectionResult result;
        if (adapters.empty())
        {
            result.error = "no GPU adapters were found";
            return result;
        }

        if (selector.mode == AdapterSelector::Mode::Auto)
        {
            const AdapterDesc* best = nullptr;
            uint64_t bestScore = 0;
            for (const AdapterDesc& adapter : adapters)
            {
                const uint64_t score = autoScore(adapter);
                if (score == 0)
                    continue;
                if (!best || score > bestScore || (score == bestScore && adapter.index < best->index))
                {
                    best = &adapter;
                    bestScore = score;
                }
            }
            if (!best)
            {
                result.error = "no suitable hardware GPU adapter was found; available adapters: "
                    + describeAdapters(adapters);
                return result;
            }
            result.index = int(best->index);
            return result;
        }

        std::vector<const AdapterDesc*> matches;
        switch (selector.mode)
        {
        case AdapterSelector::Mode::Index:
            for (const AdapterDesc& adapter : adapters)
                if (int(adapter.index) == selector.index)
                    matches.push_back(&adapter);
            break;

        case AdapterSelector::Mode::Name:
        {
            const std::string needle = toLower(selector.value);
            for (const AdapterDesc& adapter : adapters)
                if (toLower(adapter.name).find(needle) != std::string::npos)
                    matches.push_back(&adapter);
            break;
        }

        case AdapterSelector::Mode::UUID:
        {
            const std::string needle = normalizeHex(selector.value);
            for (const AdapterDesc& adapter : adapters)
                if (adapter.uuid && adapterUuidToString(*adapter.uuid) == needle)
                    matches.push_back(&adapter);
            break;
        }

        case AdapterSelector::Mode::LUID:
        {
            const std::string needle = normalizeHex(selector.value);
            for (const AdapterDesc& adapter : adapters)
                if (adapter.luid && adapterLuidToString(*adapter.luid) == needle)
                    matches.push_back(&adapter);
            break;
        }

        case AdapterSelector::Mode::Auto:
            break;
        }

        if (matches.empty())
        {
            result.error = "requested GPU adapter was not found; available adapters: " + describeAdapters(adapters);
            return result;
        }
        if (matches.size() > 1)
        {
            std::ostringstream stream;
            stream << "adapter selector is ambiguous; matches:";
            for (const AdapterDesc* adapter : matches)
                stream << " " << adapter->index << ":" << adapter->name;
            result.error = stream.str();
            return result;
        }

        if (!matches.front()->suitable || matches.front()->software
            || matches.front()->type == AdapterType::Software)
        {
            result.error = "requested GPU adapter is not suitable for this renderer: "
                + std::to_string(matches.front()->index) + ":" + matches.front()->name;
            return result;
        }

        result.index = int(matches.front()->index);
        return result;
    }

    std::string adapterUuidToString(const AdapterDesc::UUID& uuid)
    {
        return bytesToHex(uuid);
    }

    std::string adapterLuidToString(const AdapterDesc::LUID& luid)
    {
        return bytesToHex(luid);
    }

    const char* adapterTypeToString(AdapterType type)
    {
        switch (type)
        {
        case AdapterType::Discrete: return "discrete";
        case AdapterType::Integrated: return "integrated";
        case AdapterType::Virtual: return "virtual";
        case AdapterType::Software: return "software";
        case AdapterType::Unknown: return "unknown";
        }
        return "unknown";
    }

    DeviceFactory::DeviceFactory(
        EnumerateCallback enumerateCallback,
        CreateCallback createCallback,
        SelectedIndexCallback selectedIndexCallback)
        : m_enumerateCallback(std::move(enumerateCallback))
        , m_createCallback(std::move(createCallback))
        , m_selectedIndexCallback(std::move(selectedIndexCallback))
    {
    }

    bool DeviceFactory::refreshAdapters(std::string* outError)
    {
        m_adapters.clear();
        std::string error;
        if (!m_enumerateCallback || !m_enumerateCallback(m_adapters, error))
        {
            if (outError)
                *outError = error.empty() ? "failed to enumerate GPU adapters" : error;
            return false;
        }

        for (size_t i = 0; i < m_adapters.size(); ++i)
            m_adapters[i].index = uint32_t(i);
        return true;
    }

    DeviceFactoryCreateResult DeviceFactory::createDevice(const AdapterSelector& selector)
    {
        DeviceFactoryCreateResult result;
        m_selectedAdapter.reset();
        std::string error;
        if (!refreshAdapters(&error))
        {
            result.error = std::move(error);
            return result;
        }

        int requestedIndex = -1;
        AdapterSelectionResult selection = selectAdapter(m_adapters, selector);
        if (!selection)
        {
            if (!selector.allowFallback || selector.mode == AdapterSelector::Mode::Auto)
            {
                result.error = std::move(selection.error);
                return result;
            }

            selection = selectAdapter(m_adapters, AdapterSelector::automatic());
            if (!selection)
            {
                result.error = std::move(selection.error);
                return result;
            }
        }
        requestedIndex = selection.index;

        if (!m_createCallback || !m_createCallback(requestedIndex, error))
        {
            result.error = error.empty() ? "failed to create the GPU device" : error;
            return result;
        }

        int selectedIndex = requestedIndex;
        if (m_selectedIndexCallback)
            selectedIndex = m_selectedIndexCallback();

        if (selectedIndex < 0 || size_t(selectedIndex) >= m_adapters.size())
        {
            result.error = "backend reported an invalid selected adapter index";
            return result;
        }

        m_selectedAdapter = m_adapters[size_t(selectedIndex)];
        result.success = true;
        result.adapter = m_selectedAdapter;
        return result;
    }
}
