
Texture1D<uint> gSTreeMetaData;
Texture2D<uint4> gSTreeData;
Texture2D<float> gSTreeStatWeight;

Texture1D<uint> gStatisticalWeight;

uint toLinearIndex(uint2 index, uint2 texSize)
{
    return index.y * texSize.x + index.x;
}

uint2 toTexCoords(uint lineairIndex, uint2 texSize)
{
    return uint2(lineairIndex % texSize.x, lineairIndex / texSize.x);
}

struct STreeNode
{
    uint mDTreeIndex;
    uint mAxis;
    uint2 mChildren;
    uint nodeIndex;
    uint2 mTexelPos;
    float statWeight;

    bool isLeaf()
    {
        return all(mChildren == 0);
    }
};

STreeNode buildSTreeNode(uint2 texelPos, uint2 texSize)
{
    STreeNode res = { };
    uint nodeIndex = toLinearIndex(texelPos, texSize);
    uint4 packedData = gSTreeData[texelPos];
    res.mDTreeIndex = packedData.x;
    res.mAxis = packedData.y;
    res.mChildren = packedData.zw;
    res.nodeIndex = nodeIndex;
    res.mTexelPos = texelPos;
    res.statWeight = gSTreeStatWeight[texelPos];
    return res;
}

[numthreads(8, 4, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint2 texSize;
    gSTreeData.GetDimensions(0, texSize.x, texSize.y);

    if (any(DTid.xy >= texSize))
        return;

    uint amountOfSTrees = gSTreeMetaData[0];
    uint lineairIndex = toLinearIndex(DTid.xy, texSize);
    if (lineairIndex >= amountOfSTrees)
        return;
    STreeNode node = buildSTreeNode(DTid.xy, texSize);
    if (node.isLeaf())
    {
        gSTreeStatWeight[DTid.xy] += gStatisticalWeight[node.mDTreeIndex];
    }

}
