#pragma once

#include <scene/SceneGraph.h>
#include <memory>
#include <vector>

namespace caustica
{
    class IView;
}

namespace caustica::render
{
    struct DrawItem;

    class IDrawStrategy
    {
    public:
        virtual void PrepareForView(
            const std::shared_ptr<caustica::SceneGraphNode>& rootNode,
            const caustica::IView& view) = 0;

        virtual const DrawItem* GetNextItem() = 0;

        virtual ~IDrawStrategy() = default;
    };

    class PassthroughDrawStrategy : public IDrawStrategy
    {
    private:
        const DrawItem* m_Data = nullptr;
        size_t m_Count = 0;

    public:
        void PrepareForView(
            const std::shared_ptr<caustica::SceneGraphNode>& rootNode,
            const caustica::IView& view) override { }

        const DrawItem* GetNextItem() override;

        void SetData(const DrawItem* data, size_t count);
    };
    
    class InstancedOpaqueDrawStrategy : public IDrawStrategy
    {
    private:
        dm::frustum m_ViewFrustum;
        caustica::SceneGraphWalker m_Walker;
        std::vector<DrawItem> m_InstanceChunk;
        std::vector<const DrawItem*> m_InstancePtrChunk;
        size_t m_ReadPtr = 0;
        size_t m_ChunkSize = 128;

        void FillChunk();

    public:

        void PrepareForView(
            const std::shared_ptr<caustica::SceneGraphNode>& rootNode,
            const caustica::IView& view) override;

        const DrawItem* GetNextItem() override;

        [[nodiscard]] size_t GetChunkSize() const { return m_ChunkSize; }
        void SetChunkSize(size_t size) { m_ChunkSize = std::max<size_t>(size, 1u); }
    };

    class TransparentDrawStrategy : public IDrawStrategy
    {
    private:
        std::vector<DrawItem> m_InstancesToDraw;
        std::vector<const DrawItem*> m_InstancePtrsToDraw;
        size_t m_ReadPtr = 0;

    public:
        bool DrawDoubleSidedMaterialsSeparately = true;
        
        void PrepareForView(
            const std::shared_ptr<caustica::SceneGraphNode>& rootNode,
            const caustica::IView& view) override;

        const DrawItem* GetNextItem() override;
    };
}