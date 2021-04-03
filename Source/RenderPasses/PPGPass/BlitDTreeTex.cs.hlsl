//#define G_IS_NEW_TEX 0

Texture2D<float4> gOldDTreeSums;
Texture2D<uint2> gOldDTreeChildren;
Texture2D<uint> gOldDTreeParent;

Texture1D<uint> gOldDTreeSize;

RWTexture2D<float4> gDTreeSums;
RWTexture2D<uint2> gDTreeChildren;
RWTexture2D<uint> gDTreeParent;

RWTexture1D<uint> gDTreeSize;

cbuffer BlitBuf
{
    uint2 gOldRelevantTexSize;
    uint2 gNewTexSize;
};

[numthreads(8, 4, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (G_IS_NEW_TEX)
    {
        if (any(DTid.xy >= gNewTexSize))
            return;
        gDTreeSums[DTid.xy] = float4(0.f);
        gDTreeChildren[DTid.xy] = uint2(0);
        if (DTid.x == 0)
            gDTreeSize[DTid.y] = DTid.y == 0 ? 1 : 0;
        if (DTid.x % 2 == 0)
        {
            uint2 parentIndex = DTid.xy;
            parentIndex.x /= 2;
            gDTreeParent[parentIndex] = 0;
        }
    }
    else
    {
        if (any(DTid.xy >= gNewTexSize))
            return;
        if (all(DTid.xy < gOldRelevantTexSize))
        {
            gDTreeSums[DTid.xy] = gOldDTreeSums[DTid.xy];

            gDTreeChildren[DTid.xy] = gOldDTreeChildren[DTid.xy];
            [branch]
            if (DTid.x == 0)
                gDTreeSize[DTid.y] = gOldDTreeSize[DTid.y];
            if (DTid.x % 2 == 0)
            {
                uint2 parentIndex = DTid.xy;
                parentIndex.x /= 2;
                gDTreeParent[parentIndex] = gOldDTreeParent[parentIndex];
            }
        }
        else
        {
            gDTreeSums[DTid.xy] = float4(0.f);
            gDTreeChildren[DTid.xy] = uint2(0);
            [branch]
            if (DTid.x == 0)
                gDTreeSize[DTid.y] = 0;
            if (DTid.x % 2 == 0)
            {
                uint2 parentIndex = DTid.xy;
                parentIndex.x /= 2;
                gDTreeParent[parentIndex] = 0;
            }
        }
    }
}
