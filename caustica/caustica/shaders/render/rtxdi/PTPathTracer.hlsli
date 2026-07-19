#ifndef CAUSTICA_RESTIR_PT_PATH_TRACER_HLSLI
#define CAUSTICA_RESTIR_PT_PATH_TRACER_HLSLI

#include "../rtxdi/RtxdiApplicationBridge.hlsli"

#include <Rtxdi/PT/PathReconnectibility.hlsli>
#include <Rtxdi/PT/PathTracerRandomContext.hlsli>
#include <Rtxdi/PT/PathTracerContext.hlsli>
#include <Rtxdi/PT/Reservoir.hlsli>

void RAB_ReconnectionDenoiserCallback(RTXDI_PTReservoir neighborSample, RAB_Surface surface, inout RAB_PathTracerUserData userData)
{
}

RTXDI_BrdfRaySample CAUSTICA_RtxdiPT_ImportanceSampleBrdf(RAB_Surface surface, inout RTXDI_RandomSamplerState rng)
{
    RTXDI_BrdfRaySample sample = RTXDI_EmptyBrdfRaySample();
    RTXDI_BrdfRaySampleProperties properties = RTXDI_DefaultBrdfRaySampleProperties();
    float3 outDir = 0.0.xxx;

    if (RAB_SurfaceImportanceSampleBsdf(surface, rng, outDir, properties))
    {
        sample.OutDirection = outDir;
        sample.properties = properties;
        sample.OutPdf = RAB_SurfaceEvaluateBsdfPdf(surface, outDir, properties);
        sample.BrdfTimesNoL = RAB_SurfaceEvaluateBsdfTimesNoL(surface, outDir, properties.IsDelta());
    }

    return sample;
}

RayDesc CAUSTICA_RtxdiPT_SetupContinuationRay(RAB_Surface surface, float3 outDirection)
{
    RayDesc ray = (RayDesc)0;
    ray.Origin = surface.ComputeNewRayOrigin(dot(outDirection, surface.GetFaceNCorrected()) >= 0.0f);
    ray.Direction = normalize(outDirection);
    ray.TMin = g_RtxdiBridgeConst.rayEpsilon;
    ray.TMax = RTXDI_MAX_FLOAT32;
    return ray;
}

RAB_Surface CAUSTICA_RtxdiPT_CollectSurface(PathTracer::SurfaceData surfaceData, float viewDepth)
{
    const ShadingData shadingData = surfaceData.shadingData;
    const ActiveBSDF bsdf = surfaceData.bsdf;

    uint lobes = bsdf.getLobes(shadingData);
    if ((lobes & (uint)LobeType::NonDeltaReflection) == 0)
        return RAB_EmptySurface();

    return PathTracerCollectedSurfaceData::create(
        shadingData.mtl,
        shadingData.materialID,
        shadingData.T,
        shadingData.B,
        shadingData.N,
        shadingData.V,
        shadingData.posW,
        shadingData.faceNCorrected,
        shadingData.frontFacing,
        viewDepth,
        0,
        bsdf.data);
}

PathTracer::SurfaceData CAUSTICA_RtxdiPT_LoadSurfaceFromPayload(RAB_RayPayload payload, RayDesc ray, uint bounceDepth)
{
    DebugContext debug;
    debug.Init(g_Const.debug, u_FeedbackBuffer, u_DebugLinesBuffer, u_DebugDeltaPathTree, u_DeltaPathSearchStack);

    RayCone rayCone = RayCone::make(0.0f, g_Const.ptConsts.camera.PixelConeSpreadAngle);

    return Bridge::loadSurface(
        payload.instanceIndex,
        payload.geometryIndex,
        payload.primitiveIndex,
        payload.barycentrics,
        ray.Direction,
        rayCone,
        int(bounceDepth + 1),
        uint2(0, 0),
        debug);
}

void RAB_PathTrace(inout RTXDI_PathTracerContext ctx, inout RTXDI_PathTracerRandomContext ptRandContext, inout RAB_PathTracerUserData userData)
{
    while (ctx.GetBounceDepth() <= ctx.GetMaxPathBounce())
    {
        ctx.BeginPathState();

        RTXDI_BrdfRaySample brdfSample = CAUSTICA_RtxdiPT_ImportanceSampleBrdf(ctx.GetIntersectionSurface(), ptRandContext.replayRandomSamplerState);
        ctx.SetBrdfRaySample(brdfSample);

        if (!ctx.ValidContinuationRayBrdfOverPdf())
            break;

        ctx.MultiplyPathThroughput(ctx.GetContinuationRayBrdfOverPdf());
        ctx.SetContinuationRay(CAUSTICA_RtxdiPT_SetupContinuationRay(ctx.GetIntersectionSurface(), brdfSample.OutDirection));

        if (!ctx.AnalyzePathReconnectibilityBeforeTrace())
            break;

        RAB_Surface previousSurface = ctx.GetIntersectionSurface();
        RayDesc continuationRay = ctx.GetContinuationRay();
        RAB_RayPayload payload = RAB_TraceNextBounce(continuationRay);
        ctx.SetTraceResult(payload);

        if (RAB_IsValidHit(payload))
        {
            PathTracer::SurfaceData hitSurfaceData = CAUSTICA_RtxdiPT_LoadSurfaceFromPayload(payload, continuationRay, ctx.GetBounceDepth());
            RAB_Surface hitSurface = CAUSTICA_RtxdiPT_CollectSurface(hitSurfaceData, RAB_RayPayloadGetCommittedHitT(payload));

            if (!RAB_IsSurfaceValid(hitSurface))
                break;

            ctx.RecordPathIntersection(hitSurface);

            if (ctx.ShouldSampleEmissiveSurfaces() && any(hitSurfaceData.shadingData.emission > 0.0.xxx))
            {
                ctx.RecordEmissiveLightSample(hitSurfaceData.shadingData.emission, previousSurface, ptRandContext.initialRandomSamplerState);
                if (ctx.IsPathTerminated())
                    break;
            }
        }
        else
        {
            ctx.RecordPathRadianceMiss(ptRandContext.initialRandomSamplerState);

            float3 environmentRadiance = RAB_GetEnvironmentRadiance(continuationRay.Direction);
            if (any(environmentRadiance > 0.0.xxx))
                ctx.RecordEnvironmentMapLightSample(environmentRadiance, previousSurface, ptRandContext.initialRandomSamplerState);

            break;
        }

        ctx.IncreaseBounceDepth();
    }
}

#if defined(RTXDI_RESTIR_PT_INITIAL_SAMPLING)
#include <Rtxdi/PT/InitialSampling.hlsli>
#elif defined(RTXDI_RESTIR_PT_HYBRID_SHIFT)
#include <Rtxdi/PT/HybridShift.hlsli>
#endif

#endif // CAUSTICA_RESTIR_PT_PATH_TRACER_HLSLI
