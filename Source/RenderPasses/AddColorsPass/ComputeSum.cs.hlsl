
#define K_IGNORE_NON_FINITE_VALS 0

Texture2D<float4> gFirstInputColor;
Texture2D<float4> gSecondInputColor;

RWTexture2D<float4> gOutputColor;

cbuffer SumBuf
{
    uint2 gTextureSize;
};

[numthreads(8, 4, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (any(DTid.xy >= gTextureSize))
    {
        return;
    }

    if (!K_IGNORE_NON_FINITE_VALS)
        gOutputColor[DTid.xy] = float4(gFirstInputColor[DTid.xy].xyz + gSecondInputColor[DTid.xy].xyz, 1.f);
    else
    {
        float4 firstCol = gFirstInputColor[DTid.xy];
        firstCol = isfinite(firstCol) ? firstCol : float4(0.f, 0.f, 0.f, 1.f);
        float4 secondCol = gFirstInputColor[DTid.xy];
        secondCol = isfinite(secondCol) ? secondCol : float4(0.f, 0.f, 0.f, 1.f);
        gOutputColor[DTid.xy] = float4(firstCol.xyz + secondCol.xyz, 1.f);
    }
}
