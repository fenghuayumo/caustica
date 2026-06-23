#include <core/vfs/TarFile.h>
#include <core/log.h>
#include <sstream>
#include <regex>
#include <cstring>

#ifdef WIN32
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

using namespace caustica;

struct header_posix_ustar
{
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

static_assert(sizeof(header_posix_ustar) == 512);

TarFile::TarFile(const std::filesystem::path& archivePath)
{
    m_ArchivePath = archivePath.lexically_normal().generic_string();
    m_ArchiveFile = fopen(m_ArchivePath.c_str(), "rb");

    if (m_ArchiveFile)
    {
        bool errors = false;

        fseek(m_ArchiveFile, 0, SEEK_END);
        size_t archiveSize = ftello(m_ArchiveFile);
        
        size_t currentPosition = 0;

        while (currentPosition + sizeof(header_posix_ustar) <= archiveSize)
        {
            fseeko(m_ArchiveFile, currentPosition, SEEK_SET);

            header_posix_ustar header{};
            if (fread(&header, sizeof(header), 1, m_ArchiveFile) != 1)
                break;

            currentPosition += sizeof(header);

            // check if this is a regular file
            if (header.typeflag != '0' && header.typeflag != 0)
                continue;

            // combine the file name from prefix and name
            char fileName[sizeof(header.name) + sizeof(header.prefix) + 2];
            size_t prefixLength = strnlen(header.prefix, sizeof(header.prefix));
            size_t nameLength = strnlen(header.name, sizeof(header.name));
            if (prefixLength)
            {
                memcpy(fileName, header.prefix, prefixLength);
                fileName[prefixLength] = '/';
                ++prefixLength;
            }
            if (nameLength)
            {
                memcpy(fileName + prefixLength, header.name, nameLength);
            }
            fileName[nameLength + prefixLength] = 0;

            if (fileName[0] == 0)
                continue;

            // parse the octal size
            size_t fileSize = 0;
            for (char c : header.size)
            {
                if (c < '0' || c > '7')
                    break;

                fileSize = (fileSize << 3) | (c - '0');
            }

            if (fileSize == 0)
                continue;

            // validate the size
            if (currentPosition + fileSize > archiveSize)
            {
                caustica::warning("Malformed tar archive '%s': file '%s' size (%ull bytes) exceeds the archive range",
                    m_ArchivePath.c_str(), fileName, fileSize);
                errors = true;
                break;
            }

            // store the info about this file in the archive
            FileEntry entry;
            entry.offset = currentPosition;
            entry.size = fileSize;
            m_Files[fileName] = entry;

            std::filesystem::path filePath = fileName;
            if (filePath.has_parent_path())
                m_Directories.insert(filePath.parent_path().generic_string());

            // advance to the next file
            currentPosition += (fileSize + 511) & ~511;
        }

        if (errors)
        {
            fclose(m_ArchiveFile);
            m_ArchiveFile = nullptr;
            m_Files.clear();
            m_Directories.clear();
        }
    }
}

TarFile::~TarFile()
{
    // make sure we're not closing the file while some other thread is reading from it
    std::lock_guard<std::mutex> lockGuard(m_Mutex);

    if (m_ArchiveFile)
    {
        fclose(m_ArchiveFile);
        m_ArchiveFile = nullptr;
    }
}

bool TarFile::isOpen() const
{
    return m_ArchiveFile != nullptr;
}

bool TarFile::folderExists(const std::filesystem::path& name)
{
    std::string normalizedName = name.lexically_normal().relative_path().generic_string();

    return m_Directories.find(normalizedName) != m_Directories.end();
}

bool TarFile::fileExists(const std::filesystem::path& name)
{
    std::string normalizedName = name.lexically_normal().relative_path().generic_string();

    return m_Files.find(normalizedName) != m_Files.end();
}

std::shared_ptr<IBlob> TarFile::readFile(const std::filesystem::path& name)
{
    std::string normalizedName = name.lexically_normal().relative_path().generic_string();
    
    if (normalizedName.empty())
        return nullptr;
    
    auto entry = m_Files.find(normalizedName);

    if (entry == m_Files.end())
        return nullptr;

    // prevent concurrent file operations from multiple threads from this point on
    std::lock_guard<std::mutex> lockGuard(m_Mutex);
    
    if (fseeko(m_ArchiveFile, entry->second.offset, SEEK_SET) != 0)
    {
        caustica::warning("Error seeking to offset %ull for file '%s' in tar archive '%s'",
            entry->second.offset, normalizedName.c_str(), m_ArchivePath.c_str());
        return nullptr;
    }

    void* data = malloc(entry->second.size);

    if (!data)
        return nullptr;

    size_t sizeRead = fread(data, 1, entry->second.size, m_ArchiveFile);

    if (sizeRead != entry->second.size)
    {
        caustica::warning("Error reading file '%s' (%ull bytes) from tar archive '%s'", 
            entry->second.size, normalizedName.c_str(), m_ArchivePath.c_str());
        free(data);
        return nullptr;
    }

    std::shared_ptr<Blob> blob = std::make_shared<Blob>(data, entry->second.size);

    return std::static_pointer_cast<IBlob>(blob);
}

bool TarFile::writeFile(const std::filesystem::path&, const void*, size_t)
{
    // tar files are mounted read-only
    return false;
}

int TarFile::enumerateFiles(const std::filesystem::path& path, const std::vector<std::string>& extensions, enumerate_callback_t callback, bool allowDuplicates)
{
    (void)allowDuplicates;
    std::basic_regex<char> regex(getFileSearchRegex(path.relative_path(), extensions));

    int numEntries = 0;
    for (const auto& [name, record] : m_Files)
    {
        if (std::regex_match(name, regex))
        {
            std::filesystem::path filePath = name;
            callback(filePath.filename().generic_string());
            ++numEntries;
        }
    }

    return numEntries;
}

int TarFile::enumerateDirectories(const std::filesystem::path& path, enumerate_callback_t callback, bool allowDuplicates)
{
    (void)allowDuplicates;
    std::filesystem::path normalizedPath = path.relative_path().lexically_normal();

    int numEntries = 0;
    for (const auto& name : m_Directories)
    {
        std::filesystem::path dirPath = name;
        if (dirPath.parent_path() == normalizedPath)
            callback(dirPath.filename().generic_string());
        ++numEntries;
    }
    
    return numEntries;
}
