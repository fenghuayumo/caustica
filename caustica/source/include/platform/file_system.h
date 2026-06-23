#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <functional>

namespace caustica
{

// Singleton providing platform file-system services.
// Abstracts away the underlying OS file APIs.
class FileSystem
{
public:
    static FileSystem& get()
    {
        static FileSystem instance;
        return instance;
    }

    static void release()
    {
        // Currently a no-op; placeholder for future cleanup
    }

    // Initialise the file system (called by CoreSystem)
    void init();

    // ------------------------------------------------------------------
    // Path helpers
    // ------------------------------------------------------------------
    bool fileExists(const std::filesystem::path& path) const;
    bool folderExists(const std::filesystem::path& path) const;
    bool createDirectory(const std::filesystem::path& path);

    // Resolve a relative path against the asset root
    void absolutePathToFileSystem(const std::string& relative, std::string& outAbsolute) const;

    // ------------------------------------------------------------------
    // File I/O
    // ------------------------------------------------------------------
    std::string readTextFile(const std::filesystem::path& path);
    bool writeTextFile(const std::filesystem::path& path, const std::string& content);

    std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path);
    bool writeBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& data);

    // ------------------------------------------------------------------
    // Directory enumeration
    // ------------------------------------------------------------------
    std::vector<std::filesystem::path> listFiles(const std::filesystem::path& directory,
                                                  const std::string& extension = "") const;
    std::vector<std::filesystem::path> listDirectories(const std::filesystem::path& directory) const;

    // ------------------------------------------------------------------
    // Asset root (for resolving relative paths)
    // ------------------------------------------------------------------
    void setAssetRoot(const std::string& root) { m_AssetRoot = root; }
    const std::string& getAssetRoot() const    { return m_AssetRoot; }

    // ------------------------------------------------------------------
    // Watch for file changes (returns true if supported)
    // ------------------------------------------------------------------
    using FileChangeCallback = std::function<void(const std::filesystem::path&)>;
    bool watchFile(const std::filesystem::path& path, FileChangeCallback callback);
    void stopWatching(const std::filesystem::path& path);
    void pollWatches();

private:
    FileSystem()  = default;
    ~FileSystem() = default;

    std::string m_AssetRoot;
};

} // namespace caustica
