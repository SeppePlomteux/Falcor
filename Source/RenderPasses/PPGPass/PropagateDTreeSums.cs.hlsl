#define G_16_BITMASK_LEFT  0xFFFF0000
#define G_16_BITMASK_RIGHT 0x0000FFFF

RWTexture2D<float4> gDTreeSums;
Texture2D<uint2> gDTreeChildren;
Texture2D<uint> gDTreeParent;

Texture1D<uint> gDTreeSize;

cbuffer PropagateBuf
{
    uint2 gRelevantTexSize;
};

struct DTreeNode
{
    // if mSums == (0, 0, 0, 0), then node is empty
    float4 mSums;
    uint2 mChildren;
    uint2 mTexelPos;
    uint mParent;

    // Encode a leaf as a node with child indices 0 (which is the root)
    bool isLeaf()
    {
        return all(mChildren == 0);
    }

    uint getChildIndex(uint localIndex)
    {
        uint packedData = localIndex <= 1 ? mChildren.x : mChildren.y;
        return localIndex & 1 ? packedData & G_16_BITMASK_RIGHT : packedData >> 16;
    }

    void setSum(uint absoluteChildIndex, float newVal)
    {
        uint relIndex = 0;
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
        //newVal[localChildIndex] = toAdd;
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

    res.mParent = res.mTexelPos.x % 2 ? packedParentData & G_16_BITMASK_RIGHT : packedParentData >> 16;
    
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
    if (any(DTid.xy >= gRelevantTexSize))
        return;

    if (DTid.x >= gDTreeSize[DTid.y])
        return;
    
    DTreeNode node = buildDTreeNode(DTid.y, DTid.x);
    if (node.isLeaf())
        return;

    float nodeSum = node.mSums.x + node.mSums.y + node.mSums.z + node.mSums.w;
    uint oldNodeIndex = DTid.x, currNodeIndex = DTid.x;
    bool keepGoing = true;

    while (keepGoing)
    {
        currNodeIndex = node.mParent;
        node = buildDTreeNode(DTid.y, node.mParent);
        node.setSum(oldNodeIndex, nodeSum);
        if (!nodeAllowedToContinue(node, oldNodeIndex))
            return;
        node.refreshSums();
        nodeSum = node.mSums.x + node.mSums.y + node.mSums.z + node.mSums.w;
        oldNodeIndex = currNodeIndex;
        if (currNodeIndex == 0) // We got to the root!
            keepGoing = false;
    }
    
}
