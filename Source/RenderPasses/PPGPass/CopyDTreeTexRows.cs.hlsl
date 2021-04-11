
Texture1D<uint> gSTreeMetaData;

RWTexture2D<float4> gDTreeSums;
RWTexture2D<uint2> gDTreeChildren;
RWTexture2D<uint> gDTreeParent;

RWTexture1D<uint> gDTreeSize;

// 0 contains DTree copy info
// 1 contains deleted STree nodes
// 2 contains deleted DTrees
RWTexture1D<uint2> gDTreeEditData;

uint updateDTreeIndex(uint oldIndex)
{
    uint2 deletedDTrees = gDTreeEditData[2];
    deletedDTrees = oldIndex > deletedDTrees ? 1 : 0;
    return oldIndex - (deletedDTrees.x + deletedDTrees.y);
}

uint getCopySource()
{
    return gDTreeEditData[0].x;
}

uint getCopyDest()
{
    return gDTreeEditData[0].y;
}

[numthreads(32, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint2 texSize;
    gDTreeSums.GetDimensions(texSize.x, texSize.y);

    if (any(DTid.xy >= texSize))
        return;

    if (DTid.y >= gSTreeMetaData[1] + 2)
        return;

    float4 oldSums = gDTreeSums[DTid.xy];
    uint2 oldChildren = gDTreeChildren[DTid.xy];
    uint oldParent;
    uint oldSize;
    if (DTid.x % 2 == 0)
        oldParent = gDTreeParent[uint2(DTid.x / 2.f, DTid.y)];
    if (DTid.x == 0)
        oldSize = gDTreeSize[DTid.y];

    DeviceMemoryBarrier();

    uint2 newIndex = uint2(DTid.x, updateDTreeIndex(DTid.y));
    uint copySource = getCopySource();
    uint copyDest = updateDTreeIndex(getCopyDest());

    if (newIndex.y == copyDest)
        return;

    gDTreeSums[newIndex] = oldSums;
    gDTreeChildren[newIndex] = oldChildren;
    if (DTid.x % 2 == 0)
        gDTreeParent[uint2(newIndex.x / 2.f, newIndex.y)] = oldParent;
    if (DTid.x == 0)
        gDTreeSize[newIndex.y] == oldSize;

    if (DTid.y == copySource)
    {
        newIndex = uint2(DTid.x, copyDest);
        gDTreeSums[newIndex] = oldSums;
        gDTreeChildren[newIndex] = oldChildren;
        if (DTid.x % 2 == 0)
            gDTreeParent[uint2(newIndex.x / 2.f, newIndex.y)] = oldParent;
        if (DTid.x == 0)
            gDTreeSize[newIndex.y] == oldSize;
    }
}
