#pragma once

#include <core/vfs/VFS.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

class ShaderPackFileSystem : public caustica::IFileSystem
{
public:
    ShaderPackFileSystem(
        const std::filesystem::path& packPath,
        const std::filesystem::path& virtualRoot = std::filesystem::path());
    ~ShaderPackFileSystem() override;

    [[nodiscard]] bool isOpen() const { return m_packFile != nullptr; }

    // Returns true when the pack should be mounted for ShaderDynamic/Bin lookups.
    // If loose bins exist on disk, at least one must also exist in the pack; otherwise
    // we assume a pack-only distribution layout.
    [[nodiscard]] bool hasDynamicBinLayout(const std::filesystem::path& diskBinApiRoot);

    bool folderExists(const std::filesystem::path& name) override;
    bool fileExists(const std::filesystem::path& name) override;
    std::shared_ptr<caustica::IBlob> readFile(const std::filesystem::path& name) override;
    bool writeFile(const std::filesystem::path& name, const void* data, size_t size) override;
    int enumerateFiles(
        const std::filesystem::path& path,
        const std::vector<std::string>& extensions,
        caustica::enumerate_callback_t callback,
        bool allowDuplicates = false) override;
    int enumerateDirectories(
        const std::filesystem::path& path,
        caustica::enumerate_callback_t callback,
        bool allowDuplicates = false) override;

private:
    struct PackKey
    {
        uint64_t h0 = 0;
        uint64_t h1 = 0;

        bool operator==(const PackKey& other) const
        {
            return h0 == other.h0 && h1 == other.h1;
        }
    };

    struct PackKeyHash
    {
        size_t operator()(const PackKey& key) const noexcept
        {
            return size_t(key.h0 ^ (key.h1 + 0x9e3779b97f4a7c15ull + (key.h0 << 6) + (key.h0 >> 2)));
        }
    };

    struct FileEntry
    {
        uint64_t offset = 0;
        uint64_t size = 0;
    };

    static PackKey hashPath(const std::string& logicalPath);
    static void decodePayload(uint8_t* data, size_t size, const PackKey& key);

    std::string normalizeLogicalPath(const std::filesystem::path& name) const;

private:
    std::filesystem::path m_packPath;
    std::filesystem::path m_virtualRoot;
    FILE* m_packFile = nullptr;
    std::mutex m_mutex;
    std::unordered_map<PackKey, FileEntry, PackKeyHash> m_entries;
};

