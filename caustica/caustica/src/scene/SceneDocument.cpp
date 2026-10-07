#include <scene/SceneDocument.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace caustica::scene
{
namespace
{
struct EditError : std::runtime_error
{
    std::string code;
    EditError(std::string errorCode, const std::string& message)
        : std::runtime_error(message), code(std::move(errorCode)) {}
};

void require(bool condition, const std::string& message, const char* code = "invalid_argument")
{
    if (!condition)
        throw EditError(code, message);
}

std::string uniqueId()
{
    // No shared PRNG state: independent documents may have different owners.
    std::random_device random;
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i)
        out << std::setw(8) << uint32_t(random());
    return out.str();
}

Json::Value parse(const std::string& text)
{
    Json::CharReaderBuilder builder;
    builder["rejectDupKeys"] = true;
    builder["failIfExtra"] = true;
    builder["allowSpecialFloats"] = false;
    std::istringstream input(text);
    Json::Value value;
    std::string errors;
    if (!Json::parseFromStream(builder, input, &value, &errors))
        throw std::invalid_argument("Invalid scene JSON: " + errors);
    return value;
}

std::string stringify(const Json::Value& value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    return Json::writeString(builder, value);
}

void mergePatch(Json::Value& target, const Json::Value& patch)
{
    if (!patch.isObject())
    {
        target = patch;
        return;
    }
    if (!target.isObject())
        target = Json::Value(Json::objectValue);
    for (const auto& key : patch.getMemberNames())
    {
        if (patch[key].isNull())
            target.removeMember(key);
        else
            mergePatch(target[key], patch[key]);
    }
}

void finiteValues(const Json::Value& value)
{
    if (value.isNumeric())
        require(std::isfinite(value.asDouble()), "Numeric values must be finite");
    else if (value.isArray() || value.isObject())
        for (const auto& child : value)
            finiteValues(child);
}

void vectorField(const Json::Value& node, const char* key, Json::ArrayIndex size)
{
    if (!node.isMember(key))
        return;
    const auto& value = node[key];
    require(value.isArray() && value.size() == size, std::string(key) + " has the wrong vector size");
    for (const auto& item : value)
        require(item.isNumeric(), std::string(key) + " requires numeric elements");
}

void positiveField(const Json::Value& node, const char* key, bool allowZero = false)
{
    if (node.isMember(key))
        require(node[key].isNumeric() && (allowZero ? node[key].asDouble() >= 0 : node[key].asDouble() > 0),
            std::string(key) + (allowZero ? " must be nonnegative" : " must be positive"));
}

void sourceField(const Json::Value& node, const char* key)
{
    require(node[key].isString() && !node[key].asString().empty(), std::string(key) + " must be a nonempty string");
}

void materialSlots(const Json::Value& node)
{
    require(node.isObject(), "Material slots must be an object");
    for (const auto& slot : node)
        require(slot.isString() && !slot.asString().empty(), "Material slots require nonempty asset paths");
}

void validate(const Json::Value& document)
{
    require(document.isObject() && document["entities"].isArray(), "Scene requires an entities array");
    require(!document.isMember("base") && !document.isMember("overrides"),
        "Resolve base/overrides before editing a scene document", "unsupported_document");
    require(document["entities"].size() <= 100000, "Scene exceeds 100000 authored entities");
    if (document.isMember("settings"))
        require(document["settings"].isObject(), "settings must be an object");
    finiteValues(document);
    std::unordered_map<std::string, std::string> parents;
    for (const auto& entity : document["entities"])
    {
        require(entity.isObject(), "Entity must be an object");
        sourceField(entity, "id");
        const auto id = entity["id"].asString();
        require(!parents.contains(id), "Duplicate entity id: " + id, "duplicate_id");
        if (entity.isMember("name"))
            require(entity["name"].isString(), "Entity name must be a string");
        std::string parent;
        if (entity.isMember("parent"))
        {
            sourceField(entity, "parent");
            parent = entity["parent"].asString();
        }
        parents.emplace(id, parent);
        if (!entity.isMember("components"))
            continue;
        const auto& components = entity["components"];
        require(components.isObject(), "components must be an object");
        for (const auto& component : components)
            require(component.isObject(), "Each component must be an object");
        if (components.isMember("Transform"))
        {
            const auto& transform = components["Transform"];
            vectorField(transform, "translation", 3);
            vectorField(transform, "euler", 3);
            vectorField(transform, "rotation", 4);
            require(!(transform.isMember("euler") && transform.isMember("rotation")),
                "Transform must use either euler or rotation, not both");
            if (transform.isMember("rotation"))
            {
                double norm = 0;
                for (const auto& item : transform["rotation"])
                    norm += item.asDouble() * item.asDouble();
                require(std::isfinite(norm) && norm > 1e-24, "Quaternion must have finite, nonzero length");
            }
            if (transform.isMember("scale"))
            {
                if (transform["scale"].isArray())
                {
                    vectorField(transform, "scale", 3);
                    for (const auto& value : transform["scale"])
                        require(std::abs(value.asDouble()) > 1e-12, "Scale must be invertible");
                }
                else
                    require(transform["scale"].isNumeric() && std::abs(transform["scale"].asDouble()) > 1e-12,
                        "Scale must be a nonzero number or numeric triple");
            }
        }
        if (components.isMember("PrefabInstance"))
        {
            sourceField(components["PrefabInstance"], "source");
            if (components["PrefabInstance"].isMember("materials"))
                materialSlots(components["PrefabInstance"]["materials"]);
        }
        if (components.isMember("MaterialOverride"))
        {
            const auto& material = components["MaterialOverride"];
            require(material.isMember("source") || material.isMember("slots"), "MaterialOverride requires source or slots");
            if (material.isMember("source"))
                sourceField(material, "source");
            if (material.isMember("slots"))
                materialSlots(material["slots"]);
        }
        for (const char* type : {"DirectionalLight", "PointLight", "SpotLight", "RectLight", "EnvironmentLight"})
        {
            if (!components.isMember(type))
                continue;
            const auto& light = components[type];
            vectorField(light, "color", 3);
            vectorField(light, "radianceScale", 3);
            for (const char* key : {"color", "radianceScale"})
                if (light.isMember(key))
                    for (const auto& value : light[key])
                        require(value.asDouble() >= 0, "Light colors must be nonnegative");
            for (const char* key : {"intensity", "irradiance", "radius", "range", "angularSize"})
                positiveField(light, key, true);
            positiveField(light, "width");
            positiveField(light, "height");
            for (const char* key : {"innerAngle", "outerAngle"})
            {
                positiveField(light, key, true);
                if (light.isMember(key))
                    require(light[key].asDouble() <= 180, std::string(key) + " must not exceed 180 degrees");
            }
            if (light.isMember("innerAngle") && light.isMember("outerAngle"))
                require(light["innerAngle"].asDouble() <= light["outerAngle"].asDouble(), "innerAngle must not exceed outerAngle");
            if (light.isMember("source"))
                sourceField(light, "source");
            if (light.isMember("rotation"))
                require(light["rotation"].isNumeric(), "Environment rotation must be numeric");
            if (light.isMember("enabled"))
                require(light["enabled"].isBool(), "Light enabled must be boolean");
        }
        for (const char* type : {"PerspectiveCamera", "PerspectiveCameraEx"})
        {
            if (!components.isMember(type))
                continue;
            const auto& camera = components[type];
            positiveField(camera, "verticalFov");
            positiveField(camera, "zNear");
            if (camera.isMember("verticalFov"))
                require(camera["verticalFov"].asDouble() < 3.141592653589793, "verticalFov must be less than pi radians");
        }
    }
    // Three-color traversal: linear in entity count even for deep hierarchies.
    std::unordered_map<std::string, int> colors;
    for (const auto& [id, unused] : parents)
    {
        (void)unused;
        std::string current = id;
        std::vector<std::string> chain;
        while (!current.empty() && colors[current] != 2)
        {
            require(parents.contains(current), "Unknown parent id: " + current, "entity_not_found");
            require(colors[current] != 1, "Parent cycle at: " + current, "hierarchy_cycle");
            colors[current] = 1;
            chain.push_back(current);
            current = parents.at(current);
        }
        for (const auto& visited : chain)
            colors[visited] = 2;
    }
}

Json::Value* findEntity(Json::Value& document, const std::string& id)
{
    for (auto& entity : document["entities"])
        if (entity["id"].asString() == id)
            return &entity;
    return nullptr;
}
} // namespace

Json::Value SceneDocument::emptyDocument()
{
    Json::Value document(Json::objectValue);
    document["entities"] = Json::Value(Json::arrayValue);
    return document;
}

SceneDocument::SceneDocument(Json::Value document, size_t historyLimit)
    : m_document(std::move(document)), m_sessionId(uniqueId()), m_historyLimit(std::min(historyLimit, size_t(128)))
{
    try { validate(m_document); }
    catch (const EditError& error) { throw std::invalid_argument(error.what()); }
}

SceneDocument SceneDocument::fromJson(const std::string& source)
{
    return SceneDocument(parse(source));
}

SceneDocument SceneDocument::load(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot open scene document: " + path.generic_string());
    std::ostringstream text;
    text << input.rdbuf();
    if (input.bad())
        throw std::runtime_error("Cannot read scene document: " + path.generic_string());
    return fromJson(text.str());
}

Json::Value SceneDocument::capabilities()
{
    return parse(R"({"protocol_version":1,"operations":["add_entity","patch_entity","remove_entity","patch_settings","undo","redo"],"units":{"distance":"meter","transform_euler":"radian","camera_vertical_fov":"radian","spot_cone_angle":"degree","directional_angular_size":"degree","world_up":"+Y","handedness":"right","quaternion":"xyzw","color":"linear RGB"},"patch_semantics":"JSON Merge Patch; null removes a field; arrays replace","atomic_batch":true,"runtime_apply":"explicit scene reload","validation":"structure, finite values, TRS, core light/camera parameters; unknown components preserved; assets not resolved"})");
}

std::string SceneDocument::toJson() const { return stringify(m_document); }

Json::Value SceneDocument::result(const std::string& requestId) const
{
    Json::Value value(Json::objectValue);
    value["request_id"] = requestId;
    value["session_id"] = m_sessionId;
    value["revision"] = Json::UInt64(m_revision);
    value["status"] = "completed";
    return value;
}

Json::Value SceneDocument::getEntity(const std::string& id) const
{
    for (const auto& entity : m_document["entities"])
        if (entity["id"].asString() == id)
            return entity;
    return Json::Value();
}

Json::Value SceneDocument::queryEntities(const std::string& component, size_t offset, size_t limit) const
{
    auto value = result("");
    value["entities"] = Json::Value(Json::arrayValue);
    size_t total = 0;
    limit = std::min(limit, size_t(1000));
    for (const auto& entity : m_document["entities"])
    {
        if (!component.empty() && !entity["components"].isMember(component))
            continue;
        if (total >= offset && size_t(value["entities"].size()) < limit)
            value["entities"].append(entity);
        ++total;
    }
    value["total"] = Json::UInt64(total);
    value["offset"] = Json::UInt64(offset);
    value["limit"] = Json::UInt64(limit);
    return value;
}

void SceneDocument::remember(std::vector<Json::Value>& history, const Json::Value& document)
{
    if (m_historyLimit == 0)
        return;
    if (history.size() == m_historyLimit)
        history.erase(history.begin());
    history.push_back(document);
}

Json::Value SceneDocument::execute(const Json::Value& request)
{
    std::string requestId;
    Json::Value created(Json::arrayValue);
    try
    {
        require(request.isObject(), "Request must be an object");
        if (request.isMember("request_id"))
        {
            require(request["request_id"].isString(), "request_id must be a string");
            requestId = request["request_id"].asString();
        }
        if (request.isMember("expected_revision"))
        {
            require(request["expected_revision"].isUInt64(), "expected_revision must be an unsigned integer");
            require(request["expected_revision"].asUInt64() == m_revision,
                "Document revision changed; query again before retrying", "revision_conflict");
        }
        if (request.isMember("expected_session_id"))
        {
            require(request["expected_session_id"].isString(), "expected_session_id must be a string");
            require(request["expected_session_id"].asString() == m_sessionId,
                "Request belongs to another document session", "session_conflict");
        }
        const auto& operations = request["operations"];
        require(operations.isArray() && !operations.empty() && operations.size() <= 1024,
            "operations must contain 1 to 1024 operations");
        Json::Value candidate = m_document;
        for (const auto& operation : operations)
        {
            require(operation.isObject() && operation["op"].isString(), "Each operation requires op");
            const auto op = operation["op"].asString();
            if (op == "undo" || op == "redo")
            {
                require(operations.size() == 1, "undo/redo must be the only operation");
                auto& from = op == "undo" ? m_undo : m_redo;
                auto& to = op == "undo" ? m_redo : m_undo;
                require(!from.empty(), "No " + op + " history", "history_empty");
                candidate = from.back();
                remember(to, m_document);
                from.pop_back();
                m_document = std::move(candidate);
                ++m_revision;
                return result(requestId);
            }
            if (op == "patch_settings")
            {
                require(operation["patch"].isObject(), "patch_settings requires an object patch");
                mergePatch(candidate["settings"], operation["patch"]);
                continue;
            }
            if (op == "add_entity")
            {
                Json::Value entity = operation["entity"];
                require(entity.isObject(), "add_entity requires an entity object");
                if (!entity.isMember("id"))
                {
                    do { entity["id"] = "entity_" + uniqueId(); }
                    while (findEntity(candidate, entity["id"].asString()));
                }
                sourceField(entity, "id");
                require(!findEntity(candidate, entity["id"].asString()), "Entity id already exists", "duplicate_id");
                created.append(entity["id"]);
                candidate["entities"].append(std::move(entity));
                continue;
            }
            require(op == "patch_entity" || op == "remove_entity", "Unknown operation: " + op, "unsupported_operation");
            sourceField(operation, "id");
            const auto id = operation["id"].asString();
            auto* entity = findEntity(candidate, id);
            require(entity != nullptr, "Entity not found: " + id, "entity_not_found");
            if (op == "patch_entity")
            {
                const auto& patch = operation["patch"];
                require(patch.isObject() && !patch.isMember("id"), "Entity id is immutable; patch must be an object without id");
                mergePatch(*entity, patch);
            }
            else
            {
                require(!operation.isMember("cascade") || operation["cascade"].isBool(), "cascade must be boolean");
                const bool cascade = operation.get("cascade", false).asBool();
                std::unordered_map<std::string, std::vector<std::string>> children;
                for (const auto& child : candidate["entities"])
                    if (child["parent"].isString())
                        children[child["parent"].asString()].push_back(child["id"].asString());
                require(cascade || children[id].empty(), "Entity has children; use cascade=true", "entity_has_children");
                std::set<std::string> removed;
                std::vector<std::string> pending{id};
                while (!pending.empty())
                {
                    auto current = pending.back();
                    pending.pop_back();
                    if (!removed.insert(current).second)
                        continue;
                    for (const auto& child : children[current])
                        pending.push_back(child);
                }
                Json::Value kept(Json::arrayValue);
                for (const auto& child : candidate["entities"])
                    if (!removed.contains(child["id"].asString()))
                        kept.append(child);
                candidate["entities"] = std::move(kept);
            }
        }
        validate(candidate);
        const bool changed = candidate != m_document;
        if (changed)
        {
            remember(m_undo, m_document);
            m_redo.clear();
            m_document = std::move(candidate);
            ++m_revision;
        }
        auto value = result(requestId);
        value["changed"] = changed;
        value["created_ids"] = std::move(created);
        return value;
    }
    catch (const EditError& error)
    {
        auto value = result(requestId);
        value["status"] = "failed";
        value["error"]["code"] = error.code;
        value["error"]["message"] = error.what();
        return value;
    }
}

Json::Value SceneDocument::save(const std::filesystem::path& path) const
{
    auto value = result("");
    std::filesystem::path temporary;
    try
    {
        if (path.empty() || !path.has_filename())
            throw std::runtime_error("A scene file path is required");
        const auto destination = std::filesystem::absolute(path);
        temporary = destination;
        temporary += "." + uniqueId() + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("Cannot open temporary scene file");
        output << toJson() << '\n';
        output.flush();
        if (!output)
            throw std::runtime_error("Cannot write scene document");
        output.close();
        if (output.fail())
            throw std::runtime_error("Cannot close scene document");
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace scene file (Windows error " + std::to_string(GetLastError()) + ")");
#else
        std::filesystem::rename(temporary, destination);
#endif
        value["path"] = destination.generic_string();
    }
    catch (const std::exception& error)
    {
        std::error_code ignored;
        if (!temporary.empty())
            std::filesystem::remove(temporary, ignored);
        value["status"] = "failed";
        value["error"]["code"] = "io_error";
        value["error"]["message"] = error.what();
    }
    return value;
}

} // namespace caustica::scene
