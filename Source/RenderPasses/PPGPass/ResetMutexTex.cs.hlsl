
RWTexture2D<uint> gDTreeBuildingMutex;

cbuffer ResetBuf
{
    uint2 gTextureSize;
};

[numthreads(8, 4, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (any(DTid.xy >= gTextureSize))
        return;
    gDTreeBuildingMutex[DTid.xy] = 0U;
}
