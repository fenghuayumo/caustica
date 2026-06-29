#pragma once

#include <math/math.h>
#include <memory>
#include <vector>

namespace caustica
{
    class Scene;
    class IView;
}

namespace caustica::render
{
    struct DrawItem;

    class IDrawStrategy
    {
    public:
        virtual void PrepareForView(
            const caustica::Scene& scene,
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
            const caustica::Scene& scene,
            const caustica::IView& view) override { (void)scene; (void)view; }

        const DrawItem* GetNextItem() override;

        void SetData(const DrawItem* data, size_t count);
    };

    class InstancedOpaqueDrawStrategy : public IDrawStrategy
    {
    private:
        dm::frustum m_ViewFrustum;
        std::vector<DrawItem> m_Items;
        std::vector<const DrawItem*> m_ItemPtrs;
        size_t m_ReadPtr = 0;

    public:
        void PrepareForView(
            const caustica::Scene& scene,
            const caustica::IView& view) override;

        const DrawItem* GetNextItem() override;
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
            const caustica::Scene& scene,
            const caustica::IView& view) override;

        const DrawItem* GetNextItem() override;
    };
}
