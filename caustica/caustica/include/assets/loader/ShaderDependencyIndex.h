#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace caustica
{

/// Answers "is this compiled shader still current?" per shader, using the include
/// closure the cook recorded, instead of the newest timestamp anywhere under the
/// shader tree.
///
/// The tree-timestamp approach it replaces reported every ray-tracing shader as
/// stale whenever any shader was touched — including post-process or denoiser
/// shaders that no path-tracing library includes — so a one-line edit anywhere
/// forced a full recompile and PSO rebuild.
///
/// The include closure is produced by the cook (support/python/precompile_pt_shader_bins.py),
/// which gets it exactly from the preprocessor, and is read here from
/// `<ShaderBin>/<api>/deps.manifest`. This side only hashes the listed files and
/// combines them, so there is no second `#include` parser to keep in sync.
///
/// Fingerprints are stored per binary rather than per source so a partial recook
/// cannot make binaries built against older sources look current.
class ShaderDependencyIndex
{
public:
    /// `repoRoot` is what the manifest's relative paths resolve against; when shaders
    /// are deployed under ShaderDev this is that deployment root, not the repository.
    void initialize(std::filesystem::path manifestPath, std::filesystem::path repoRoot);

    /// Missing or unreadable manifests are not an error: callers fall back to
    /// treating binaries as unverifiable, which is the pre-existing behaviour for a
    /// tree that was never cooked.
    bool load();
    bool save();

    [[nodiscard]] bool isLoaded() const { return m_loaded; }

    /// True when `cacheHashHex` was built from the current contents of its closure.
    /// False when unknown, which conservatively forces a recompile.
    [[nodiscard]] bool isUpToDate(const std::string& cacheHashHex);

    /// Note that `cacheHashHex` has just been compiled from `sourceRelPath`.
    void record(const std::string& cacheHashHex, const std::string& sourceRelPath);

    /// Content fingerprint of a root source's recorded include closure.
    [[nodiscard]] std::string currentFingerprint(const std::string& sourceRelPath);

    /// Forget memoized file hashes. Call before re-checking after possible edits.
    void invalidateContentCache();

    /// Root sources whose fingerprint changed since the last call. Used by hot reload
    /// to recompile only the variants built from those sources.
    [[nodiscard]] std::vector<std::string> takeChangedSources();

    /// Convert an absolute shader path into the manifest's relative form.
    [[nodiscard]] std::string toRelative(const std::filesystem::path& absolute) const;

private:
    std::string fileHashLocked(const std::string& relPath);
    std::string fingerprintLocked(const std::string& sourceRelPath);

    struct BinRecord
    {
        std::string sourceRelPath;
        std::string fingerprint;
    };

    std::filesystem::path m_manifestPath;
    std::filesystem::path m_repoRoot;

    // Sorted on load; the fingerprint depends on iteration order matching the cook.
    std::unordered_map<std::string, std::vector<std::string>> m_closures;
    std::unordered_map<std::string, BinRecord> m_bins;

    std::mutex m_mutex;
    std::unordered_map<std::string, std::string> m_fileHashCache;
    std::unordered_map<std::string, std::string> m_fingerprintCache;
    std::unordered_map<std::string, std::string> m_changeSnapshot;

    bool m_loaded = false;
    bool m_dirty = false;
};

} // namespace caustica
