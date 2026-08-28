#include <assets/loader/ShaderDependencyIndex.h>
#include <assets/loader/ShaderCompilerUtils.h>

#include <core/log.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace caustica
{

namespace
{
    // Must match DependencyManifest in support/python/shader_cook_cache.py.
    constexpr const char* k_manifestMagic = "CAUSDEP1";
    constexpr const char* k_missingMarker = "<missing>";

    std::string normalizeSeparators(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        return path;
    }
}

void ShaderDependencyIndex::initialize(
    std::filesystem::path manifestPath,
    std::filesystem::path repoRoot)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_manifestPath = std::move(manifestPath);
    m_repoRoot = std::move(repoRoot);
    m_closures.clear();
    m_bins.clear();
    m_fileHashCache.clear();
    m_fingerprintCache.clear();
    m_changeSnapshot.clear();
    m_loaded = false;
    m_dirty = false;
}

bool ShaderDependencyIndex::load()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_closures.clear();
    m_bins.clear();
    m_fileHashCache.clear();
    m_fingerprintCache.clear();
    m_loaded = false;

    std::ifstream stream(m_manifestPath, std::ios::binary);
    if (!stream.is_open())
        return false;

    std::string line;
    if (!std::getline(stream, line))
        return false;
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line != k_manifestMagic)
    {
        caustica::warning(
            "Shader dependency manifest '%s' has unexpected header '%s'; ignoring it.",
            m_manifestPath.string().c_str(), line.c_str());
        return false;
    }

    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() < 3)
            continue;

        std::istringstream fields(line.substr(2));
        if (line[0] == 'D')
        {
            std::string source, include;
            if (fields >> source >> include)
                m_closures[source].push_back(include);
        }
        else if (line[0] == 'B')
        {
            std::string cacheHash, source, fingerprint;
            if (fields >> cacheHash >> source >> fingerprint)
                m_bins[cacheHash] = BinRecord{ source, fingerprint };
        }
    }

    // The cook writes closures sorted; sorting here keeps the fingerprint stable even
    // if a future writer does not.
    for (auto& [source, includes] : m_closures)
        std::sort(includes.begin(), includes.end());

    m_loaded = true;
    caustica::info(
        "Shader dependency manifest: %zu root sources, %zu binaries ('%s').",
        m_closures.size(), m_bins.size(), m_manifestPath.string().c_str());
    return true;
}

bool ShaderDependencyIndex::save()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_dirty || m_manifestPath.empty())
        return false;

    std::error_code ec;
    std::filesystem::create_directories(m_manifestPath.parent_path(), ec);

    const std::filesystem::path staging = m_manifestPath.string() + ".tmp";
    {
        std::ofstream stream(staging, std::ios::binary | std::ios::trunc);
        if (!stream.is_open())
        {
            caustica::warning(
                "Could not write shader dependency manifest '%s'.",
                m_manifestPath.string().c_str());
            return false;
        }

        stream << k_manifestMagic << '\n';

        std::vector<std::string> sources;
        sources.reserve(m_closures.size());
        for (const auto& [source, _] : m_closures)
            sources.push_back(source);
        std::sort(sources.begin(), sources.end());
        for (const std::string& source : sources)
        {
            for (const std::string& include : m_closures.at(source))
                stream << "D " << source << ' ' << include << '\n';
        }

        std::vector<std::string> hashes;
        hashes.reserve(m_bins.size());
        for (const auto& [cacheHash, _] : m_bins)
            hashes.push_back(cacheHash);
        std::sort(hashes.begin(), hashes.end());
        for (const std::string& cacheHash : hashes)
        {
            const BinRecord& record = m_bins.at(cacheHash);
            stream << "B " << cacheHash << ' ' << record.sourceRelPath << ' '
                   << record.fingerprint << '\n';
        }
    }

    std::filesystem::rename(staging, m_manifestPath, ec);
    if (ec)
    {
        std::filesystem::remove(staging, ec);
        return false;
    }

    m_dirty = false;
    return true;
}

std::string ShaderDependencyIndex::fileHashLocked(const std::string& relPath)
{
    auto cached = m_fileHashCache.find(relPath);
    if (cached != m_fileHashCache.end())
        return cached->second;

    std::string digest = k_missingMarker;
    std::ifstream stream(m_repoRoot / relPath, std::ios::binary);
    if (stream.is_open())
    {
        std::ostringstream contents;
        contents << stream.rdbuf();
        digest = ShaderCompilerUtils::computeSha256Hex(contents.str());
    }

    m_fileHashCache.emplace(relPath, digest);
    return digest;
}

std::string ShaderDependencyIndex::fingerprintLocked(const std::string& sourceRelPath)
{
    auto cached = m_fingerprintCache.find(sourceRelPath);
    if (cached != m_fingerprintCache.end())
        return cached->second;

    auto closure = m_closures.find(sourceRelPath);
    if (closure == m_closures.end())
        return {};

    // Combination rule mirrors DependencyManifest.fingerprint(): "<relpath>:<sha256>"
    // per closure entry, sorted by path, joined with newlines, then hashed.
    std::string payload;
    payload.reserve(closure->second.size() * 80);
    for (const std::string& include : closure->second)
    {
        if (!payload.empty())
            payload += '\n';
        payload += include;
        payload += ':';
        payload += fileHashLocked(include);
    }

    std::string fingerprint = ShaderCompilerUtils::computeSha256Hex(payload);
    m_fingerprintCache.emplace(sourceRelPath, fingerprint);
    return fingerprint;
}

std::string ShaderDependencyIndex::currentFingerprint(const std::string& sourceRelPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return fingerprintLocked(sourceRelPath);
}

bool ShaderDependencyIndex::isUpToDate(const std::string& cacheHashHex)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto record = m_bins.find(cacheHashHex);
    if (record == m_bins.end())
        return false;

    const std::string fingerprint = fingerprintLocked(record->second.sourceRelPath);
    return !fingerprint.empty() && fingerprint == record->second.fingerprint;
}

void ShaderDependencyIndex::record(
    const std::string& cacheHashHex,
    const std::string& sourceRelPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // A shader compiled at runtime may use a source the cook never saw. Without a
    // closure there is nothing to fingerprint, so leave it unrecorded and let it be
    // recompiled next run rather than assert freshness we cannot back up.
    const std::string fingerprint = fingerprintLocked(sourceRelPath);
    if (fingerprint.empty())
        return;

    m_bins[cacheHashHex] = BinRecord{ sourceRelPath, fingerprint };
    m_dirty = true;
}

void ShaderDependencyIndex::invalidateContentCache()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_fileHashCache.clear();
    m_fingerprintCache.clear();
}

std::vector<std::string> ShaderDependencyIndex::takeChangedSources()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_fileHashCache.clear();
    m_fingerprintCache.clear();

    std::vector<std::string> changed;
    for (const auto& [source, _] : m_closures)
    {
        const std::string fingerprint = fingerprintLocked(source);
        auto previous = m_changeSnapshot.find(source);
        if (previous == m_changeSnapshot.end())
        {
            m_changeSnapshot.emplace(source, fingerprint);
            continue;
        }
        if (previous->second != fingerprint)
        {
            previous->second = fingerprint;
            changed.push_back(source);
        }
    }
    return changed;
}

std::string ShaderDependencyIndex::toRelative(const std::filesystem::path& absolute) const
{
    std::error_code ec;
    const std::filesystem::path relative =
        std::filesystem::relative(absolute, m_repoRoot, ec);
    if (ec || relative.empty())
        return normalizeSeparators(absolute.generic_string());
    return normalizeSeparators(relative.generic_string());
}

} // namespace caustica
