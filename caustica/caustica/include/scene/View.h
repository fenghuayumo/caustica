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
        dm::affine3 m_viewMatrix = dm::affine3::identity();
        dm::float4x4 m_projMatrix = dm::float4x4::identity();
        dm::float2 m_pixelOffset = dm::float2::zero();
        int m_arraySlice = 0;

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

        [[nodiscard]] ViewportStateDesc getViewportState() const;
        [[nodiscard]] TextureSubresourceDesc getSubresources() const;
        [[nodiscard]] bool isReverseDepth() const;
        [[nodiscard]] bool isOrthographicProjection() const;
        [[nodiscard]] bool isBoxVisible(const dm::box3& bbox) const;
        [[nodiscard]] bool isMirrored() const;
        [[nodiscard]] dm::float3 getViewOrigin() const;
        [[nodiscard]] dm::float3 getViewDirection() const;
        [[nodiscard]] dm::frustum getViewFrustum() const;
        [[nodiscard]] dm::frustum getProjectionFrustum() const;
        [[nodiscard]] dm::affine3 getViewMatrix() const;
        [[nodiscard]] dm::affine3 getInverseViewMatrix() const;
        [[nodiscard]] dm::float4x4 getProjectionMatrix(bool includeOffset = true) const;
        [[nodiscard]] dm::float4x4 getInverseProjectionMatrix(bool includeOffset = true) const;
        [[nodiscard]] dm::float4x4 getViewProjectionMatrix(bool includeOffset = true) const;
        [[nodiscard]] dm::float4x4 getInverseViewProjectionMatrix(bool includeOffset = true) const;
        [[nodiscard]] ScissorDesc getViewExtent() const;
        [[nodiscard]] dm::float2 getPixelOffset() const;
    };

    void fillViewConstants(PlanarViewConstants& constants, const ViewInfo& view);
}
