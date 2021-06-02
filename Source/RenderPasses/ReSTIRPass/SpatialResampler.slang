#include "Utils/Math/MathConstants.slangh"

#define COS_25 0.9063077870366f;

import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import RenderPasses.Shared.Reservoir;
import Experimental.Scene.Material.MaterialShading;
import Scene.Raytracing;

// Reservoir input textures
Texture2D<float4> shPackedInputReservoirData1;
Texture2D<float4> shPackedInputReservoirData2;
Texture2D<uint> shPackedInputReservoirData3;

// Depth input
//Texture2D<float> shDepth;

// Ray origin input
Texture2D<float4> shShadowRayOrigin;

// Material input textures
Texture2D<float4> shWorldPos;
Texture2D<float4> shShadingNormal;
Texture2D<float4> shGeoNormal;
Texture2D<float4> shDiffColorOp;
Texture2D<float4> shSpecRoughness;
Texture2D<float4> shEmissiveColor;
Texture2D<float4> shMatParams;
Texture2D<float4> shViewVec;

// Reservoir output textures
RWTexture2D<float4> shPackedOutputReservoirData1;
RWTexture2D<float4> shPackedOutputReservoirData2;
RWTexture2D<uint> shPackedOutputReservoirData3;

cbuffer SharedBuffer
{
    uint gFrameCount;
    uint2 gImageSize;
};

ShadingData createShadingData(uint2 pixelPos)
{
    MaterialParams matParams;
    matParams.diffuseOpacity = shDiffColorOp[pixelPos];
    matParams.specularRoughness = shSpecRoughness[pixelPos];
    matParams.emissive = shEmissiveColor[pixelPos];
    matParams.extraParams = shMatParams[pixelPos];

    
    float3 toCamera = shViewVec[pixelPos].xyz;
    float3 normal = shShadingNormal[pixelPos].xyz;
    float4 tangent = float4(perp_stark(normal), 1);
    GeometryParams geoParams = prepareGeometryParams(
        shWorldPos[pixelPos].xyz,
        toCamera,
        normal,
        tangent,
        shGeoNormal[pixelPos].xyz);
    return prepareShadingData(geoParams, matParams);
}

[numthreads(8, 4, 1)] //32 Trheads per group, aangezien NVIDIA 32 threads per CU heeft
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint2 pixelPosition = DTid.xy;
    uint2 dimensions = gImageSize;

    if (any(pixelPosition > dimensions)) // Do nothing if thread lies outside of image bounds
        return;
    
    SampleGenerator sg = SampleGenerator.create(DTid.xy, gFrameCount);

    Reservoir r = createReservoir(  shPackedInputReservoirData1[pixelPosition],
                                    shPackedInputReservoirData2[pixelPosition],
                                    shPackedInputReservoirData3[pixelPosition]);

    ShadingData sd = createShadingData(pixelPosition);
    
    for (int i = 0; i < AMOUNT_OF_SAMPLES; i++)
    {
        float2 sample = sampleNext2D(sg);
        //sample = float2(30 * sqrt(sample.x), M_PI_2 * sample.y);
        //sample = float2(sample.x * cos(sample.y), sample.x * sin(sample.y));
        //sample = float2(sign(sample.x) * 1 + sample.x + pixelPosition.x, sign(sample.y) * 1 + sample.y + pixelPosition.y);
        
        sample = 16 * sample - 8;
        sample += sign(sample) * 1;
        
        // uint2 samplePos = (uint2) sample + pixelPosition; Dit werkt
        uint2 samplePos = pixelPosition + int2(sample);
        //uint2 samplePos = (uint2) sample;
        samplePos = max(samplePos, uint2(0));
        samplePos = min(samplePos, dimensions - 1);
        // check if samplePos != pixelPos, we willen niet dezelfde pixel resamplen
        Reservoir toCombine = createReservoir(  shPackedInputReservoirData1[samplePos],
                                                shPackedInputReservoirData2[samplePos],
                                                shPackedInputReservoirData3[samplePos]);
        
        float3 temp = evalBSDFCosine(sd,
            normalize(toCombine.mSamplePoint - shShadowRayOrigin[pixelPosition].xyz));
        //float3 temp = normalize(shWorldPos[samplePos].xyz - gCameraPos);
        temp *= toCombine.mIncommingRadiance;
        float newPDF = (temp.x + temp.y + temp.z);
        float test = toCombine.pdf == 0 ? 0 : newPDF / toCombine.pdf;
        toCombine.pdf = newPDF;
        toCombine.mTotalWeight *= test;
        //toCombine.mTotalWeight = 0;
        //r.mIncommingRadiance = float3(((float2) pixelPosition - samplePos)/16.f+0.5-1.f, 0);
        //r.mIncommingRadiance = float3(samplePos > pixelPosition ? 1.f : 0.f, 0);
        //r.mIncommingRadiance = float3(abs((int2) pixelPosition - (int2) samplePos) / 9.f, 0);
        
        bool sampleOK = dot(shGeoNormal[pixelPosition], shGeoNormal[samplePos]) > COS_25;
        sampleOK &= abs(shWorldPos[pixelPosition].w - shWorldPos[samplePos].w) < 0.1 * shWorldPos[pixelPosition].w;
        sampleOK &= all(samplePos != pixelPosition);
        if (sampleOK) // Late if test to minimise divergence
            combineReservoirs(r, toCombine, sampleNext1D(sg));
    }
    float4 outputData1 = float4(0, 0, 0, 0);
    float4 outputData2 = float4(0, 0, 0, 0);
    uint outputData3 = 0;
    extractPackedData(r, outputData1, outputData2, outputData3);
    shPackedOutputReservoirData1[pixelPosition] = outputData1;
    shPackedOutputReservoirData2[pixelPosition] = outputData2;
    shPackedOutputReservoirData3[pixelPosition] = outputData3;
}
