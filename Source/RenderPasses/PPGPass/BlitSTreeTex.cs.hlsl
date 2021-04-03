#define G_IS_NEW_TEX 0

RWTexture2D<uint4> gSTreeData;

Texture2D<uint4> gOldSTreeData;

cbuffer BlitBuf
{
    uint2 gOldRelevantTexSize;
    uint2 gNewTexSize;
};

[numthreads(8, 4, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (any(DTid.xy >= gNewTexSize))
        return;
    if (all(DTid.xy < gOldRelevantTexSize))
    {
        gSTreeData[DTid.xy] = gOldSTreeData[DTid.xy];
    }
    else
    {
        gSTreeData[DTid.xy] = uint4(0);
    }
}
