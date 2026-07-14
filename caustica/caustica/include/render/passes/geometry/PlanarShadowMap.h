#pragma once

#include <scene/SceneObjects.h>
#include <render/core/ShadowMap.h>
#include <scene/View.h>
#include <rhi/nvrhi.h>
#include <memory>

namespace caustica::render
{
    class PlanarShadowMap : public caustica::IShadowMap
    {
    private:
        nvrhi::TextureHandle m_ShadowMapTexture;
        std::shared_ptr<caustica::PlanarView> m_View;
        bool m_IsLitOutOfBounds = false;
        dm::float2 m_FadeRangeTexels = 1.f;
        dm::float2 m_ShadowMapSize;
        dm::float2 m_TextureSize;
        float m_FalloffDistance = 1.f;

    public:
        PlanarShadowMap(
            nvrhi::IDevice* device,
            int resolution,
            nvrhi::Format format);

        PlanarShadowMap(
            nvrhi::IDevice* device,
            nvrhi::ITexture* texture,
            uint32_t arraySlice,
            const caustica::ViewportDesc& viewport);

        bool setupWholeSceneDirectionalLightView(
            const caustica::DirectionalLight& light, 
            dm::box3_arg sceneBounds, 
            float fadeRangeWorld = 0.f);

        bool setupDynamicDirectionalLightView(
            const caustica::DirectionalLight& light, 
            dm::float3 anchor, 
            dm::float3 halfShadowBoxSize, 
            dm::float3 preViewTranslation = 0.f,
            float fadeRangeWorld = 0.f);

        void setupProxyView();

        void clear(nvrhi::ICommandList* commandList);

        void setLitOutOfBounds(bool litOutOfBounds);
        void setFalloffDistance(float distance);

        std::shared_ptr<caustica::PlanarView> getPlanarView();

        virtual dm::float4x4 getWorldToUvzwMatrix() const override;
        virtual const caustica::ICompositeView& getView() const override;
        virtual nvrhi::ITexture* getTexture() const override;
        virtual uint32_t getNumberOfCascades() const override;
        virtual const IShadowMap* getCascade(uint32_t index) const override;
        virtual uint32_t getNumberOfPerObjectShadows() const override;
        virtual const IShadowMap* getPerObjectShadow(uint32_t index) const override;
        virtual dm::int2 getTextureSize() const override;
        virtual dm::box2 getUVRange() const override;
        virtual dm::float2 getFadeRangeInTexels() const override;
        virtual bool isLitOutOfBounds() const override;
        virtual void fillShadowConstants(ShadowConstants& constants) const override;
    };
}