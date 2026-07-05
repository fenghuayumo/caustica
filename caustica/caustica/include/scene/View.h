#pragma once

#include <math/math.h>
#include <scene/ViewDesc.h>

#include <algorithm>
#include <cassert>
#include <memory>
#include <vector>

struct PlanarViewConstants;

namespace caustica
{
    class IView;

    struct ViewType
    {
        enum Enum
        {
            PLANAR = 0x01,
            STEREO = 0x02,
            CUBEMAP = 0x04
        };
    };

    class ICompositeView
    {
    public:
        [[nodiscard]] virtual uint32_t getNumChildViews(ViewType::Enum supportedTypes) const = 0;
        [[nodiscard]] virtual const IView* getChildView(ViewType::Enum supportedTypes, uint32_t index) const = 0;

        virtual ~ICompositeView() = default;
    };

    class IView : public ICompositeView
    {
    public:
        virtual void fillPlanarViewConstants(PlanarViewConstants& constants) const;

        [[nodiscard]] virtual ViewportStateDesc getViewportState() const = 0;
        [[nodiscard]] virtual TextureSubresourceDesc getSubresources() const = 0;
        [[nodiscard]] virtual bool isReverseDepth() const = 0;
        [[nodiscard]] virtual bool isOrthographicProjection() const = 0;
        [[nodiscard]] virtual bool isStereoView() const = 0;
        [[nodiscard]] virtual bool isCubemapView() const = 0;
        [[nodiscard]] virtual bool isBoxVisible(const dm::box3& bbox) const = 0;
        [[nodiscard]] virtual bool isMirrored() const = 0;
        [[nodiscard]] virtual dm::float3 getViewOrigin() const = 0;
        [[nodiscard]] virtual dm::float3 getViewDirection() const = 0;
        [[nodiscard]] virtual dm::frustum getViewFrustum() const = 0;
        [[nodiscard]] virtual dm::frustum getProjectionFrustum() const = 0;
        [[nodiscard]] virtual dm::affine3 getViewMatrix() const = 0;
        [[nodiscard]] virtual dm::affine3 getInverseViewMatrix() const = 0;
        [[nodiscard]] virtual dm::float4x4 getProjectionMatrix(bool includeOffset = true) const = 0;
        [[nodiscard]] virtual dm::float4x4 getInverseProjectionMatrix(bool includeOffset = true) const = 0;
        [[nodiscard]] virtual dm::float4x4 getViewProjectionMatrix(bool includeOffset = true) const = 0;
        [[nodiscard]] virtual dm::float4x4 getInverseViewProjectionMatrix(bool includeOffset = true) const = 0;
        [[nodiscard]] virtual ScissorDesc getViewExtent() const = 0;
        [[nodiscard]] virtual dm::float2 getPixelOffset() const = 0;

        [[nodiscard]] uint32_t getNumChildViews(ViewType::Enum supportedTypes) const override;
        [[nodiscard]] const IView* getChildView(ViewType::Enum supportedTypes, uint32_t index) const override;
    };


    class PlanarView : public IView
    {
    protected:
        // Directly settable parameters
        ViewportDesc m_viewport;
        ScissorDesc m_scissorRect;
        dm::affine3 m_viewMatrix = dm::affine3::identity();
        dm::float4x4 m_projMatrix = dm::float4x4::identity();
        dm::float2 m_pixelOffset = dm::float2::zero();
        int m_arraySlice = 0;

        // Derived matrices and other information - computed and cached on access
        dm::float4x4 m_pixelOffsetMatrix = dm::float4x4::identity();
        dm::float4x4 m_pixelOffsetMatrixInv = dm::float4x4::identity();
        dm::float4x4 m_viewProjMatrix = dm::float4x4::identity();
        dm::float4x4 m_viewProjOffsetMatrix = dm::float4x4::identity();
        dm::affine3 m_viewMatrixInv = dm::affine3::identity();
        dm::float4x4 m_projMatrixInv = dm::float4x4::identity();
        dm::float4x4 m_viewProjMatrixInv = dm::float4x4::identity();
        dm::float4x4 m_viewProjOffsetMatrixInv = dm::float4x4::identity();
        dm::frustum m_viewFrustum = dm::frustum::empty();
        dm::frustum m_projectionFrustum = dm::frustum::empty();
        bool m_reverseDepth = false;
        bool m_isMirrored = false;
        bool m_cacheValid = false;

        void ensureCacheIsValid() const;
        
    public:
        void setViewport(const ViewportDesc& viewport);
        void setMatrices(const dm::affine3& viewMatrix, const dm::float4x4& projMatrix);
        void setPixelOffset(dm::float2 offset);
        void setArraySlice(int arraySlice);
        void updateCache();

        [[nodiscard]] const ViewportDesc& getViewport() const { return m_viewport; }
        [[nodiscard]] const ScissorDesc& getScissorRect() const { return m_scissorRect; }

        [[nodiscard]] ViewportStateDesc getViewportState() const override;
        [[nodiscard]] TextureSubresourceDesc getSubresources() const override;
        [[nodiscard]] bool isReverseDepth() const override;
        [[nodiscard]] bool isOrthographicProjection() const override;
        [[nodiscard]] bool isStereoView() const override;
        [[nodiscard]] bool isCubemapView() const override;
        [[nodiscard]] bool isBoxVisible(const dm::box3& bbox) const override;
        [[nodiscard]] bool isMirrored() const override;
        [[nodiscard]] dm::float3 getViewOrigin() const override;
        [[nodiscard]] dm::float3 getViewDirection() const override;
        [[nodiscard]] dm::frustum getViewFrustum() const override;
        [[nodiscard]] dm::frustum getProjectionFrustum() const override;
        [[nodiscard]] dm::affine3 getViewMatrix() const override;
        [[nodiscard]] dm::affine3 getInverseViewMatrix() const override;
        [[nodiscard]] dm::float4x4 getProjectionMatrix(bool includeOffset = true) const override;
        [[nodiscard]] dm::float4x4 getInverseProjectionMatrix(bool includeOffset = true) const override;
        [[nodiscard]] dm::float4x4 getViewProjectionMatrix(bool includeOffset = true) const override;
        [[nodiscard]] dm::float4x4 getInverseViewProjectionMatrix(bool includeOffset = true) const override;
        [[nodiscard]] ScissorDesc getViewExtent() const override;
        [[nodiscard]] dm::float2 getPixelOffset() const override;
    };

    class CompositeView : public ICompositeView
    {
    protected:
        std::vector<std::shared_ptr<IView>> m_childViews;

    public:
        void addView(std::shared_ptr<IView> view);

        [[nodiscard]] uint32_t getNumChildViews(ViewType::Enum supportedTypes) const override;
        [[nodiscard]] const IView* getChildView(ViewType::Enum supportedTypes, uint32_t index) const override;
    };

    template<typename ChildType>
    class StereoView : public IView
    {
    private:
        typedef IView Super;

    public:
        ChildType leftView;
        ChildType rightView;

        [[nodiscard]] ViewportStateDesc getViewportState() const override
        {
            ViewportStateDesc merged = leftView.getViewportState();
            const ViewportStateDesc right = rightView.getViewportState();

            for (const ViewportDesc& viewport : right.viewports)
                merged.viewports.push_back(viewport);
            for (const ScissorDesc& scissor : right.scissorRects)
                merged.scissorRects.push_back(scissor);

            return merged;
        }

        [[nodiscard]] TextureSubresourceDesc getSubresources() const override
        {
            return leftView.getSubresources(); // TODO: not really...
        }

        [[nodiscard]] bool isReverseDepth() const override
        {
            return leftView.isReverseDepth();
        }

        [[nodiscard]] bool isOrthographicProjection() const override
        {
            return leftView.isOrthographicProjection();
        }

        [[nodiscard]] bool isStereoView() const override
        {
            return true;
        }

        [[nodiscard]] bool isCubemapView() const override
        {
            return false;
        }

        [[nodiscard]] bool isBoxVisible(const dm::box3& bbox) const override
        {
            return leftView.isBoxVisible(bbox) || rightView.isBoxVisible(bbox);
        }

        [[nodiscard]] bool isMirrored() const override
        {
            return leftView.isMirrored();
        }

        [[nodiscard]] dm::float3 getViewOrigin() const override
        {
            return (leftView.getViewOrigin() + rightView.getViewOrigin()) * 0.5f;
        }

        [[nodiscard]] dm::float3 getViewDirection() const override
        {
            return leftView.getViewDirection();
        }

        [[nodiscard]] dm::frustum getViewFrustum() const override
        {
            dm::frustum left = leftView.getViewFrustum();
            dm::frustum right = rightView.getViewFrustum();

            // not robust but should work for regular stereo views
            left.planes[dm::frustum::RIGHT_PLANE] = right.planes[dm::frustum::RIGHT_PLANE];

            return left;
        }

        [[nodiscard]] dm::frustum getProjectionFrustum() const override
        {
            dm::frustum left = leftView.getProjectionFrustum();
            dm::frustum right = rightView.getProjectionFrustum();

            // not robust but should work for regular stereo views
            left.planes[dm::frustum::RIGHT_PLANE] = right.planes[dm::frustum::RIGHT_PLANE];

            return left;
        }

        [[nodiscard]] dm::affine3 getViewMatrix() const override
        {
            assert(false);
            return dm::affine3::identity();
        }

        [[nodiscard]] dm::affine3 getInverseViewMatrix() const override
        {
            assert(false);
            return dm::affine3::identity();
        }

        [[nodiscard]] dm::float4x4 getProjectionMatrix(bool includeOffset = true) const override
        {
            assert(false);
            (void)includeOffset;
            return dm::float4x4::identity();
        }

        [[nodiscard]] dm::float4x4 getInverseProjectionMatrix(bool includeOffset = true) const override
        {
            assert(false);
            (void)includeOffset;
            return dm::float4x4::identity();
        }

        [[nodiscard]] dm::float4x4 getViewProjectionMatrix(bool includeOffset = true) const override
        {
            assert(false);
            (void)includeOffset;
            return dm::float4x4::identity();
        }

        [[nodiscard]] dm::float4x4 getInverseViewProjectionMatrix(bool includeOffset = true) const override
        {
            assert(false);
            (void)includeOffset;
            return dm::float4x4::identity();
        }

        [[nodiscard]] ScissorDesc getViewExtent() const override
        {
            const ScissorDesc left = leftView.getViewExtent();
            const ScissorDesc right = rightView.getViewExtent();

            return ScissorDesc(
                std::min(left.minX, right.minX),
                std::max(left.maxX, right.maxX),
                std::min(left.minY, right.minY),
                std::max(left.maxY, right.maxY));
        }

        [[nodiscard]] uint32_t getNumChildViews(ViewType::Enum supportedTypes) const override
        {
            if (supportedTypes & ViewType::STEREO)
                return 1;

            return 2;
        }

        [[nodiscard]] const IView* getChildView(ViewType::Enum supportedTypes, uint32_t index) const override
        {
            if (supportedTypes & ViewType::STEREO)
            {
                assert(index == 0);
                return this;
            }

            assert(index < 2);
            if (index == 0)
                return &leftView;
            return &rightView;
        }

        [[nodiscard]] dm::float2 getPixelOffset() const override
        {
            return leftView.getPixelOffset();
        }
    };

    typedef StereoView<PlanarView> StereoPlanarView;

    class CubemapView : public IView
    {
    protected:
        typedef IView Super;

        PlanarView m_faceViews[6];
        dm::affine3 m_viewMatrix = dm::affine3::identity();
        dm::affine3 m_viewMatrixInv = dm::affine3::identity();
        dm::float4x4 m_projMatrix = dm::float4x4::identity();
        dm::float4x4 m_projMatrixInv = dm::float4x4::identity();
        dm::float4x4 m_viewProjMatrix = dm::float4x4::identity();
        dm::float4x4 m_viewProjMatrixInv = dm::float4x4::identity();
        float m_cullDistance = 1.f;
        float m_nearPlane = 1.f;
        dm::float3 m_center = dm::float3::zero();
        dm::box3 m_cullingBox = dm::box3::empty();
        int m_firstArraySlice = 0;
        bool m_cacheValid = false;

        void ensureCacheIsValid() const;

    public:
        void setTransform(dm::affine3 viewMatrix, float zNear, float cullDistance, bool useReverseInfiniteProjections = true);
        void setArrayViewports(int resolution, int firstArraySlice);
        void updateCache();

        [[nodiscard]] float getNearPlane() const;
        [[nodiscard]] dm::box3 getCullingBox() const;

        [[nodiscard]] ViewportStateDesc getViewportState() const override;
        [[nodiscard]] TextureSubresourceDesc getSubresources() const override;
        [[nodiscard]] bool isReverseDepth() const override;
        [[nodiscard]] bool isOrthographicProjection() const override;
        [[nodiscard]] bool isStereoView() const override;
        [[nodiscard]] bool isCubemapView() const override;
        [[nodiscard]] bool isBoxVisible(const dm::box3& bbox) const override;
        [[nodiscard]] bool isMirrored() const override;
        [[nodiscard]] dm::float3 getViewOrigin() const override;
        [[nodiscard]] dm::float3 getViewDirection() const override;
        [[nodiscard]] dm::frustum getViewFrustum() const override;
        [[nodiscard]] dm::frustum getProjectionFrustum() const override;
        [[nodiscard]] dm::affine3 getViewMatrix() const override;
        [[nodiscard]] dm::affine3 getInverseViewMatrix() const override;
        [[nodiscard]] dm::float4x4 getProjectionMatrix(bool includeOffset = true) const override;
        [[nodiscard]] dm::float4x4 getInverseProjectionMatrix(bool includeOffset = true) const override;
        [[nodiscard]] dm::float4x4 getViewProjectionMatrix(bool includeOffset = true) const override;
        [[nodiscard]] dm::float4x4 getInverseViewProjectionMatrix(bool includeOffset = true) const override;
        [[nodiscard]] ScissorDesc getViewExtent() const override;
        [[nodiscard]] dm::float2 getPixelOffset() const override;

        [[nodiscard]] uint32_t getNumChildViews(ViewType::Enum supportedTypes) const override;
        [[nodiscard]] const IView* getChildView(ViewType::Enum supportedTypes, uint32_t index) const override;

        static uint32_t* getCubemapCoordinateSwizzle();
    };
}
