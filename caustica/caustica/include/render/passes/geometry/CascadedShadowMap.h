#pragma once

#include <scene/SceneObjects.h>
#include <render/core/ShadowMap.h>
#include <scene/View.h>
#include <rhi/nvrhi.h>
#include <memory>

namespace caustica::render
{
    class PlanarShadowMap;

    class CascadedShadowMap : public caustica::IShadowMap
    {
    private:
        nvrhi::TextureHandle m_ShadowMapTexture;
        std::vector<std::shared_ptr<PlanarShadowMap>> m_Cascades;
        std::vector<std::shared_ptr<PlanarShadowMap>> m_PerObjectShadows;
        caustica::CompositeView m_CompositeView;
        int m_NumberOfCascades;

    public:
        CascadedShadowMap(
            nvrhi::IDevice* device,
            int resolution,
            int numCascades,
            int numPerObjectShadows,
            nvrhi::Format format,
            bool isUAV = false);

        // Computes the cascade projections based on the view frustum, shadow distance, and the distribution exponent.
        bool setupForPlanarView(
            const caustica::DirectionalLight& light, 
            dm::frustum viewFrustum, 
            float maxShadowDistance, 
            float lightSpaceZUp, 
            float lightSpaceZDown, 
            float exponent = 4.f,
            dm::float3 preViewTranslation = 0.f,
            int numberOfCascades = -1);

        // Similar to setupForPlanarView, but the size of the cascades does not depend on orientation, and therefore 
        // the shadow map texels have the same world space projections when the camera turns or moves.
        // The downside of this algorithm is that the cascades are often larger than necessary.
        bool setupForPlanarViewStable(
            const caustica::DirectionalLight& light, 
            dm::frustum projectionFrustum, 
            dm::affine3 inverseViewMatrix, 
            float maxShadowDistance, 
            float lightSpaceZUp, 
            float lightSpaceZDown, 
            float exponent = 4.f, 
            dm::float3 preViewTranslation = 0.f,
            int numberOfCascades = -1);

        // Computes the cascade projections to cover an omnidirectional view from a given point. The cascades are all centered on that point.
        bool setupForCubemapView(
            const caustica::DirectionalLight& light, 
            dm::float3 center, 
            float maxShadowDistance, float lightSpaceZUp, 
            float lightSpaceZDown, 
            float exponent = 4.f,
            int numberOfCascades = -1);

        // Computes a simple directional shadow projection that covers a given world space box.
        bool setupPerObjectShadow(const caustica::DirectionalLight& light, uint32_t object, const dm::box3& objectBounds);

        void setupProxyViews();

        void clear(nvrhi::ICommandList* commandList);

        void setLitOutOfBounds(bool litOutOfBounds);
        void setFalloffDistance(float distance);
		void setNumberOfCascadesUnsafe(int cascades);

        std::shared_ptr<caustica::PlanarView> getCascadeView(uint32_t cascade);
        std::shared_ptr<caustica::PlanarView> getPerObjectView(uint32_t object);

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