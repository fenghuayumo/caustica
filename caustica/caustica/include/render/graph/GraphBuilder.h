#pragma once

#include <render/graph/GpuDeviceAdapter.h>
#include <render/graph/GpuTypes.h>
#include <rhi/nvrhi.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nvrhi
{
class ICommandList;
class IDevice;
class ITexture;
} // namespace nvrhi

namespace caustica::rg
{

enum class TextureAccess : uint8_t
{
    ShaderResource,
    RenderTarget,
    UnorderedAccess,
    CopySource,
    CopyDest,
};

class GraphBuilder;

class PassBuilder
{
public:
    explicit PassBuilder(GraphBuilder& graph);

    void read(TextureHandle texture, TextureAccess access = TextureAccess::ShaderResource);
    void write(TextureHandle texture, TextureAccess access = TextureAccess::RenderTarget);

    [[nodiscard]] TextureHandle createTexture(const TextureDesc& desc);

    [[nodiscard]] const std::vector<std::pair<TextureHandle, TextureAccess>>& reads() const { return m_reads; }
    [[nodiscard]] const std::vector<std::pair<TextureHandle, TextureAccess>>& writes() const { return m_writes; }

private:
    GraphBuilder* m_graph = nullptr;
    std::vector<std::pair<TextureHandle, TextureAccess>> m_reads;
    std::vector<std::pair<TextureHandle, TextureAccess>> m_writes;
};

class RenderPassContext
{
public:
    RenderPassContext(nvrhi::ICommandList* commandList, const GraphBuilder& graph);

    [[nodiscard]] nvrhi::ICommandList* commandList() const { return m_commandList; }
    [[nodiscard]] CommandList rhiCommandList() const;
    [[nodiscard]] nvrhi::ITexture* texture(TextureHandle handle) const;

private:
    nvrhi::ICommandList* m_commandList = nullptr;
    const GraphBuilder* m_graph = nullptr;
};

// Render graph: sequential passes with automatic texture barriers.
// Phase R2 — import + transient resources, compile/execute split.
class GraphBuilder
{
public:
    using SetupFn = std::function<void(PassBuilder&)>;
    using ExecuteFn = std::function<void(RenderPassContext&)>;

    void setDevice(nvrhi::IDevice* device);
    void setDevice(Device& device);

    TextureHandle importTexture(nvrhi::ITexture* texture, nvrhi::ResourceStates initialState);
    TextureHandle importTexture(nvrhi::ITexture* texture, TextureAccess initialAccess = TextureAccess::ShaderResource);

    [[nodiscard]] TextureHandle createTexture(const TextureDesc& desc);

    void addPass(std::string_view name, SetupFn setup, ExecuteFn execute, bool enabled = true);

    void compile();
    void execute(nvrhi::ICommandList* commandList);
    void execute(CommandList& commandList);

    void reset();

    [[nodiscard]] nvrhi::ITexture* resolveTexture(TextureHandle handle) const;
    [[nodiscard]] nvrhi::ResourceStates textureState(TextureHandle handle) const;
    [[nodiscard]] bool isCompiled() const { return m_compiled; }

    [[nodiscard]] size_t passCount() const { return m_passes.size(); }
    [[nodiscard]] const std::vector<std::string>& passNames() const { return m_passNames; }

private:
    friend class PassBuilder;
    friend class RenderPassContext;

    enum class ResourceLifetime : uint8_t
    {
        Imported,
        Transient,
    };

    struct GraphTexture
    {
        nvrhi::ITexture* texture = nullptr;
        nvrhi::ResourceStates currentState = nvrhi::ResourceStates::Common;
        ResourceLifetime lifetime = ResourceLifetime::Imported;
        nvrhi::TextureHandle owned;
    };

    struct Pass
    {
        std::string name;
        SetupFn setup;
        ExecuteFn execute;
        bool enabled = true;
        std::vector<std::pair<TextureHandle, TextureAccess>> reads;
        std::vector<std::pair<TextureHandle, TextureAccess>> writes;
    };

    static nvrhi::ResourceStates accessToState(TextureAccess access);

    void releaseTransientResources();
    void transitionTexture(nvrhi::ICommandList* commandList, TextureHandle handle, TextureAccess access);
    void syncPassEndStates(const Pass& pass);
    static bool passUsesTextureAsWrite(const Pass& pass, TextureHandle handle);

    nvrhi::IDevice* m_device = nullptr;
    bool m_compiled = false;
    std::vector<GraphTexture> m_textures;
    std::vector<Pass> m_passes;
    std::vector<std::string> m_passNames;
    std::unordered_map<nvrhi::ITexture*, uint32_t> m_importIndexByTexture;
};

} // namespace caustica::rg
