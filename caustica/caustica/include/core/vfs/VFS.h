#pragma once

#include <memory>
#include <string>
#include <filesystem>
#include <functional>
#include <vector>

/* 
Caustica Virtual File System (VFS) main classes.

The VFS provides read and sometimes write access to entire files stored in a
real file system, mounted into a virtual tree, stored in archives or resources.
*/

namespace caustica
{
    namespace status
    {
        constexpr int OK = 0;
        constexpr int Failed = -1;
        constexpr int PathNotFound = -2;
        constexpr int NotImplemented = -3;
    }

    typedef const std::function<void(std::string_view)>& enumerate_callback_t;

    inline std::function<void(std::string_view)> enumerate_to_vector(std::vector<std::string>& v)
    {
        return [&v](std::string_view s) { v.push_back(std::string(s)); };
    }

    // A blob is a package for untyped data, typically read from a file.
    class IBlob
    {
    public:
        virtual ~IBlob() = default;
        [[nodiscard]] virtual const void* data() const = 0;
        [[nodiscard]] virtual size_t size() const = 0;

        // Returns true if the provided blob contains no data.
        // Note: the previous version of this function was called IsEmpty (capital I)
        // and it returned the inverse of what it was supposed to.
        static bool isEmpty(IBlob const* blob)
        {
            return blob == nullptr || blob->data() == nullptr || blob->size() == 0;
        }
    };

    // Specific blob implementation that owns the data and frees it when deleted.
    class Blob : public IBlob
    {
    private:
        void* m_data;
        size_t m_size;

    public:
        Blob(void* data, size_t size);
        ~Blob() override;
        [[nodiscard]] const void* data() const override;
        [[nodiscard]] size_t size() const override;
    };

    // Basic interface for the virtual file system.
    class IFileSystem
    {
    public:
        virtual ~IFileSystem() = default;

        // Test if a folder exists.
        virtual bool folderExists(const std::filesystem::path& name) = 0;

        // Test if a file exists.
        virtual bool fileExists(const std::filesystem::path& name) = 0;

        // read the entire file.
        // Returns nullptr if the file cannot be read.
        virtual std::shared_ptr<IBlob> readFile(const std::filesystem::path& name) = 0;

        // write the entire file.
        // Returns false if the file cannot be written.
        virtual bool writeFile(const std::filesystem::path& name, const void* data, size_t size) = 0;

        // Search for files with any of the provided 'extensions' in 'path'.
        // Extensions should not include any wildcard characters.
        // Returns the number of files found, or a negative number on errors - see caustica::status.
        // The file names, relative to the 'path', are passed to 'callback' in no particular order.
        virtual int enumerateFiles(const std::filesystem::path& path, const std::vector<std::string>& extensions, enumerate_callback_t callback, bool allowDuplicates = false) = 0;

        // Search for directories in 'path'.
        // Returns the number of directories found, or a negative number on errors - see caustica::status.
        // The directory names, relative to the 'path', are passed to 'callback' in no particular order.
        virtual int enumerateDirectories(const std::filesystem::path& path, enumerate_callback_t callback, bool allowDuplicates = false) = 0;
    };

    // An implementation of virtual file system that directly maps to the OS files.
    class NativeFileSystem : public IFileSystem
    {
    public:
		bool folderExists(const std::filesystem::path& name) override;
        bool fileExists(const std::filesystem::path& name) override;
        std::shared_ptr<IBlob> readFile(const std::filesystem::path& name) override;
        bool writeFile(const std::filesystem::path& name, const void* data, size_t size) override;
        int enumerateFiles(const std::filesystem::path& path, const std::vector<std::string>& extensions, enumerate_callback_t callback, bool allowDuplicates = false) override;
        int enumerateDirectories(const std::filesystem::path& path, enumerate_callback_t callback, bool allowDuplicates = false) override;
    };

    // A layer that represents some path in the underlying file system as an entire FS.
    // Effectively, just prepends the provided base path to every file name
    // and passes the requests to the underlying FS.
    class RelativeFileSystem : public IFileSystem
    {
    private:
        std::shared_ptr<IFileSystem> m_UnderlyingFS;
        std::filesystem::path m_BasePath;
    public:
        RelativeFileSystem(std::shared_ptr<IFileSystem> fs, const std::filesystem::path& basePath);

        [[nodiscard]] std::filesystem::path const& GetBasePath() const { return m_BasePath; }

        bool folderExists(const std::filesystem::path& name) override;
        bool fileExists(const std::filesystem::path& name) override;
        std::shared_ptr<IBlob> readFile(const std::filesystem::path& name) override;
        bool writeFile(const std::filesystem::path& name, const void* data, size_t size) override;
        int enumerateFiles(const std::filesystem::path& path, const std::vector<std::string>& extensions, enumerate_callback_t callback, bool allowDuplicates = false) override;
        int enumerateDirectories(const std::filesystem::path& path, enumerate_callback_t callback, bool allowDuplicates = false) override;
    };

    // A virtual file system that allows mounting, or attaching, other VFS objects to paths.
    // Does not have any file systems by default, all of them must be mounted first.
    class RootFileSystem : public IFileSystem
    {
    private:
        std::vector<std::pair<std::string, std::shared_ptr<IFileSystem>>> m_MountPoints;

        bool findMountPoint(const std::filesystem::path& path, std::filesystem::path* pRelativePath, IFileSystem** ppFS);
    public:
        void mount(const std::filesystem::path& path, std::shared_ptr<IFileSystem> fs);
        void mount(const std::filesystem::path& path, const std::filesystem::path& nativePath);
        bool unmount(const std::filesystem::path& path);

		bool folderExists(const std::filesystem::path& name) override;
        bool fileExists(const std::filesystem::path& name) override;
        std::shared_ptr<IBlob> readFile(const std::filesystem::path& name) override;
        bool writeFile(const std::filesystem::path& name, const void* data, size_t size) override;
        int enumerateFiles(const std::filesystem::path& path, const std::vector<std::string>& extensions, enumerate_callback_t callback, bool allowDuplicates = false) override;
        int enumerateDirectories(const std::filesystem::path& path, enumerate_callback_t callback, bool allowDuplicates = false) override;
    };

    std::string getFileSearchRegex(const std::filesystem::path& path, const std::vector<std::string>& extensions);
}