#define G_16_BITMASK_LEFT  0xFFFF0000
#define G_16_BITMASK_RIGHT 0x0000FFFF

RWTexture2D<float4> gDTreeSums;
Texture2D<uint2> gDTreeChildren;
Texture2D<uint> gDTreeParent;

Texture1D<uint> gDTreeSize;
Texture1D<uint> gSTreeMetaData;

struct DTreeNode
{
    float4 mSums;
    uint2 mChildren;
    uint2 mTexelPos;
    uint mParent;

    uint getChildIndex(uint localIndex)
    {
        uint packedData = localIndex <= 1 ? mChildren.x : mChildren.y;
        return localIndex & 1 ? packedData & G_16_BITMASK_RIGHT : packedData >> 16;
    }

    // Encode a leaf as a node with index 0
    bool isLeaf(uint relativeIndex)
    {
        return getChildIndex(relativeIndex) == 0;
    }

    bool allChildsAreLeaf()
    {
        return all(mChildren == 0);
    }
    
    void setSum(uint absoluteChildIndex, float newVal)
    {
        for (int i = 0; i < 4; i++)
        {
            [branch]
            if (absoluteChildIndex == getChildIndex(i))
            {
                float4 toChange = gDTreeSums[mTexelPos];
                toChange[i] = newVal;
                gDTreeSums[mTexelPos] = toChange;
            }
            DeviceMemoryBarrier();
        }
    }

    [mutating]void refreshSums()
    {
        mSums = gDTreeSums[mTexelPos];
    }
    
    void addToSum(uint localChildIndex, float toAdd)
    {
        float4 newVal = float4(0.f);
        
        newVal[localChildIndex] = toAdd; // ALS DEZE SHADER NI WERKT, KIJK HIER NAAR

        gDTreeSums[mTexelPos] += newVal;
    }
};

DTreeNode buildDTreeNode(uint treeIndex, uint nodeIndex)
{
    DTreeNode res = { };
    res.mTexelPos = uint2(nodeIndex, treeIndex);
    res.mSums = gDTreeSums[res.mTexelPos];
    res.mChildren = gDTreeChildren[res.mTexelPos];
    uint2 parentTexelPos = res.mTexelPos;
    parentTexelPos.x /= 2;
    uint packedParentData = gDTreeParent[parentTexelPos];

    res.mParent = (res.mTexelPos.x % 2 == 1) ? (packedParentData & G_16_BITMASK_RIGHT) : (packedParentData >> 16);
    
    return res;
}

bool nodeAllowedToContinue(DTreeNode node, uint oldNodeIndex)
{
    bool res = true;
    for (int i = 0; i < 4; i++)
    {
        res &= node.getChildIndex(i) <= oldNodeIndex;
    }
    return res;
}

[numthreads(32, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (DTid.y >= gSTreeMetaData[1])
        return;
    
    if (DTid.x >= gDTreeSize[DTid.y])
        return;
    
    DTreeNode node = buildDTreeNode(DTid.y, DTid.x);
    if (!node.allChildsAreLeaf())
        return;

    float nodeSum = node.mSums.x + node.mSums.y + node.mSums.z + node.mSums.w;
    uint oldNodeIndex = DTid.x, currNodeIndex = DTid.x;
    bool keepGoing = currNodeIndex != 0;

    while (keepGoing)
    {
        currNodeIndex = node.mParent;
        node = buildDTreeNode(DTid.y, node.mParent);
        node.setSum(oldNodeIndex, nodeSum);
        if (!nodeAllowedToContinue(node, oldNodeIndex))
            return;
        node.refreshSums();
        nodeSum = node.mSums.x + node.mSums.y + node.mSums.z + node.mSums.w;
        
        //oldNodeIndex = currNodeIndex;
        if (currNodeIndex == 0 /*|| currNodeIndex >= oldNodeIndex*/) // We got to the root!
            keepGoing = false;
        oldNodeIndex = currNodeIndex;
    }
    
}
