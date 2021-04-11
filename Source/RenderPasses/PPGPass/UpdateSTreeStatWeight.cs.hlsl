#define G_2_BITMASK_RIGHT 0x00000003 // 0b0000.0000|0000.0000|0000.0000|0000.0011
#define G_30_BITMASK_LEFT 0xFFFFFFFC // 0b1111.1111|1111.1111|1111.1111|1111.1100


Texture1D<uint> gSTreeMetaData;
Texture2D<uint4> gSTreeData;
RWTexture2D<float> gSTreeStatWeight;

Texture1D<uint> gDTreeStatisticalWeight;

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
    float statWeight;
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
    res.statWeight = gSTreeStatWeight[texelPos];
    return res;
}

bool isHighestChild(STreeNode parent, uint childIndex)
{
    return all(parent.mChildren <= childIndex);
}

[numthreads(8, 4, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint2 texSize;
    gSTreeData.GetDimensions(texSize.x, texSize.y);

    if (any(DTid.xy >= texSize))
        return;

    uint amountOfSTrees = gSTreeMetaData[0];
    uint lineairIndex = toLinearIndex(DTid.xy, texSize);
    if (lineairIndex >= amountOfSTrees)
        return;
    STreeNode node = buildSTreeNode(DTid.xy, texSize);
    if (!node.isLeaf())
    {
        return;
    }
    gSTreeStatWeight[DTid.xy] += gDTreeStatisticalWeight[node.mDTreeIndex];
    if (lineairIndex == 0)
        return;
    uint parentIndex = node.getParent();
    uint2 parentTexCoords = toTexCoords(parentIndex, texSize);
    node = buildSTreeNode(parentTexCoords, texSize);
    bool keepGoing = true;
    while (keepGoing)
    {
        if (! isHighestChild(node, lineairIndex))
            break;
        DeviceMemoryBarrier();
        gSTreeStatWeight[parentTexCoords] =
            gSTreeStatWeight[toTexCoords(node.mChildren.x, texSize)] +
            gSTreeStatWeight[toTexCoords(node.mChildren.y, texSize)];
        
        if (parentIndex == 0)
            break;

        lineairIndex = parentIndex;
        parentIndex = node.getParent();
        parentTexCoords = toTexCoords(parentIndex, texSize);
        node = buildSTreeNode(parentTexCoords, texSize);
    }
}
