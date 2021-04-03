#define K_SCALE_FACTOR 0.9881813556f

RWTexture2D<float4> gDTreeSums;

cbuffer RescaleBuf
{
    uint2 gRelevantTexSize;
};

[numthreads(8, 4, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (any(DTid.xy >= gRelevantTexSize))
        return;

    gDTreeSums[DTid.xy] *= K_SCALE_FACTOR;
}
