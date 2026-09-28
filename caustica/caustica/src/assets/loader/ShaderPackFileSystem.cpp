#include <assets/loader/ShaderPackFileSystem.h>

#include <core/log.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <stb_image.h>
#include <vector>

#ifdef WIN32
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

namespace
{
    constexpr std::array<char, 8> c_ShaderPackMagic = { 'C', 'A', 'U', 'S', 'S', 'H', 'D', '1' };
    constexpr uint32_t c_ShaderPackVersion = 3;

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

    struct ShaderPackEntryV3
    {
        uint64_t hash0;
        uint64_t hash1;
        uint64_t contentHash0;
        uint64_t contentHash1;
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

    // Must match support/python/build_wheel.py:xorshift64star. The multiplied
    // value is written back into `state`; classic xorshift64* only returns it.
    // Leaving the unmultiplied value here decrypts only the first 8 bytes
    // (e.g. the CAUSSMF1 magic) and then reports the shader manifest truncated.
    static uint64_t XorShift64Star(uint64_t& state)
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        state *= 2685821657736338717ull;
        return state;
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
        (header.version != 1 && header.version != 2 && header.version != c_ShaderPackVersion))
    {
        caustica::warning("Shader pack '%s' has an unsupported format", m_packPath.string().c_str());
        fclose(m_packFile);
        m_packFile = nullptr;
        return;
    }
    m_packVersion = header.version;

    m_entries.reserve(header.entryCount);
    for (uint32_t index = 0; index < header.entryCount; ++index)
    {
        ShaderPackEntryV3 diskEntry{};
        if (header.version == 3)
        {
            if (fread(&diskEntry, sizeof(diskEntry), 1, m_packFile) != 1)
            {
                caustica::warning("Shader pack '%s' has a truncated entry table", m_packPath.string().c_str());
                fclose(m_packFile);
                m_packFile = nullptr;
                m_entries.clear();
                return;
            }
        }
        else
        {
            ShaderPackEntry oldEntry{};
            if (fread(&oldEntry, sizeof(oldEntry), 1, m_packFile) != 1)
            {
                caustica::warning("Shader pack '%s' has a truncated entry table", m_packPath.string().c_str());
                fclose(m_packFile);
                m_packFile = nullptr;
                m_entries.clear();
                return;
            }
            diskEntry = { oldEntry.hash0, oldEntry.hash1, oldEntry.hash0, oldEntry.hash1,
                oldEntry.offset, oldEntry.size };
        }

        PackKey key{ diskEntry.hash0, diskEntry.hash1 };
        m_entries[key] = FileEntry{ diskEntry.offset, diskEntry.size,
            PackKey{ diskEntry.contentHash0, diskEntry.contentHash1 } };
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

bool ShaderPackFileSystem::hasShaderBinLayout()
{
    return m_packFile && fileExists("manifest.bin");
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

    decodePayload(encodedData.data(), encodedData.size(), entryIt->second.contentKey);

    if (m_packVersion >= 2)
    {
        if (encodedData.size() < sizeof(uint64_t))
            return nullptr;
        uint64_t decodedSize = 0;
        std::memcpy(&decodedSize, encodedData.data(), sizeof(decodedSize));
        if (decodedSize > uint64_t(std::numeric_limits<int>::max()) ||
            encodedData.size() - sizeof(uint64_t) > size_t(std::numeric_limits<int>::max()))
        {
            caustica::warning("Compressed shader pack entry '%s' is too large", logicalPath.c_str());
            return nullptr;
        }
        void* blobData = malloc(size_t(std::max<uint64_t>(decodedSize, 1)));
        if (!blobData)
            return nullptr;
        const int decoded = stbi_zlib_decode_buffer(
            static_cast<char*>(blobData), int(decodedSize),
            reinterpret_cast<const char*>(encodedData.data() + sizeof(uint64_t)),
            int(encodedData.size() - sizeof(uint64_t)));
        if (decoded < 0 || uint64_t(decoded) != decodedSize)
        {
            free(blobData);
            caustica::warning("Unable to decompress shader pack entry '%s'", logicalPath.c_str());
            return nullptr;
        }
        return std::make_shared<caustica::Blob>(blobData, size_t(decodedSize));
    }

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
