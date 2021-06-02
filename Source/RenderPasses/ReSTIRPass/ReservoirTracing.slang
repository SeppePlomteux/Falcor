#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Utils.Math.MathHelpers;
import RenderPasses.Shared.Reservoir;
import Experimental.Scene.Lights.EnvMap;
import Experimental.Scene.Material.MaterialShading;

// Inputs
Texture2D<float4> shPackedInputReservoirData1;
Texture2D<float4> shPackedInputReservoirData2;
Texture2D<uint> shPackedInputReservoirData3;
Texture2D<float4> shWorldPos;
Texture2D<float4> shShadingNormal;
Texture2D<float4> shGeoNormal;
Texture2D<float4> shDiffColorOp;
Texture2D<float4> shSpecRoughness;
Texture2D<float4> shEmissiveColor;
Texture2D<float4> shMatParams;
Texture2D<float4> shViewVec;
Texture2D<float4> shShadowRayOrigin;

RWTexture2D<float4> shOutputColor;

static const bool kUseEnvLight = USE_ENV_LIGHT;

static const bool kDoVisbilityReuse = DO_VISIBILITY_REUSE;

static const bool kClampSamples = CLAMP_RESULT;
static const float kMaxClampValue = MAX_CLAMP;

struct ShadowRayPayload
{
    bool visible;
};

struct DummyScatterRayPayload
{
    bool _dummy;
};

MaterialParams loadMaterialParams(uint2 pixelPos)
{
    MaterialParams matParams;
    matParams.diffuseOpacity = shDiffColorOp[pixelPos];
    matParams.specularRoughness = shSpecRoughness[pixelPos];
    matParams.emissive = shEmissiveColor[pixelPos];
    matParams.extraParams = shMatParams[pixelPos];

    return matParams;
}

float3 getPrimaryRayDir(uint2 launchIndex, uint2 launchDim, const Camera camera)
{
    //return camera.computeRayPinhole(launchIndex, launchDim).dir;
    return -shViewVec[launchIndex].xyz;
}

/** Shadow ray handlers ************************/
[shader("miss")]
void shadowRayMiss(inout ShadowRayPayload rayData : SV_RayPayload)
{
    // Miss shader execution means no obstruction found. Mark as visible
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

/** Dummy scatter ray handlers *****************/
[shader("miss")]
void scatterMiss(inout DummyScatterRayPayload rayData : SV_RayPayload)
{
    return;
}

[shader("anyhit")]
void scatterAnyHit(
    uniform HitShaderParams hitParams,
    inout DummyScatterRayPayload rayData : SV_RayPayload,
    BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    return;
}

[shader("closesthit")]
void scatterClosestHit(
    uniform HitShaderParams hitParams,
    inout DummyScatterRayPayload rayData : SV_RayPayload,
    BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes)
{
    return;
}

[shader("raygeneration")]
void main()
{
    uint2 pixelPosition = DispatchRaysIndex().xy;
    uint2 canvasDim = DispatchRaysDimensions().xy;

    const float4 packedData1 = shPackedInputReservoirData1[pixelPosition];
    const float4 packedData2 = shPackedInputReservoirData2[pixelPosition];
    const uint packedData3 = shPackedInputReservoirData3[pixelPosition];
    
    //float3 outColor = float3(0.f);
    float3 outColor = shEmissiveColor[pixelPosition].xyz;
    
    float4 worldPos = shWorldPos[pixelPosition];
    
    float3 primaryRayDir = gScene.camera.computeRayPinhole(pixelPosition, canvasDim).dir; // Does not work for depth of field

    if (worldPos.w == 0)
    {
        outColor = kUseEnvLight ? gScene.envMap.eval(primaryRayDir) : float3(0., 0., 0.);
    }
    else if (packedData1.w >= 0 && packedData2.w > FLT_MIN && packedData3 > 0) // ignore if pdf is too low
    {
        //outColor += shEmissiveColor[pixelPosition].xyz;

        Reservoir r = createReservoir(packedData1, packedData2, packedData3);

        float3 normal = shShadingNormal[pixelPosition].xyz;
        float4 tangent = float4(perp_stark(normal), 1);
        float3 geoNormal = shGeoNormal[pixelPosition].xyz;
        GeometryParams geoParams = prepareGeometryParams(worldPos.xyz, -primaryRayDir, normal, tangent, geoNormal);

        // Load material parameters from G-buffer
        MaterialParams matParams = loadMaterialParams(pixelPosition);

        // Prepare shading data struct
        ShadingData sd = prepareShadingData(geoParams, matParams);

        if (kDoVisbilityReuse)
        {
            float3 toLight = r.mSamplePoint - shShadowRayOrigin[pixelPosition].xyz;
        
            float3 direction = normalize(toLight);
            outColor += (r.mIncommingRadiance / r.pdf) * evalBSDFCosine(sd, direction) * (r.mTotalWeight / (float) r.mTotalSampleCount);
        }
        else
        {
            RayDesc ray;
            ray.Origin = shShadowRayOrigin[pixelPosition].xyz;
        
            float3 toLight = r.mSamplePoint - ray.Origin;
            
            ray.Direction = normalize(toLight);
            ray.TMin = 0.f;
            ray.TMax = length(toLight);

            ShadowRayPayload rayData;
            rayData.visible = false; // Set to true by miss shader if ray is not terminated before
        
            TraceRay(gRtScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff /* instanceInclusionMask */, 1 /* hitIdx */, hitProgramCount, 1 /* missIdx */, ray, rayData);
            outColor += rayData.visible ? r.mIncommingRadiance * evalBSDFCosine(sd, ray.Direction) / r.pdf * r.mTotalWeight / (float) r.mTotalSampleCount : float3(0.f);
        }
    }
    
    if (kClampSamples)
        outColor = clamp(outColor, 0, kMaxClampValue);
    shOutputColor[pixelPosition] = float4(outColor, 1);
}
