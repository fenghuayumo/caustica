#pragma once

#include <core/vfs/VFS.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace caustica
{

// Read-only IFileSystem over a single self-contained `.caustica` scene pack
// (docs/scene-pack-format.md). Resolution order per readFile/fileExists:
//   1. exact entry match on the normalized generic path
//   2. case-insensitive entry match
//   3. suffix match of the requested path against entries
//   4. optional native fallback filesystem
// Reading the pack's own path yields the primary scene JSON so
// SceneManager::loadSceneToPending works unchanged when handed a `.caustica`.
class PackFileSystem final : public IFileSystem
{
public:
    // Opens and parses the pack immediately. Check isOpen() before use.
    explicit PackFileSystem(
        const std::filesystem::path& packPath,
        std::shared_ptr<IFileSystem> fallback = nullptr);
    ~PackFileSystem() override;

    PackFileSystem(const PackFileSystem&) = delete;
    PackFileSystem& operator=(const PackFileSystem&) = delete;

    [[nodiscard]] bool isOpen() const { return m_open; }
    [[nodiscard]] const std::filesystem::path& getPackPath() const { return m_packPath; }
    // Pack-relative path of the primary scene document (may be empty).
    [[nodiscard]] const std::string& getPrimarySceneEntry() const { return m_primarySceneEntry; }

    bool folderExists(const std::filesystem::path& name) override;
    bool fileExists(const std::filesystem::path& name) override;
    std::shared_ptr<IBlob> readFile(const std::filesystem::path& name) override;
    bool writeFile(const std::filesystem::path& name, const void* data, size_t size) override;
    int enumerateFiles(const std::filesystem::path& path, const std::vector<std::string>& extensions, enumerate_callback_t callback, bool allowDuplicates = false) override;
    int enumerateDirectories(const std::filesystem::path& path, enumerate_callback_t callback, bool allowDuplicates = false) override;

private:
    struct Entry
    {
        std::string path;
        uint32_t crc32 = 0;
        uint64_t uncompSize = 0;
        uint64_t compSize = 0;
        uint64_t offset = 0;
        uint8_t type = 0;
        uint8_t compression = 0;
    };

    bool parse();
    [[nodiscard]] const Entry* findEntry(const std::filesystem::path& path) const;
    [[nodiscard]] const Entry* findPrimaryForPackPath(const std::filesystem::path& path) const;
    [[nodiscard]] std::shared_ptr<IBlob> readEntry(const Entry& entry) const;
    static void buildLookupKeys(const Entry& entry, std::string& exact, std::string& folded);

    std::filesystem::path m_packPath;
    std::filesystem::path m_packKeyPath;
    std::shared_ptr<IFileSystem> m_fallback;
    std::vector<uint8_t> m_fileData;
    std::vector<Entry> m_entries;
    std::map<std::string, size_t> m_byExactPath;
    std::map<std::string, size_t> m_byFoldedPath;
    std::map<std::string, size_t> m_bySuffix;
    std::string m_primarySceneEntry;
    bool m_open = false;
};

} // namespace caustica
