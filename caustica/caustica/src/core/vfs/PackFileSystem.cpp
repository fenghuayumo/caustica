#include <core/vfs/PackFileSystem.h>

#include <core/log.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace caustica
{
    namespace
    {
        constexpr char kPackMagic[8] = { 'C', 'A', 'U', 'S', 'T', 'I', 'C', 'P' };
        constexpr uint32_t kPackFormatVersion = 1;
        constexpr size_t kPackHeaderSize = 64;

        std::string toGenericKey(const std::filesystem::path& path)
        {
            return path.generic_string();
        }

        std::string foldKey(const std::string& key)
        {
            std::string folded = key;
            std::transform(
                folded.begin(),
                folded.end(),
                folded.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return folded;
        }

        uint32_t crc32Range(const void* data, size_t size)
        {
            static uint32_t table[256] = {};
            static bool tableInit = false;
            if (!tableInit)
            {
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t crc = i;
                    for (int bit = 0; bit < 8; ++bit)
                        crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
                    table[i] = crc;
                }
                tableInit = true;
            }

            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            uint32_t crc = 0xFFFFFFFFu;
            for (size_t i = 0; i < size; ++i)
                crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
            return crc ^ 0xFFFFFFFFu;
        }

        uint16_t readU16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
        uint32_t readU32(const uint8_t* p)
        {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }
        uint64_t readU64(const uint8_t* p)
        {
            return uint64_t(readU32(p)) | (uint64_t(readU32(p + 4)) << 32);
        }

        // Minimal LZ4 block decoder (raw block, not framed). Returns true only
        // when exactly dstCapacity bytes were produced.
        bool lz4DecompressBlock(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstCapacity)
        {
            const uint8_t* srcPtr = src;
            const uint8_t* const srcEnd = src + srcSize;
            uint8_t* dstPtr = dst;
            uint8_t* const dstEnd = dst + dstCapacity;

            while (srcPtr < srcEnd)
            {
                const uint8_t token = *srcPtr++;

                size_t literalLength = size_t(token >> 4);
                if (literalLength == 15)
                {
                    uint8_t byte = 255;
                    while (byte == 255)
                    {
                        if (srcPtr >= srcEnd)
                            return false;
                        byte = *srcPtr++;
                        literalLength += byte;
                    }
                }
                if (size_t(srcEnd - srcPtr) < literalLength || size_t(dstEnd - dstPtr) < literalLength)
                    return false;
                std::memcpy(dstPtr, srcPtr, literalLength);
                srcPtr += literalLength;
                dstPtr += literalLength;
                if (srcPtr >= srcEnd)
                    break; // trailing literals terminate the block

                if (srcEnd - srcPtr < 2)
                    return false;
                const uint16_t offset = readU16(srcPtr);
                srcPtr += 2;
                if (offset == 0 || size_t(dstPtr - dst) < offset)
                    return false;

                size_t matchLength = size_t(token & 0x0F) + 4;
                if ((token & 0x0F) == 15)
                {
                    uint8_t byte = 255;
                    while (byte == 255)
                    {
                        if (srcPtr >= srcEnd)
                            return false;
                        byte = *srcPtr++;
                        matchLength += byte;
                    }
                }
                if (size_t(dstEnd - dstPtr) < matchLength)
                    return false;

                const uint8_t* matchPtr = dstPtr - offset;
                // Byte-wise copy: matches may overlap (offset < matchLength).
                for (size_t i = 0; i < matchLength; ++i)
                    dstPtr[i] = matchPtr[i];
                dstPtr += matchLength;
            }

            return dstPtr == dstEnd;
        }
    } // namespace

    PackFileSystem::PackFileSystem(const std::filesystem::path& packPath, std::shared_ptr<IFileSystem> fallback)
        : m_packPath(packPath)
        , m_packKeyPath(std::filesystem::absolute(packPath))
        , m_fallback(std::move(fallback))
    {
        m_open = parse();
        if (!m_open)
            caustica::error("PackFileSystem: failed to open scene pack '%s'.", m_packPath.generic_string().c_str());
    }

    PackFileSystem::~PackFileSystem() = default;

    bool PackFileSystem::parse()
    {
        std::FILE* file = nullptr;
#ifdef _WIN32
        if (_wfopen_s(&file, m_packPath.c_str(), L"rb") != 0 || !file)
            return false;
#else
        file = std::fopen(m_packPath.string().c_str(), "rb");
        if (!file)
            return false;
#endif

        bool ok = false;
        if (std::fseek(file, 0, SEEK_END) == 0)
        {
            const long fileSize = std::ftell(file);
            if (fileSize > 0 && std::fseek(file, 0, SEEK_SET) == 0)
            {
                m_fileData.resize(size_t(fileSize));
                ok = std::fread(m_fileData.data(), 1, m_fileData.size(), file) == m_fileData.size();
            }
        }
        std::fclose(file);
        if (!ok || m_fileData.size() < kPackHeaderSize)
            return false;

        const uint8_t* header = m_fileData.data();
        if (std::memcmp(header, kPackMagic, 8) != 0)
            return false;
        if (readU32(header + 8) != kPackFormatVersion)
            return false;

        const uint64_t indexOffset = readU64(header + 16);
        const uint64_t indexSize = readU64(header + 24);
        const uint32_t indexCrc = readU32(header + 40);
        if (indexOffset < kPackHeaderSize || indexSize < 4
            || indexOffset + indexSize > m_fileData.size())
            return false;
        const uint8_t* directory = m_fileData.data() + indexOffset;
        if (crc32Range(directory, size_t(indexSize)) != indexCrc)
            return false;

        uint32_t entryCount = readU32(directory);
        const uint8_t* cursor = directory + 4;
        const uint8_t* const dirEnd = directory + indexSize;
        m_entries.reserve(entryCount);
        for (uint32_t i = 0; i < entryCount; ++i)
        {
            if (cursor + 2 > dirEnd)
                return false;
            const uint16_t pathLen = readU16(cursor);
            cursor += 2;
            if (cursor + pathLen + 30 > dirEnd)
                return false;

            Entry entry;
            entry.path.assign(reinterpret_cast<const char*>(cursor), pathLen);
            cursor += pathLen;
            entry.type = *cursor++;
            entry.compression = *cursor++;
            entry.crc32 = readU32(cursor);
            cursor += 4;
            entry.uncompSize = readU64(cursor);
            cursor += 8;
            entry.compSize = readU64(cursor);
            cursor += 8;
            entry.offset = readU64(cursor);
            cursor += 8;
            cursor += 7; // reserved

            if (entry.offset + entry.compSize > m_fileData.size())
                return false;

            std::string exact;
            std::string folded;
            buildLookupKeys(entry, exact, folded);
            const size_t index = m_entries.size();
            m_entries.push_back(std::move(entry));
            m_byExactPath.emplace(std::move(exact), index);
            // Copy (not move): 'folded' also seeds the suffix index below.
            m_byFoldedPath.emplace(folded, index);
            // Suffix index: register each trailing path suffix starting at a
            // '/' separator. Keeps absolute resolved media paths resolvable.
            for (size_t pos = folded.find('/'); pos != std::string::npos; pos = folded.find('/', pos + 1))
            {
                m_bySuffix.emplace(folded.substr(pos + 1), index);
            }
        }

        // Primary scene from the manifest entry.
        for (const Entry& entry : m_entries)
        {
            if (entry.type == 7 /* manifest */)
            {
                std::shared_ptr<IBlob> blob = readEntry(entry);
                if (blob)
                {
                    // Extract "primaryScene": "..." without a JSON dependency.
                    const std::string_view text(static_cast<const char*>(blob->data()), blob->size());
                    constexpr std::string_view needle = "\"primaryScene\"";
                    if (const size_t at = text.find(needle); at != std::string_view::npos)
                    {
                        const size_t colon = text.find(':', at + needle.size());
                        const size_t quote1 = colon == std::string_view::npos
                            ? std::string_view::npos
                            : text.find('"', colon + 1);
                        const size_t quote2 = quote1 == std::string_view::npos
                            ? std::string_view::npos
                            : text.find('"', quote1 + 1);
                        if (quote1 != std::string_view::npos && quote2 != std::string_view::npos)
                            m_primarySceneEntry = std::string(text.substr(quote1 + 1, quote2 - quote1 - 1));
                    }
                }
                break;
            }
        }
        return true;
    }

    void PackFileSystem::buildLookupKeys(const Entry& entry, std::string& exact, std::string& folded)
    {
        exact = entry.path;
        folded = foldKey(entry.path);
    }

    const PackFileSystem::Entry* PackFileSystem::findEntry(const std::filesystem::path& path) const
    {
        const std::string generic = toGenericKey(path);
        if (generic.empty())
            return nullptr;

        if (auto it = m_byExactPath.find(generic); it != m_byExactPath.end())
            return &m_entries[it->second];

        const std::string folded = foldKey(generic);
        if (auto it = m_byFoldedPath.find(folded); it != m_byFoldedPath.end())
            return &m_entries[it->second];

        // Normalized absolute form (handles ".." segments and CWD-relative
        // inputs) before the suffix walk.
        const std::string absoluteFolded =
            foldKey(toGenericKey(std::filesystem::absolute(path).lexically_normal()));
        if (auto it = m_byExactPath.find(absoluteFolded); it != m_byExactPath.end())
            return &m_entries[it->second];
        if (auto it = m_byFoldedPath.find(absoluteFolded); it != m_byFoldedPath.end())
            return &m_entries[it->second];

        // Suffix lookup: try each trailing path segment of the request. The
        // index maps "models/x/y.bin" style entry tails to entries, so an
        // absolute resolved path like <root>/Assets/models/x/y.bin finds the
        // entry after its "Assets/" boundary.
        for (size_t pos = absoluteFolded.find('/'); pos != std::string::npos; pos = absoluteFolded.find('/', pos + 1))
        {
            if (auto it = m_bySuffix.find(absoluteFolded.substr(pos + 1)); it != m_bySuffix.end())
                return &m_entries[it->second];
        }

        return nullptr;
    }

    const PackFileSystem::Entry* PackFileSystem::findPrimaryForPackPath(const std::filesystem::path& path) const
    {
        if (m_primarySceneEntry.empty())
            return nullptr;
        const std::string generic = toGenericKey(std::filesystem::absolute(path));
        const std::string packGeneric = toGenericKey(m_packKeyPath);
        const std::string foldedGeneric = foldKey(generic);
        const std::string foldedPack = foldKey(packGeneric);
        if (foldedGeneric == foldedPack)
        {
            if (auto it = m_byExactPath.find(m_primarySceneEntry); it != m_byExactPath.end())
                return &m_entries[it->second];
            if (auto it = m_byFoldedPath.find(foldKey(m_primarySceneEntry)); it != m_byFoldedPath.end())
                return &m_entries[it->second];
        }
        return nullptr;
    }

    std::shared_ptr<IBlob> PackFileSystem::readEntry(const Entry& entry) const
    {
        const uint8_t* src = m_fileData.data() + entry.offset;
        void* buffer = std::malloc(size_t(entry.uncompSize));
        if (!buffer)
            return nullptr;

        if (entry.compression == 1)
        {
            if (!lz4DecompressBlock(src, size_t(entry.compSize), static_cast<uint8_t*>(buffer), size_t(entry.uncompSize)))
            {
                caustica::error("PackFileSystem: LZ4 decode failed for '%s'.", entry.path.c_str());
                std::free(buffer);
                return nullptr;
            }
        }
        else
        {
            std::memcpy(buffer, src, size_t(entry.uncompSize));
        }
        return std::make_shared<Blob>(buffer, size_t(entry.uncompSize));
    }

    bool PackFileSystem::folderExists(const std::filesystem::path& name)
    {
        const std::string folded = foldKey(toGenericKey(name));
        if (!folded.empty())
        {
            const std::string prefix = folded.back() == '/' ? folded : folded + "/";
            for (const Entry& entry : m_entries)
            {
                if (foldKey(entry.path).starts_with(prefix))
                    return true;
            }
        }
        return m_fallback ? m_fallback->folderExists(name) : false;
    }

    bool PackFileSystem::fileExists(const std::filesystem::path& name)
    {
        if (findEntry(name) || findPrimaryForPackPath(name))
            return true;
        return m_fallback ? m_fallback->fileExists(name) : false;
    }

    std::shared_ptr<IBlob> PackFileSystem::readFile(const std::filesystem::path& name)
    {
        if (const Entry* primary = findPrimaryForPackPath(name))
            return readEntry(*primary);
        if (const Entry* entry = findEntry(name))
            return readEntry(*entry);
        if (m_fallback)
            return m_fallback->readFile(name);
        return nullptr;
    }

    bool PackFileSystem::writeFile(const std::filesystem::path&, const void*, size_t)
    {
        // Scene packs are read-only bundles.
        return false;
    }

    int PackFileSystem::enumerateFiles(
        const std::filesystem::path& path,
        const std::vector<std::string>& extensions,
        enumerate_callback_t callback,
        bool allowDuplicates)
    {
        const std::string prefix = foldKey(toGenericKey(path));
        std::vector<std::string> results;
        for (const Entry& entry : m_entries)
        {
            const std::string folded = foldKey(entry.path);
            if (!prefix.empty() && !folded.starts_with(prefix + "/"))
                continue;
            if (!extensions.empty())
            {
                const size_t dot = folded.rfind('.');
                const std::string ext = dot == std::string::npos ? std::string() : folded.substr(dot);
                bool matched = false;
                for (const std::string& candidate : extensions)
                {
                    std::string candidateFolded = foldKey(candidate);
                    if (!candidateFolded.empty() && candidateFolded[0] != '.')
                        candidateFolded = "." + candidateFolded;
                    if (ext == candidateFolded)
                    {
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                    continue;
            }
            results.push_back(entry.path);
        }
        if (m_fallback)
            m_fallback->enumerateFiles(path, extensions, enumerate_to_vector(results), true);

        if (allowDuplicates)
        {
            for (const std::string& item : results)
                callback(item);
            return int(results.size());
        }

        std::sort(results.begin(), results.end());
        results.erase(std::unique(results.begin(), results.end()), results.end());
        for (const std::string& item : results)
            callback(item);
        return int(results.size());
    }

    int PackFileSystem::enumerateDirectories(
        const std::filesystem::path& path,
        enumerate_callback_t callback,
        bool allowDuplicates)
    {
        const std::string prefix = foldKey(toGenericKey(path));
        std::vector<std::string> results;
        for (const Entry& entry : m_entries)
        {
            std::string folded = foldKey(entry.path);
            if (!prefix.empty())
            {
                if (!folded.starts_with(prefix + "/"))
                    continue;
                folded = folded.substr(prefix.size() + 1);
            }
            const size_t slash = folded.find('/');
            if (slash != std::string::npos)
                results.push_back(folded.substr(0, slash));
        }
        std::sort(results.begin(), results.end());
        results.erase(std::unique(results.begin(), results.end()), results.end());
        for (const std::string& item : results)
            callback(item);
        return int(results.size());
    }

} // namespace caustica
