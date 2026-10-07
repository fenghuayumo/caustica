#pragma once

#include <json/json.h>

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace caustica::scene
{

// CPU-only authoring document. Independent of ECS simulation state and GPU
// lifetime. A successful execute() is one atomic, undoable document edit;
// publishing it to EngineApp is a separate, asynchronous scene load.
// Confine each instance to one owner thread (including queries and save).
class SceneDocument
{
public:
    explicit SceneDocument(Json::Value document = emptyDocument(), size_t historyLimit = 32);
    SceneDocument(const SceneDocument&) = delete;
    SceneDocument& operator=(const SceneDocument&) = delete;
    SceneDocument(SceneDocument&&) noexcept = default;
    SceneDocument& operator=(SceneDocument&&) noexcept = default;
    [[nodiscard]] static Json::Value emptyDocument();
    [[nodiscard]] static SceneDocument fromJson(const std::string& source);
    [[nodiscard]] static SceneDocument load(const std::filesystem::path& path);
    [[nodiscard]] static Json::Value capabilities();

    [[nodiscard]] uint64_t revision() const { return m_revision; }
    [[nodiscard]] const std::string& sessionId() const { return m_sessionId; }
    [[nodiscard]] const Json::Value& document() const { return m_document; }
    [[nodiscard]] std::string toJson() const;
    [[nodiscard]] Json::Value getEntity(const std::string& id) const;
    [[nodiscard]] Json::Value queryEntities(
        const std::string& component = {}, size_t offset = 0, size_t limit = 100) const;

    // {request_id?, expected_session_id?, expected_revision?, operations:[...]}
    // Operations: add_entity, patch_entity (JSON Merge Patch), remove_entity,
    // patch_settings, undo, redo. Undo/redo must be the sole operation.
    // Validation/conflict failures leave document, revision and history intact.
    [[nodiscard]] Json::Value execute(const Json::Value& request);
    // Atomic sibling-temp-file replacement. Does not mutate the document or
    // revision, copy referenced assets, or capture live runtime property edits.
    [[nodiscard]] Json::Value save(const std::filesystem::path& path) const;

private:
    [[nodiscard]] Json::Value result(const std::string& requestId) const;
    void remember(std::vector<Json::Value>& history, const Json::Value& document);

    Json::Value m_document;
    std::string m_sessionId;
    uint64_t m_revision = 0;
    size_t m_historyLimit;
    std::vector<Json::Value> m_undo;
    std::vector<Json::Value> m_redo;
};

} // namespace caustica::scene
