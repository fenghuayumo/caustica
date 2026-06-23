#pragma once

#include <core/vfs/VFS.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace caustica
{
    /* 
    A read-only file system that provides access to files in a tar archive.
    The archive is partially read to enumerate the files when TarFile is created.
    TarFile can only operate on real files, i.e. underlying virtual file systems are not supported.
    Designed to work in combination with CompressionLayer to store packaged assets.
    */
    class TarFile : public IFileSystem
    {
    private:
        std::string m_ArchivePath;
        std::mutex m_Mutex;
        FILE* m_ArchiveFile = nullptr;

        struct FileEntry
        {
            size_t offset = 0;
            size_t size = 0;
        };

        std::unordered_map<std::string, FileEntry> m_Files;
        std::unordered_set<std::string> m_Directories;
        
    public:
        TarFile(const std::filesystem::path& archivePath);
        ~TarFile() override;

        [[nodiscard]] bool isOpen() const;
        
        bool folderExists(const std::filesystem::path& name) override;
        bool fileExists(const std::filesystem::path& name) override;
        std::shared_ptr<IBlob> readFile(const std::filesystem::path& name) override;
        bool writeFile(const std::filesystem::path& name, const void* data, size_t size) override;
        int enumerateFiles(const std::filesystem::path& path, const std::vector<std::string>& extensions, enumerate_callback_t callback, bool allowDuplicates = false) override;
        int enumerateDirectories(const std::filesystem::path& path, enumerate_callback_t callback, bool allowDuplicates = false) override;
    };
}
