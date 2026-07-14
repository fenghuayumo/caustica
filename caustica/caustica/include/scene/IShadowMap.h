#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>

struct ShadowConstants;

namespace caustica
{

class ICompositeView;

class IShadowMap
{
public:
    virtual ~IShadowMap() = default;
    virtual dm::float4x4 getWorldToUvzwMatrix() const = 0;
    virtual const ICompositeView& getView() const = 0;
    virtual nvrhi::ITexture* getTexture() const = 0;
    virtual uint32_t getNumberOfCascades() const = 0;
    virtual const IShadowMap* getCascade(uint32_t index) const = 0;
    virtual uint32_t getNumberOfPerObjectShadows() const = 0;
    virtual const IShadowMap* getPerObjectShadow(uint32_t index) const = 0;
    virtual dm::int2 getTextureSize() const = 0;
    virtual dm::box2 getUVRange() const = 0;
    virtual dm::float2 getFadeRangeInTexels() const = 0;
    virtual bool isLitOutOfBounds() const = 0;
    virtual void fillShadowConstants(ShadowConstants& constants) const = 0;
};

} // namespace caustica
