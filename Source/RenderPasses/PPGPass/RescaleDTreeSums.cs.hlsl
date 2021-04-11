#define K_SCALE_FACTOR 0.9881813556f

RWTexture2D<float4> gDTreeSums;

Texture1D<uint> gSTreeMetaData;

[numthreads(8, 4, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (DTid.y >= gSTreeMetaData[1])
        return;

    gDTreeSums[DTid.xy] *= K_SCALE_FACTOR;
}
