#define G_2_BITMASK_RIGHT 0x00000003 // 0b0000.0000|0000.0000|0000.0000|0000.0011
#define G_30_BITMASK_LEFT 0xFFFFFFFC // 0b1111.1111|1111.1111|1111.1111|1111.1100

RWTexture1D<uint> gSTreeMetaData;
RWTexture2D<uint4> gSTreeData;
RWTexture2D<float> gSTreeStatWeight;

// 0 contains DTree copy info
// 1 contains deleted STree nodes
// 2 contains deleted DTrees
Texture1D<uint2> gDTreeEditData;

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
    /* Block border here */
    uint mNodeIndex;
    uint2 mTexelPos;
    float mStatWeight;
    /* Block border here */
    
    bool isLeaf()
    {
        return all(mChildren == 0);
    }

    uint getAxis()
    {
        return mAxis & G_2_BITMASK_RIGHT;
    }

    uint getParent()
    {
        return mAxis & G_30_BITMASK_LEFT >> 2;
    }

    [mutating]void setAxis(uint axis)
    {
        mAxis = (mAxis & G_30_BITMASK_LEFT) + (axis & G_2_BITMASK_RIGHT);
    }

    [mutating]void setParent(uint parent)
    {
        mAxis = (parent << 2) + (mAxis & G_2_BITMASK_RIGHT);
    }

    void persist()
    {
        gSTreeData[mTexelPos] = uint4(mDTreeIndex, mAxis, mChildren.x, mChildren.y);
        gSTreeStatWeight[mTexelPos] = mStatWeight;
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
    res.mNodeIndex = nodeIndex;
    res.mTexelPos = texelPos;
    res.mStatWeight = gSTreeStatWeight[texelPos];
    return res;
}

uint updateDTreeIndex(uint oldIndex)
{
    uint2 deletedDTrees = gDTreeEditData[2];
    deletedDTrees = oldIndex > deletedDTrees ? 1 : 0;
    return oldIndex - (deletedDTrees.x + deletedDTrees.y);
}

uint updateSTreeIndex(uint oldIndex)
{
    uint2 deletedSTrees = gDTreeEditData[1];
    deletedSTrees = oldIndex > deletedSTrees ? 1 : 0;
    return oldIndex - (deletedSTrees.x + deletedSTrees.y);
}

[numthreads(8, 4, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 texSize;
    gSTreeData.GetDimensions(texSize.x, texSize.y);

    if (any(DTid.xy >= texSize))
        return;

    uint amountOfSTrees = gSTreeMetaData[0] + 2;
    uint lineairIndex = toLinearIndex(DTid.xy, texSize);
    if (lineairIndex >= amountOfSTrees)
        return;

    STreeNode oldNode = buildSTreeNode(DTid.xy, texSize);
    oldNode.mChildren.x = updateSTreeIndex(oldNode.mChildren.x);
    oldNode.mChildren.y = updateSTreeIndex(oldNode.mChildren.y);
    oldNode.mDTreeIndex = updateDTreeIndex(oldNode.mDTreeIndex);
    oldNode.setParent(updateSTreeIndex(oldNode.getParent()));
    oldNode.mNodeIndex = updateSTreeIndex(oldNode.mNodeIndex);
    oldNode.mTexelPos = toTexCoords(oldNode.mNodeIndex, texSize);

    uint2 deleted = gDTreeEditData[1];
    if (any(oldNode.mNodeIndex == deleted))
        return;
    DeviceMemoryBarrier();
    oldNode.persist();
}
