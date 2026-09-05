#include <scene/View.h>

#include <cassert>

using namespace caustica::math;
using namespace caustica;

#include <shaders/view_cb.h>

void caustica::fillViewConstants(PlanarViewConstants& constants, const ViewInfo& view)
{
    constants.matWorldToView = affineToHomogeneous(view.getViewMatrix());
    constants.matViewToClip = view.getProjectionMatrix(true);
    constants.matWorldToClip = view.getViewProjectionMatrix(true);
    constants.matClipToView = view.getInverseProjectionMatrix(true);
    constants.matViewToWorld = affineToHomogeneous(view.getInverseViewMatrix());
    constants.matClipToWorld = view.getInverseViewProjectionMatrix(true);
    constants.matViewToClipNoOffset = view.getProjectionMatrix(false);
    constants.matWorldToClipNoOffset = view.getViewProjectionMatrix(false);
    constants.matClipToViewNoOffset = view.getInverseProjectionMatrix(false);
    constants.matClipToWorldNoOffset = view.getInverseViewProjectionMatrix(false);

    const ViewportStateDesc viewportState = view.getViewportState();
    const ViewportDesc& viewport = viewportState.viewports[0];
    constants.viewportOrigin = float2(viewport.minX, viewport.minY);
    constants.viewportSize = float2(viewport.width(), viewport.height());
    constants.viewportSizeInv = 1.f / constants.viewportSize;

    constants.clipToWindowScale = float2(0.5f * viewport.width(), -0.5f * viewport.height());
    constants.clipToWindowBias = constants.viewportOrigin + constants.viewportSize * 0.5f;

    constants.windowToClipScale = 1.f / constants.clipToWindowScale;
    constants.windowToClipBias = -constants.clipToWindowBias * constants.windowToClipScale;

    constants.cameraDirectionOrPosition = view.isOrthographicProjection()
        ? float4(view.getViewDirection(), 0.f)
        : float4(view.getViewOrigin(), 1.f);

    constants.pixelOffset = view.getPixelOffset();
}

void ViewInfo::updateCache()
{
    if (m_cacheValid)
        return;

    m_pixelOffsetMatrix = affineToHomogeneous(translation(
        float3(2.f * m_pixelOffset.x / (m_viewport.maxX - m_viewport.minX),
            -2.f * m_pixelOffset.y / (m_viewport.maxY - m_viewport.minY), 0.f)));
    m_pixelOffsetMatrixInv = inverse(m_pixelOffsetMatrix);

    m_viewProjMatrix = affineToHomogeneous(m_viewMatrix) * m_projMatrix;
    m_viewProjOffsetMatrix = m_viewProjMatrix * m_pixelOffsetMatrix;

    m_viewMatrixInv = inverse(m_viewMatrix);
    m_projMatrixInv = inverse(m_projMatrix);
    m_viewProjMatrixInv = m_projMatrixInv * affineToHomogeneous(m_viewMatrixInv);
    m_viewProjOffsetMatrixInv = m_pixelOffsetMatrixInv * m_viewProjMatrixInv;

    m_reverseDepth = (m_projMatrix[2][2] <= 0.f);
    m_viewFrustum = frustum(m_viewProjMatrix, m_reverseDepth);
    m_projectionFrustum = frustum(m_projMatrix, m_reverseDepth);

    m_isMirrored = determinant(m_viewMatrix.m_linear) < 0.f;

    m_cacheValid = true;
}

void ViewInfo::ensureCacheIsValid() const
{
    assert(m_cacheValid); // Call updateCache() after changing any view parameters
}

void ViewInfo::setViewport(const ViewportDesc& viewport)
{
    m_viewport = viewport;
    m_scissorRect = ScissorDesc(viewport);
    m_cacheValid = false;
}

void ViewInfo::setMatrices(const affine3& viewMatrix, const float4x4& projMatrix)
{
    m_viewMatrix = viewMatrix;
    m_projMatrix = projMatrix;
    m_cacheValid = false;
}

void ViewInfo::setPixelOffset(const float2 offset)
{
    m_pixelOffset = offset;
    m_cacheValid = false;
}

void ViewInfo::setArraySlice(int arraySlice)
{
    m_arraySlice = arraySlice;
}

ViewportStateDesc ViewInfo::getViewportState() const
{
    return ViewportStateDesc()
        .addViewport(m_viewport)
        .addScissorRect(m_scissorRect);
}

TextureSubresourceDesc ViewInfo::getSubresources() const
{
    return TextureSubresourceDesc(0, 1, m_arraySlice, 1);
}

bool ViewInfo::isReverseDepth() const
{
    ensureCacheIsValid();
    return m_reverseDepth;
}

bool ViewInfo::isOrthographicProjection() const
{
    return m_projMatrix[2][3] == 0.f;
}

float3 ViewInfo::getViewOrigin() const
{
    ensureCacheIsValid();
    return m_viewMatrixInv.m_translation;
}

float3 ViewInfo::getViewDirection() const
{
    ensureCacheIsValid();
    return m_viewMatrixInv.m_linear[2];
}

frustum ViewInfo::getViewFrustum() const
{
    ensureCacheIsValid();
    return m_viewFrustum;
}

frustum ViewInfo::getProjectionFrustum() const
{
    ensureCacheIsValid();
    return m_projectionFrustum;
}

affine3 ViewInfo::getViewMatrix() const
{
    return m_viewMatrix;
}

affine3 ViewInfo::getInverseViewMatrix() const
{
    ensureCacheIsValid();
    return m_viewMatrixInv;
}

float4x4 ViewInfo::getProjectionMatrix(bool includeOffset) const
{
    ensureCacheIsValid();
    return includeOffset ? m_projMatrix * m_pixelOffsetMatrix : m_projMatrix;
}

float4x4 ViewInfo::getInverseProjectionMatrix(bool includeOffset) const
{
    ensureCacheIsValid();
    return includeOffset ? m_pixelOffsetMatrixInv * m_projMatrixInv : m_projMatrixInv;
}

float4x4 ViewInfo::getViewProjectionMatrix(bool includeOffset) const
{
    ensureCacheIsValid();
    return includeOffset ? m_viewProjOffsetMatrix : m_viewProjMatrix;
}

float4x4 ViewInfo::getInverseViewProjectionMatrix(bool includeOffset) const
{
    ensureCacheIsValid();
    return includeOffset ? m_viewProjOffsetMatrixInv : m_viewProjMatrixInv;
}

ScissorDesc ViewInfo::getViewExtent() const
{
    return m_scissorRect;
}

float2 ViewInfo::getPixelOffset() const
{
    return m_pixelOffset;
}

bool ViewInfo::isBoxVisible(const math::box3& bbox) const
{
    ensureCacheIsValid();
    return m_viewFrustum.intersectsWith(bbox);
}

bool ViewInfo::isMirrored() const
{
    ensureCacheIsValid();
    return m_isMirrored;
}
