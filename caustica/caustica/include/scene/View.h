#pragma once

#include <math/math.h>
#include <scene/ViewDesc.h>

struct PlanarViewConstants;

namespace caustica
{
    // Per-frame camera snapshot: matrices, viewport, and cached derived state.
    // Not a polymorphic view, and not a stereo/cubemap composite.
    class ViewInfo
    {
    protected:
        ViewportDesc m_viewport;
        ScissorDesc m_scissorRect;
        math::affine3 m_viewMatrix = math::affine3::identity();
        math::float4x4 m_projMatrix = math::float4x4::identity();
        math::float2 m_pixelOffset = math::float2::zero();
        int m_arraySlice = 0;

        math::float4x4 m_pixelOffsetMatrix = math::float4x4::identity();
        math::float4x4 m_pixelOffsetMatrixInv = math::float4x4::identity();
        math::float4x4 m_viewProjMatrix = math::float4x4::identity();
        math::float4x4 m_viewProjOffsetMatrix = math::float4x4::identity();
        math::affine3 m_viewMatrixInv = math::affine3::identity();
        math::float4x4 m_projMatrixInv = math::float4x4::identity();
        math::float4x4 m_viewProjMatrixInv = math::float4x4::identity();
        math::float4x4 m_viewProjOffsetMatrixInv = math::float4x4::identity();
        math::frustum m_viewFrustum = math::frustum::empty();
        math::frustum m_projectionFrustum = math::frustum::empty();
        bool m_reverseDepth = false;
        bool m_isMirrored = false;
        bool m_cacheValid = false;

        void ensureCacheIsValid() const;

    public:
        void setViewport(const ViewportDesc& viewport);
        void setMatrices(const math::affine3& viewMatrix, const math::float4x4& projMatrix);
        void setPixelOffset(math::float2 offset);
        void setArraySlice(int arraySlice);
        void updateCache();

        [[nodiscard]] const ViewportDesc& getViewport() const { return m_viewport; }
        [[nodiscard]] const ScissorDesc& getScissorRect() const { return m_scissorRect; }

        [[nodiscard]] ViewportStateDesc getViewportState() const;
        [[nodiscard]] TextureSubresourceDesc getSubresources() const;
        [[nodiscard]] bool isReverseDepth() const;
        [[nodiscard]] bool isOrthographicProjection() const;
        [[nodiscard]] bool isBoxVisible(const math::box3& bbox) const;
        [[nodiscard]] bool isMirrored() const;
        [[nodiscard]] math::float3 getViewOrigin() const;
        [[nodiscard]] math::float3 getViewDirection() const;
        [[nodiscard]] math::frustum getViewFrustum() const;
        [[nodiscard]] math::frustum getProjectionFrustum() const;
        [[nodiscard]] math::affine3 getViewMatrix() const;
        [[nodiscard]] math::affine3 getInverseViewMatrix() const;
        [[nodiscard]] math::float4x4 getProjectionMatrix(bool includeOffset = true) const;
        [[nodiscard]] math::float4x4 getInverseProjectionMatrix(bool includeOffset = true) const;
        [[nodiscard]] math::float4x4 getViewProjectionMatrix(bool includeOffset = true) const;
        [[nodiscard]] math::float4x4 getInverseViewProjectionMatrix(bool includeOffset = true) const;
        [[nodiscard]] ScissorDesc getViewExtent() const;
        [[nodiscard]] math::float2 getPixelOffset() const;
    };

    void fillViewConstants(PlanarViewConstants& constants, const ViewInfo& view);
}
