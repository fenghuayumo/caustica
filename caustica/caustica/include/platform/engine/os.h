#pragma once

#include <filesystem>
#include <string>

namespace caustica
{

// =============================================================================
// OS — Platform singleton. One concrete impl per platform (WindowsOS, UnixOS).
// Must call initialize() once before get().
// =============================================================================
class OS
{
public:
    virtual ~OS() = default;

    static void initialize();
    static OS& get();

    virtual std::filesystem::path getExecutablePath() const = 0;
    virtual std::filesystem::path getExecutableDirectory() const;

    virtual void* loadLibrary(const std::string& path) const = 0;
    virtual void  unloadLibrary(void* handle) const = 0;
    virtual void* getLibrarySymbol(void* handle, const std::string& name) const = 0;

protected:
    OS() = default;

private:
    static OS* s_Instance;
};

#ifdef _WIN32
class WindowsOS : public OS
{
public:
    WindowsOS();

    std::filesystem::path getExecutablePath() const override;
    void* loadLibrary(const std::string& path) const override;
    void  unloadLibrary(void* handle) const override;
    void* getLibrarySymbol(void* handle, const std::string& name) const override;

private:
    void* m_HInstance;
};
#endif

} // namespace caustica
