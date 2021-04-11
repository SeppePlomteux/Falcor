#define G_IS_NEW_TEX 0

RWTexture1D<uint> gSTreeMetaData;
RWTexture2D<uint4> gSTreeData;
RWTexture2D<float> gSTreeStatWeight;

cbuffer BlitBuf
{
    uint2 gNewTexSize;
};

[numthreads(8, 4, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (any(DTid.xy >= gNewTexSize))
        return;
    gSTreeData[DTid.xy] = uint4(0);
    gSTreeStatWeight[DTid.xy] = 0.f;
    if (all(DTid.xy == uint2(0, 0)))
    {
        gSTreeMetaData[0] = 1;
        gSTreeMetaData[1] = 1;
    }
}
