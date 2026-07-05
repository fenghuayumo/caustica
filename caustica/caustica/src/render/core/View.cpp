#include <scene/View.h>
#include <algorithm>

using namespace caustica::math;
using namespace caustica;

#include <shaders/view_cb.h>

void IView::fillPlanarViewConstants(PlanarViewConstants& constants) const
{
    constants.matWorldToView = affineToHomogeneous(getViewMatrix());
    constants.matViewToClip = getProjectionMatrix(true);
    constants.matWorldToClip = getViewProjectionMatrix(true);
    constants.matClipToView = getInverseProjectionMatrix(true);
    constants.matViewToWorld = affineToHomogeneous(getInverseViewMatrix());
    constants.matClipToWorld = getInverseViewProjectionMatrix(true);
    constants.matViewToClipNoOffset = getProjectionMatrix(false);
    constants.matWorldToClipNoOffset = getViewProjectionMatrix(false);
    constants.matClipToViewNoOffset = getInverseProjectionMatrix(false);
    constants.matClipToWorldNoOffset = getInverseViewProjectionMatrix(false);

    const ViewportStateDesc viewportState = getViewportState();
    const ViewportDesc& viewport = viewportState.viewports[0];
    constants.viewportOrigin = float2(viewport.minX, viewport.minY);
    constants.viewportSize = float2(viewport.width(), viewport.height());
    constants.viewportSizeInv = 1.f / constants.viewportSize;

    constants.clipToWindowScale = float2(0.5f * viewport.width(), -0.5f * viewport.height());
    constants.clipToWindowBias = constants.viewportOrigin + constants.viewportSize * 0.5f;

    constants.windowToClipScale = 1.f / constants.clipToWindowScale;
    constants.windowToClipBias = -constants.clipToWindowBias * constants.windowToClipScale;

    constants.cameraDirectionOrPosition = isOrthographicProjection()
        ? float4(getViewDirection(), 0.f)
        : float4(getViewOrigin(), 1.f);

    constants.pixelOffset = getPixelOffset();
}

void PlanarView::updateCache()
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

void PlanarView::ensureCacheIsValid() const
{
    assert(m_cacheValid); // Call updateCache() after changing any view parameters
}

void PlanarView::setViewport(const ViewportDesc& viewport)
{
    m_viewport = viewport;
    m_scissorRect = ScissorDesc(viewport);
    m_cacheValid = false;
}

void PlanarView::setMatrices(const affine3& viewMatrix, const float4x4& projMatrix)
{
    m_viewMatrix = viewMatrix;
    m_projMatrix = projMatrix;
    m_cacheValid = false;
}

void PlanarView::setPixelOffset(const float2 offset)
{
    m_pixelOffset = offset;
    m_cacheValid = false;
}

void PlanarView::setArraySlice(int arraySlice)
{
    m_arraySlice = arraySlice;
}

ViewportStateDesc PlanarView::getViewportState() const
{
    return ViewportStateDesc()
        .addViewport(m_viewport)
        .addScissorRect(m_scissorRect);
}

TextureSubresourceDesc PlanarView::getSubresources() const
{
    return TextureSubresourceDesc(0, 1, m_arraySlice, 1);
}

bool PlanarView::isReverseDepth() const
{
    ensureCacheIsValid();
    return m_reverseDepth;
}

bool PlanarView::isOrthographicProjection() const
{
    return m_projMatrix[2][3] == 0.f;
}

bool PlanarView::isStereoView() const
{
    return false;
}

bool PlanarView::isCubemapView() const
{
    return false;
}

float3 PlanarView::getViewOrigin() const
{
    ensureCacheIsValid();
    return m_viewMatrixInv.m_translation;
}

float3 PlanarView::getViewDirection() const
{
    ensureCacheIsValid();
    return m_viewMatrixInv.m_linear[2];
}

frustum PlanarView::getViewFrustum() const
{
    ensureCacheIsValid();
    return m_viewFrustum;
}

frustum PlanarView::getProjectionFrustum() const
{
    ensureCacheIsValid();
    return m_projectionFrustum;
}

affine3 PlanarView::getViewMatrix() const
{
    return m_viewMatrix;
}

affine3 PlanarView::getInverseViewMatrix() const
{
    ensureCacheIsValid();
    return m_viewMatrixInv;
}

float4x4 PlanarView::getProjectionMatrix(bool includeOffset) const
{
    ensureCacheIsValid();
    return includeOffset ? m_projMatrix * m_pixelOffsetMatrix : m_projMatrix;
}

float4x4 PlanarView::getInverseProjectionMatrix(bool includeOffset) const
{
    ensureCacheIsValid();
    return includeOffset ? m_pixelOffsetMatrixInv * m_projMatrixInv : m_projMatrixInv;
}

float4x4 PlanarView::getViewProjectionMatrix(bool includeOffset) const
{
    ensureCacheIsValid();
    return includeOffset ? m_viewProjOffsetMatrix : m_viewProjMatrix;
}

float4x4 PlanarView::getInverseViewProjectionMatrix(bool includeOffset) const
{
    ensureCacheIsValid();
    return includeOffset ? m_viewProjOffsetMatrixInv : m_viewProjMatrixInv;
}

ScissorDesc PlanarView::getViewExtent() const
{
    return m_scissorRect;
}

float2 PlanarView::getPixelOffset() const
{
    return m_pixelOffset;
}

bool PlanarView::isBoxVisible(const dm::box3& bbox) const
{
    ensureCacheIsValid();
    return m_viewFrustum.intersectsWith(bbox);
}

bool PlanarView::isMirrored() const
{
    ensureCacheIsValid();
    return m_isMirrored;
}

uint32_t IView::getNumChildViews(ViewType::Enum supportedTypes) const
{
    (void)supportedTypes;
    return 1;
}

const IView* IView::getChildView(ViewType::Enum supportedTypes, uint32_t index) const
{
    (void)supportedTypes;
    assert(index == 0);
    return this;
}

void CompositeView::addView(std::shared_ptr<IView> view)
{
    m_childViews.push_back(view);
}

uint32_t CompositeView::getNumChildViews(ViewType::Enum supportedTypes) const
{
    (void)supportedTypes;
    return uint32_t(m_childViews.size());
}

const IView* CompositeView::getChildView(ViewType::Enum supportedTypes, uint32_t index) const
{
    (void)supportedTypes;
    return m_childViews[index].get();
}

static const float3x3 g_CubemapViewMatrices[6] = {
    float3x3(
        0, 0, 1,
        0, 1, 0,
        -1, 0, 0
    ),
    float3x3(
        0, 0, -1,
        0, 1, 0,
        1, 0, 0
    ),
    float3x3(
        1, 0, 0,
        0, 0, 1,
        0, -1, 0
    ),
    float3x3(
        1, 0, 0,
        0, 0, -1,
        0, 1, 0
    ),
    float3x3(
        1, 0, 0,
        0, 1, 0,
        0, 0, 1
    ),
    float3x3(
        -1, 0, 0,
        0, 1, 0,
        0, 0, -1
    )
};

void CubemapView::ensureCacheIsValid() const
{
    assert(m_cacheValid); // Call updateCache() after changing any view parameters
}

void CubemapView::setTransform(affine3 viewMatrix, float zNear, float cullDistance, bool useReverseInfiniteProjections)
{
    m_viewMatrix = viewMatrix;
    m_nearPlane = zNear;
    m_cullDistance = cullDistance;

    float4x4 faceProjMatrix;
    if (useReverseInfiniteProjections)
        faceProjMatrix = float4x4(
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 0, 1,
            0, 0, zNear, 0
        );
    else
        faceProjMatrix = perspProjD3DStyle(-1.f, 1.f, -1.f, 1.f, zNear, cullDistance);

    for (int face = 0; face < 6; face++)
    {
        affine3 faceViewMatrix = m_viewMatrix * affine3(g_CubemapViewMatrices[face], float3(0.f));

        m_faceViews[face].setMatrices(faceViewMatrix, faceProjMatrix);
    }

    m_cacheValid = false;
}

void CubemapView::setArrayViewports(int resolution, int firstArraySlice)
{
    m_firstArraySlice = firstArraySlice;

    for (int face = 0; face < 6; face++)
    {
        m_faceViews[face].setViewport(ViewportDesc(float(resolution), float(resolution)));
        m_faceViews[face].setArraySlice(face + firstArraySlice);
    }
}

void CubemapView::updateCache()
{
    for (auto& view : m_faceViews)
        view.updateCache();

    if (m_cacheValid)
        return;

    m_viewMatrixInv = inverse(m_viewMatrix);
    m_projMatrix = affineToHomogeneous(scaling<float, 3>(1.0f / m_nearPlane));
    m_projMatrixInv = inverse(m_projMatrix);
    m_viewProjMatrix = affineToHomogeneous(m_viewMatrix) * m_projMatrix;
    m_viewProjMatrixInv = inverse(m_viewProjMatrix);
    m_center = inverse(m_viewMatrix).m_translation;
    m_cullingBox = box3(m_center - m_cullDistance, m_center + m_cullDistance);

    m_cacheValid = true;
}

float CubemapView::getNearPlane() const
{
    return m_nearPlane;
}

box3 CubemapView::getCullingBox() const
{
    ensureCacheIsValid();
    return m_cullingBox;
}

ViewportStateDesc CubemapView::getViewportState() const
{
    ViewportStateDesc result;

    for (const auto& faceView : m_faceViews)
    {
        result.addViewport(faceView.getViewport());
        result.addScissorRect(faceView.getScissorRect());
    }

    return result;
}

bool CubemapView::isBoxVisible(const dm::box3& bbox) const
{
    ensureCacheIsValid();

    if (m_cullDistance <= 0)
        return true;

    return m_cullingBox.intersects(bbox);
}

bool CubemapView::isMirrored() const
{
    return false;
}

TextureSubresourceDesc CubemapView::getSubresources() const
{
    return TextureSubresourceDesc(0, 1, m_firstArraySlice, 6);
}

bool CubemapView::isReverseDepth() const
{
    return true;
}

bool CubemapView::isOrthographicProjection() const
{
    return false;
}

bool CubemapView::isStereoView() const
{
    return false;
}

bool CubemapView::isCubemapView() const
{
    return true;
}

float3 CubemapView::getViewOrigin() const
{
    return m_center;
}

float3 CubemapView::getViewDirection() const
{
    assert(false);
    return 0.f;
}

frustum CubemapView::getViewFrustum() const
{
    ensureCacheIsValid();

    return frustum::fromBox(m_cullingBox);
}

frustum CubemapView::getProjectionFrustum() const
{
    ensureCacheIsValid();

    box3 b = box3(-m_cullDistance, m_cullDistance);
    return frustum::fromBox(b);
}

affine3 CubemapView::getViewMatrix() const
{
    return m_viewMatrix;
}

affine3 CubemapView::getInverseViewMatrix() const
{
    ensureCacheIsValid();
    return m_viewMatrixInv;
}

float4x4 CubemapView::getProjectionMatrix(bool includeOffset) const
{
    (void)includeOffset;
    ensureCacheIsValid();
    return m_projMatrix;
}

float4x4 CubemapView::getInverseProjectionMatrix(bool includeOffset) const
{
    (void)includeOffset;
    ensureCacheIsValid();
    return m_projMatrixInv;
}

float4x4 CubemapView::getViewProjectionMatrix(bool includeOffset) const
{
    (void)includeOffset;
    ensureCacheIsValid();
    return m_viewProjMatrix;
}

float4x4 CubemapView::getInverseViewProjectionMatrix(bool includeOffset) const
{
    (void)includeOffset;
    ensureCacheIsValid();
    return m_viewProjMatrixInv;
}

ScissorDesc CubemapView::getViewExtent() const
{
    return m_faceViews[0].getViewExtent();
}

float2 CubemapView::getPixelOffset() const
{
    return 0.f;
}

uint32_t CubemapView::getNumChildViews(ViewType::Enum supportedTypes) const
{
    if (supportedTypes & ViewType::CUBEMAP)
        return 1;

    return 6;
}

const IView* CubemapView::getChildView(ViewType::Enum supportedTypes, uint32_t index) const
{
    if (supportedTypes & ViewType::CUBEMAP)
        return this;

    assert(index < 6);
    return &m_faceViews[index];
}


typedef enum _NV_SWIZZLE_MODE
{
    NV_SWIZZLE_POS_X = 0,
    NV_SWIZZLE_NEG_X = 1,
    NV_SWIZZLE_POS_Y = 2,
    NV_SWIZZLE_NEG_Y = 3,
    NV_SWIZZLE_POS_Z = 4,
    NV_SWIZZLE_NEG_Z = 5,
    NV_SWIZZLE_POS_W = 6,
    NV_SWIZZLE_NEG_W = 7
}NV_SWIZZLE_MODE;

typedef enum _NV_SWIZZLE_OFFSET
{
    NV_SWIZZLE_OFFSET_X = 0,
    NV_SWIZZLE_OFFSET_Y = 4,
    NV_SWIZZLE_OFFSET_Z = 8,
    NV_SWIZZLE_OFFSET_W = 12
}NV_SWIZZLE_OFFSET;

static const uint32_t g_CubemapCoordinateSwizzle[16] = {
    (NV_SWIZZLE_NEG_Z << NV_SWIZZLE_OFFSET_X) | (NV_SWIZZLE_POS_Y << NV_SWIZZLE_OFFSET_Y) | (NV_SWIZZLE_POS_W << NV_SWIZZLE_OFFSET_Z) | (NV_SWIZZLE_POS_X << NV_SWIZZLE_OFFSET_W),
    (NV_SWIZZLE_POS_Z << NV_SWIZZLE_OFFSET_X) | (NV_SWIZZLE_POS_Y << NV_SWIZZLE_OFFSET_Y) | (NV_SWIZZLE_POS_W << NV_SWIZZLE_OFFSET_Z) | (NV_SWIZZLE_NEG_X << NV_SWIZZLE_OFFSET_W),
    (NV_SWIZZLE_POS_X << NV_SWIZZLE_OFFSET_X) | (NV_SWIZZLE_NEG_Z << NV_SWIZZLE_OFFSET_Y) | (NV_SWIZZLE_POS_W << NV_SWIZZLE_OFFSET_Z) | (NV_SWIZZLE_POS_Y << NV_SWIZZLE_OFFSET_W),
    (NV_SWIZZLE_POS_X << NV_SWIZZLE_OFFSET_X) | (NV_SWIZZLE_POS_Z << NV_SWIZZLE_OFFSET_Y) | (NV_SWIZZLE_POS_W << NV_SWIZZLE_OFFSET_Z) | (NV_SWIZZLE_NEG_Y << NV_SWIZZLE_OFFSET_W),
    (NV_SWIZZLE_POS_X << NV_SWIZZLE_OFFSET_X) | (NV_SWIZZLE_POS_Y << NV_SWIZZLE_OFFSET_Y) | (NV_SWIZZLE_POS_W << NV_SWIZZLE_OFFSET_Z) | (NV_SWIZZLE_POS_Z << NV_SWIZZLE_OFFSET_W),
    (NV_SWIZZLE_NEG_X << NV_SWIZZLE_OFFSET_X) | (NV_SWIZZLE_POS_Y << NV_SWIZZLE_OFFSET_Y) | (NV_SWIZZLE_POS_W << NV_SWIZZLE_OFFSET_Z) | (NV_SWIZZLE_NEG_Z << NV_SWIZZLE_OFFSET_W),
    0
};

uint32_t* CubemapView::getCubemapCoordinateSwizzle()
{
    return (uint32_t*)g_CubemapCoordinateSwizzle;
}
