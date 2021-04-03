#include "ErrorMetrics.slangh"

Texture2D<float4> shInputColor;
Texture2D<float4> shReference;
Texture2D<float4> shWorldPos;
RWTexture2D<float4> shOutputDiff;

// Used defines:
// K_ERR_METRIC

cbuffer buf
{
    uint4 gToMap;
    //uint gCalcAbsoluteDiff; OLD
};

float4 calculateAbsoluteError(uint2 imagePos)
{
    return abs(shInputColor[imagePos] - shReference[imagePos]);
}

float4 calculateSquaredError(uint2 imagePos)
{
    float4 temp = shInputColor[imagePos] - shReference[imagePos];
    return temp * temp;
}

float4 calculateSMAPE(uint2 imagePos)
{
    float4 image = shInputColor[imagePos];
    float4 reference = shReference[imagePos];
    return saturate(abs(image - reference) / (abs(image) + abs(reference)));
}

[numthreads(16, 16, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint2 relPos = DTid.xy;
    uint2 imagePos = DTid.xy + gToMap.xy;
    if (any(imagePos > gToMap.zw))
    {
        return;
    }
    // OLD CODE, delete if everything works
    //float4 diff = shWorldPos[imagePos].w != 0.f ? abs(shInputColor[imagePos] - shReference[imagePos]) : float4(0.f);
    //diff = gCalcAbsoluteDiff ? diff : diff * diff;
    float4 diff = float4(0.f);
    if (K_ERR_METRIC == K_ABSOLUTE_ERROR)
        diff = calculateAbsoluteError(imagePos);
    else if (K_ERR_METRIC == K_SQUARED_ERROR)
        diff = calculateSquaredError(imagePos);
    else if (K_ERR_METRIC == K_SMAPE)
        diff = calculateSMAPE(imagePos);
    diff = shWorldPos[imagePos].w != 0.f ? diff : float4(0.f);
    shOutputDiff[relPos] = diff;
}
