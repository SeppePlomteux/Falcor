/**
    Samples per reservoir = SAMPLES_PER_RESERVOIR
*/

#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Experimental.Scene.Material.MaterialShading;
import Experimental.Scene.Lights.LightHelpers;

import RenderPasses.Shared.PathTracer.PathTracerHelpers;
import RenderPasses.Shared.Reservoir;

// Inputs
Texture2D<float4> shWorldPos;
Texture2D<float4> shShadingNormal;
Texture2D<float4> shGeoNormal;
Texture2D<float4> shDiffColorOp;
Texture2D<float4> shSpecRoughness;
Texture2D<float4> shEmissiveColor;
Texture2D<float4> shMatParams;
Texture2D<float4> shViewVec;

// Outputs
RWTexture2D<float4> shPackedOutputReservoirData1;
RWTexture2D<float4> shPackedOutputReservoirData2;
RWTexture2D<uint> shPackedOutputReservoirData3;
RWTexture2D<float4> shShadowRayOrigin;

static const bool kDoVisbilityReuse = DO_VISIBILITY_REUSE;

struct SharedTracerData
{
    PathTracerParams params;
    EnvMapSampler envMapSampler;
    EmissiveLightSampler emissiveLightSampler;
};

struct ShadowRayPayload
{
    bool visible;
};

struct DummyPayload
{
    bool _dummy;
};

ParameterBlock<SharedTracerData> gData;

MaterialParams loadMaterialParams(uint2 pixelPos)
{
    MaterialParams matParams;
    matParams.diffuseOpacity = shDiffColorOp[ pixelPos];
    matParams.specularRoughness = shSpecRoughness[ pixelPos];
    matParams.emissive = shEmissiveColor[ pixelPos];
    matParams.extraParams = shMatParams[ pixelPos];

    return matParams;
}

float3 getPrimaryRayDir(uint2 launchIndex, uint2 launchDim, const Camera camera)
{
    //return camera.computeRayPinhole(launchIndex, launchDim).dir;
    return -shViewVec[launchIndex].xyz;
}

/** Shadow ray handlers */

[shader("miss")]
void shadowRayMiss(inout ShadowRayPayload rayData : SV_RayPayload)
{
    rayData.visible = true;
}

[shader("anyhit")]
void shadowRayHit(
    uniform HitShaderParams hitParams,
    inout ShadowRayPayload rayData : SV_RayPayload,
    BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    // alpha test, to ignore fully opaque geometry
    VertexData v = getVertexData(hitParams, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(hitParams.getGlobalHitID());
    if (alphaTest(v, gScene.materials[materialID], gScene.materialResources[materialID], 0.f))
        IgnoreHit();
}

/** Useless method stubs start here */
[shader("miss")]
void scatterMiss(inout DummyPayload rayData : SV_RayPayload)
{
    return;
}

[shader("anyhit")]
void scatterAnyHit(
    uniform HitShaderParams hitParams,
    inout DummyPayload rayData : SV_RayPayload,
    BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    return;
}

[shader("closesthit")]
void scatterClosestHit(
    uniform HitShaderParams hitParams,
    inout DummyPayload rayData : SV_RayPayload,
    BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    return;
}
/** Useless method stubs stop here  */

[shader("raygeneration")]
void main()
{
    uint2 pixelLocation = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    
    const float3 rayDir = getPrimaryRayDir(pixelLocation, launchDim, gScene.camera);
    const float4 worldPos = shWorldPos[pixelLocation];
    if (worldPos.w != 0.f)
    {
        Reservoir r = createReservoir();
        // Pixel represents a valid primary hit. Compute direct lighting

        // Load geometry parameters from G-buffer
        float3 normal = shShadingNormal[pixelLocation].xyz;
        float4 tangent = float4(perp_stark(normal), 1);
        float3 geoNormal = shGeoNormal[pixelLocation].xyz;
        GeometryParams geoParams = prepareGeometryParams(worldPos.xyz, -rayDir, normal, tangent, geoNormal);

        // Load material parameters from G-buffer
        MaterialParams matParams = loadMaterialParams(pixelLocation);

        // Prepare shading data struct
        ShadingData sd = prepareShadingData(geoParams, matParams);

        // Create sample generator
        SampleGenerator sg = SampleGenerator.create(pixelLocation, gData.params.frameCount);

        // Advance the generator to the first available dimension. (nu in comments)
        // TODO: This is potentially expensive. We may want to store/restore the state from memory if it becomes a problem.
        // for (uint i = 0; i < gPRNGDimension; i++) sampleNext1D(sg);

        // Compute ray origin for new rays spawned from the G-buffer
        const float3 rayOrigin = sd.computeNewRayOrigin();
        
        ReservoirLightSample rls = { };

        //[loop]
        for (int i = 0; i < SAMPLES_PER_RESERVOIR; i++)
        {
            bool valid = sampleSceneLightsReservoir(gData.params, gData.envMapSampler, gData.emissiveLightSampler, sd, rayOrigin, /*SAMPLES_PER_RESERVOIR*/1, sg, rls);
            // Weight = pdf/unshadowed light contribution
            //rls.samplePos = worldPos.xyz + normal;
            //rls.Li = float3(0.7, 0.7, 0.7);
            //rls.pdf = 0.8;
            //bool valid = true;
            
            float3 toLight = normalize(rls.samplePos - rayOrigin);
            float3 temp = evalBSDFCosine(sd, toLight) * rls.Li;
            float pdf = temp.x + temp.y + temp.z;
            float weight = pdf / max(FLT_MIN, rls.pdf);
            weight = valid ? weight : 0;

            //weight = isnan(weight) ? 0 : weight;
            //weight = length(rls.samplePos);
            updateReservoir(r, rls.samplePos, rls.Li, weight, pdf, sampleNext1D(sg));
            //r.mIncommingRadiance = evalBSDFCosine(sd, toLight);
            //r.mIncommingRadiance = float3(1, 1, 1);
            //r.mTotalWeight = 1;
            //r.mSamplePoint = float3(0, 0, 0);
            //r.mTotalSampleCount = 1;
            //r.pdf = 1;
        }

        // trace shadow ray

        if (kDoVisbilityReuse)
        {
            RayDesc ray;
            ray.Origin = rayOrigin;
        
            float3 toLight = r.mSamplePoint - rayOrigin;
        
            ray.Direction = normalize(toLight);
            ray.TMin = 0.f;
            ray.TMax = length(toLight);
        
            ShadowRayPayload payload;
            payload.visible = false;
            TraceRay(gRtScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff /* instanceInclusionMask */, 1 /* hitIdx */, hitProgramCount, 1 /* missIdx */, ray, payload);

            ////r.mTotalWeight = payload.visible * r.mTotalWeight; // Evnetueel ook M resetten
            r.mTotalSampleCount = payload.visible * r.mTotalSampleCount;
            r.pdf = payload.visible ? r.pdf : -1;
        }
        /* OLD CODE, DELETE IF COMPILER SUCCEEDS
        RayDesc ray;
        ray.Origin = rayOrigin;
        
        float3 toLight = r.mSamplePoint - rayOrigin;
        
        ray.Direction = normalize(toLight);
        ray.TMin = 0.f;
        ray.TMax = length(toLight);
        
        ShadowRayPayload payload;
        payload.visible = true;
        //TraceRay(gRtScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff , 1, hitProgramCount, 1, ray, payload);

        ////r.mTotalWeight = payload.visible * r.mTotalWeight; // Evnetueel ook M resetten
        r.mTotalSampleCount = payload.visible * r.mTotalSampleCount;
        r.pdf = payload.visible ? r.pdf : -1;
        */
        
        float4 packing1;
        float4 packing2;
        uint packing3;
        extractPackedData(r, packing1, packing2, packing3);

        shPackedOutputReservoirData1[pixelLocation] = packing1;
        shPackedOutputReservoirData2[pixelLocation] = packing2;
        shPackedOutputReservoirData3[pixelLocation] = packing3;
        shShadowRayOrigin[pixelLocation] = float4(rayOrigin, 0);
    }
    else
    {
        shPackedOutputReservoirData1[pixelLocation] = float4(0, 0, 0, -1);
        shPackedOutputReservoirData2[pixelLocation] = float4(0, 0, 0, 0);
        shPackedOutputReservoirData3[pixelLocation] = 0;
        shShadowRayOrigin[pixelLocation] = float4(0, 0, 0, 0);
    }
}
