#include <assets/loader/ShaderPackFileSystem.h>

#include <core/log.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <vector>

#ifdef WIN32
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

namespace
{
    constexpr std::array<char, 8> c_ShaderPackMagic = { 'C', 'A', 'U', 'S', 'S', 'H', 'D', '1' };
    constexpr uint32_t c_ShaderPackVersion = 1;

#pragma pack(push, 1)
    struct ShaderPackHeader
    {
        char magic[8];
        uint32_t version;
        uint32_t entryCount;
    };

    struct ShaderPackEntry
    {
        uint64_t hash0;
        uint64_t hash1;
        uint64_t offset;
        uint64_t size;
    };
#pragma pack(pop)

    static uint64_t Fnva64(const std::string& value, uint64_t seed)
    {
        uint64_t hash = 14695981039346656037ull ^ seed;
        for (unsigned char ch : value)
        {
            hash ^= uint64_t(ch);
            hash *= 1099511628211ull;
        }
        return hash;
    }

    static uint64_t Rotl64(uint64_t value, uint32_t shift)
    {
        return (value << shift) | (value >> (64u - shift));
    }

    static uint64_t XorShift64Star(uint64_t& state)
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 2685821657736338717ull;
    }
}

ShaderPackFileSystem::ShaderPackFileSystem(
    const std::filesystem::path& packPath,
    const std::filesystem::path& virtualRoot)
    : m_packPath(std::filesystem::absolute(packPath).lexically_normal())
    , m_virtualRoot(virtualRoot.lexically_normal())
{
    m_packFile = fopen(m_packPath.string().c_str(), "rb");
    if (!m_packFile)
        return;

    ShaderPackHeader header{};
    if (fread(&header, sizeof(header), 1, m_packFile) != 1)
    {
        caustica::warning("Unable to read shader pack header '%s'", m_packPath.string().c_str());
        fclose(m_packFile);
        m_packFile = nullptr;
        return;
    }

    if (std::memcmp(header.magic, c_ShaderPackMagic.data(), c_ShaderPackMagic.size()) != 0 ||
        header.version != c_ShaderPackVersion)
    {
        caustica::warning("Shader pack '%s' has an unsupported format", m_packPath.string().c_str());
        fclose(m_packFile);
        m_packFile = nullptr;
        return;
    }

    m_entries.reserve(header.entryCount);
    for (uint32_t index = 0; index < header.entryCount; ++index)
    {
        ShaderPackEntry diskEntry{};
        if (fread(&diskEntry, sizeof(diskEntry), 1, m_packFile) != 1)
        {
            caustica::warning("Shader pack '%s' has a truncated entry table", m_packPath.string().c_str());
            fclose(m_packFile);
            m_packFile = nullptr;
            m_entries.clear();
            return;
        }

        PackKey key{ diskEntry.hash0, diskEntry.hash1 };
        m_entries[key] = FileEntry{ diskEntry.offset, diskEntry.size };
    }

    caustica::info("Mounted shader pack '%s' at virtual root '%s' (%d entries)",
        m_packPath.string().c_str(),
        m_virtualRoot.generic_string().c_str(),
        int(m_entries.size()));
}

ShaderPackFileSystem::~ShaderPackFileSystem()
{
    std::lock_guard<std::mutex> lockGuard(m_mutex);
    if (m_packFile)
    {
        fclose(m_packFile);
        m_packFile = nullptr;
    }
}

ShaderPackFileSystem::PackKey ShaderPackFileSystem::hashPath(const std::string& logicalPath)
{
    return PackKey{
        Fnva64(logicalPath, 0x243f6a8885a308d3ull),
        Fnva64(logicalPath, 0x13198a2e03707344ull)
    };
}

void ShaderPackFileSystem::decodePayload(uint8_t* data, size_t size, const PackKey& key)
{
    uint64_t state = key.h0 ^ Rotl64(key.h1, 1) ^ 0xa5a5a5a55a5a5a5aull;
    uint64_t streamWord = 0;
    int streamBytesLeft = 0;

    for (size_t index = 0; index < size; ++index)
    {
        if (streamBytesLeft == 0)
        {
            streamWord = XorShift64Star(state);
            streamBytesLeft = 8;
        }

        data[index] ^= uint8_t(streamWord & 0xffu);
        streamWord >>= 8;
        --streamBytesLeft;
    }
}

std::string ShaderPackFileSystem::normalizeLogicalPath(const std::filesystem::path& name) const
{
    std::filesystem::path logicalPath = (m_virtualRoot / name.relative_path()).lexically_normal();
    std::string normalized = logicalPath.generic_string();

    while (!normalized.empty() && (normalized.front() == '/' || normalized.front() == '\\'))
        normalized.erase(normalized.begin());

    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    return normalized;
}

bool ShaderPackFileSystem::folderExists(const std::filesystem::path&)
{
    return false;
}

bool ShaderPackFileSystem::hasDynamicBinLayout(const std::filesystem::path& diskBinApiRoot)
{
    if (!m_packFile)
        return false;

    std::error_code ec;
    bool foundDiskBin = false;
    for (const auto& prefixDir : std::filesystem::directory_iterator(diskBinApiRoot, ec))
    {
        if (!prefixDir.is_directory())
            continue;

        for (const auto& binFile : std::filesystem::directory_iterator(prefixDir.path(), ec))
        {
            if (binFile.path().extension() != ".bin")
                continue;

            foundDiskBin = true;
            const std::filesystem::path vfsRelative =
                prefixDir.path().filename() / binFile.path().filename();
            return fileExists(vfsRelative);
        }
    }

    return !foundDiskBin;
}

bool ShaderPackFileSystem::fileExists(const std::filesystem::path& name)
{
    if (!m_packFile)
        return false;

    const PackKey key = hashPath(normalizeLogicalPath(name));
    return m_entries.find(key) != m_entries.end();
}

std::shared_ptr<caustica::IBlob> ShaderPackFileSystem::readFile(const std::filesystem::path& name)
{
    if (!m_packFile)
        return nullptr;

    const std::string logicalPath = normalizeLogicalPath(name);
    const PackKey key = hashPath(logicalPath);
    auto entryIt = m_entries.find(key);
    if (entryIt == m_entries.end())
        return nullptr;

    if (entryIt->second.size > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    {
        caustica::warning("Shader pack entry '%s' is too large to load", logicalPath.c_str());
        return nullptr;
    }

    std::vector<uint8_t> encodedData(size_t(entryIt->second.size));
    {
        std::lock_guard<std::mutex> lockGuard(m_mutex);
        if (fseeko(m_packFile, int64_t(entryIt->second.offset), SEEK_SET) != 0)
        {
            caustica::warning("Unable to seek shader pack '%s' for '%s'",
                m_packPath.string().c_str(), logicalPath.c_str());
            return nullptr;
        }

        if (fread(encodedData.data(), 1, encodedData.size(), m_packFile) != encodedData.size())
        {
            caustica::warning("Unable to read shader pack '%s' entry '%s'",
                m_packPath.string().c_str(), logicalPath.c_str());
            return nullptr;
        }
    }

    decodePayload(encodedData.data(), encodedData.size(), key);

    void* blobData = malloc(encodedData.size());
    if (!blobData)
        return nullptr;

    std::memcpy(blobData, encodedData.data(), encodedData.size());
    return std::make_shared<caustica::Blob>(blobData, encodedData.size());
}

bool ShaderPackFileSystem::writeFile(const std::filesystem::path&, const void*, size_t)
{
    return false;
}

int ShaderPackFileSystem::enumerateFiles(
    const std::filesystem::path&,
    const std::vector<std::string>&,
    caustica::enumerate_callback_t,
    bool)
{
    return caustica::status::NotImplemented;
}

int ShaderPackFileSystem::enumerateDirectories(
    const std::filesystem::path&,
    caustica::enumerate_callback_t,
    bool)
{
    return caustica::status::NotImplemented;
}
