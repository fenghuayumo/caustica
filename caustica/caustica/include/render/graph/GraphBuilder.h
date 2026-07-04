#pragma once

#include <rhi/nvrhi.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace caustica::rg
{

enum class TextureAccess : uint8_t
{
    ShaderResource,
    RenderTarget,
    UnorderedAccess,
};

struct TextureHandle
{
    uint32_t index = kInvalid;
    static constexpr uint32_t kInvalid = UINT32_MAX;

    [[nodiscard]] bool valid() const { return index != kInvalid; }
};

class PassBuilder
{
public:
    void read(TextureHandle texture, TextureAccess access = TextureAccess::ShaderResource);
    void write(TextureHandle texture, TextureAccess access = TextureAccess::RenderTarget);

    [[nodiscard]] const std::vector<std::pair<TextureHandle, TextureAccess>>& reads() const { return m_reads; }
    [[nodiscard]] const std::vector<std::pair<TextureHandle, TextureAccess>>& writes() const { return m_writes; }

private:
    std::vector<std::pair<TextureHandle, TextureAccess>> m_reads;
    std::vector<std::pair<TextureHandle, TextureAccess>> m_writes;
};

class RenderPassContext
{
public:
    RenderPassContext(nvrhi::ICommandList* commandList, const class GraphBuilder& graph);

    [[nodiscard]] nvrhi::ICommandList* commandList() const { return m_commandList; }
    [[nodiscard]] nvrhi::ITexture* texture(TextureHandle handle) const;

private:
    nvrhi::ICommandList* m_commandList = nullptr;
    const GraphBuilder* m_graph = nullptr;
};

// Minimal render graph: sequential passes with automatic texture barriers.
// Phase R1 — no pass culling, no transient allocation, import-only resources.
class GraphBuilder
{
public:
    using SetupFn = std::function<void(PassBuilder&)>;
    using ExecuteFn = std::function<void(RenderPassContext&)>;

    TextureHandle importTexture(nvrhi::ITexture* texture, TextureAccess initialAccess = TextureAccess::ShaderResource);

    void addPass(std::string_view name, SetupFn setup, ExecuteFn execute);

    void execute(nvrhi::ICommandList* commandList);

    [[nodiscard]] nvrhi::ITexture* resolveTexture(TextureHandle handle) const;

    [[nodiscard]] size_t passCount() const { return m_passes.size(); }

private:
    friend class RenderPassContext;

    struct ImportedTexture
    {
        nvrhi::ITexture* texture = nullptr;
        nvrhi::ResourceStates currentState = nvrhi::ResourceStates::Common;
    };

    struct Pass
    {
        std::string name;
        SetupFn setup;
        ExecuteFn execute;
        std::vector<std::pair<TextureHandle, TextureAccess>> reads;
        std::vector<std::pair<TextureHandle, TextureAccess>> writes;
    };

    static nvrhi::ResourceStates accessToState(TextureAccess access);

    void transitionTexture(nvrhi::ICommandList* commandList, TextureHandle handle, TextureAccess access);

    std::vector<ImportedTexture> m_textures;
    std::vector<Pass> m_passes;
};

} // namespace caustica::rg
