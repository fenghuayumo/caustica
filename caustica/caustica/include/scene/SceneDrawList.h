#pragma once

#include <ecs/Entity.h>
#include <rhi/nvrhi.h>
#include <vector>

namespace caustica
{
    class IView;
    struct MeshInfo;
    struct MeshGeometry;
    struct Material;
    struct BufferGroup;
}

namespace caustica::scene
{
    class SceneRenderData;

    // Raster draw command built from SceneRenderData for a specific view.
    struct DrawCommand
    {
        ecs::Entity meshEntity = ecs::NullEntity;
        int instanceIndex = -1;
        const caustica::MeshInfo* mesh = nullptr;
        const caustica::MeshGeometry* geometry = nullptr;
        const caustica::Material* material = nullptr;
        const caustica::BufferGroup* buffers = nullptr;
        float distanceToCamera = 0.f;
        nvrhi::RasterCullMode cullMode = nvrhi::RasterCullMode::Back;
    };

    enum class MeshDrawDomain : uint8_t
    {
        Opaque,
        Transparent,
    };

    struct DrawListBuildOptions
    {
        bool drawDoubleSidedMaterialsSeparately = true;
    };

    struct ViewDrawLists
    {
        std::vector<DrawCommand> opaque;
        std::vector<DrawCommand> transparent;
    };

    void buildOpaqueDrawList(
        const SceneRenderData& renderData,
        const caustica::IView& view,
        std::vector<DrawCommand>& out);

    void buildTransparentDrawList(
        const SceneRenderData& renderData,
        const caustica::IView& view,
        std::vector<DrawCommand>& out,
        const DrawListBuildOptions& options = {});

    void buildViewDrawLists(
        const SceneRenderData& renderData,
        const caustica::IView& view,
        ViewDrawLists& out,
        const DrawListBuildOptions& options = {});

} // namespace caustica::scene
