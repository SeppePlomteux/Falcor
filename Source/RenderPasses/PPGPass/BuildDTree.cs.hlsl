#define G_MAX_DTREE_SIZE 600
#define G_MAX_DTREE_DEPTH 5

#define G_MERGE_FACTOR 0.026666666f
#define G_SPLIT_FACTOR 0.038095

#define G_16_BITMASK_LEFT  0xFFFF0000
#define G_16_BITMASK_RIGHT 0x0000FFFF

import Utils.Sampling.SampleGenerator;

RWTexture2D<float4> gDTreeSums;
RWTexture2D<uint2> gDTreeChildren;
RWTexture2D<uint> gDTreeParent;
RWTexture1D<uint> gDTreeSize;

RWTexture1D<uint> gFreedNodes;

cbuffer BuildBuf
{
    uint gAmountOfDTrees;
    uint gFrameIndex;
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

    uint4 getChildIndeces()
    {
        uint4 res;
        res.x = mChildren.x >> 16;
        res.y = mChildren.x & G_16_BITMASK_RIGHT;
        res.z = mChildren.y >> 16;
        res.w = mChildren.y & G_16_BITMASK_RIGHT;
        return res;
    }
    
    uint getChildIndex(uint localIndex)
    {
        uint packedData = localIndex <= 1 ? mChildren.x : mChildren.y;
        return localIndex & 1 ? packedData & G_16_BITMASK_RIGHT : packedData >> 16;
    }

    [mutating]void setChildIndex(uint localIndex, uint newAbsoluteIndex)
    {
        uint packedDataIndex = localIndex <= 1 ? 0 : 1;
        uint packedData = mChildren[packedDataIndex];
        if (localIndex & 1)
        {
            packedData = (packedData & G_16_BITMASK_LEFT) + (newAbsoluteIndex & G_16_BITMASK_RIGHT);
        }
        else
        {
            packedData = newAbsoluteIndex << 16 + (packedData & G_16_BITMASK_RIGHT);
        }
        mChildren[packedDataIndex] = packedData;
    }

    [mutating]void refreshSums()
    {
        mSums = gDTreeSums[mTexelPos];
    }

    void writeToTextures()
    {
        gDTreeSums[mTexelPos] = mSums;
        gDTreeChildren[mTexelPos] = mChildren;
        uint2 parentPos = mTexelPos;
        parentPos.x /= 2;
        uint oldVal = gDTreeParent[parentPos], newVal;
        if (mTexelPos.x % 2)
            newVal = (oldVal & G_16_BITMASK_LEFT) + (mParent & G_16_BITMASK_RIGHT);
        else
            newVal = mParent << 16 + (oldVal & G_16_BITMASK_RIGHT);
        gDTreeParent[parentPos] = newVal;
    }

    void writeChildDataToTextures()
    {
        gDTreeChildren[mTexelPos] = mChildren;
    }

    uint getTreeIndex()
    {
        return mTexelPos.y;
    }
    
    [mutating]void setTreeIndex(uint newIndex)
    {
        mTexelPos.y = newIndex;
    }

    uint getNodeIndex()
    {
        return mTexelPos.x;
    }

    [mutating]void setNodeIndex(uint newIndex)
    {
        mTexelPos.x = newIndex;
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

uint removeDTreeCell(DTreeNode rootNode, SampleGenerator sg)
{
    if (gDTreeSize[rootNode.getTreeIndex()] <= 1)
        return 0;
    float totalSum = rootNode.mSums.x + rootNode.mSums.y + rootNode.mSums.z + rootNode.mSums.w;
    DTreeNode childNode;
    while (true)
    {
        float4 factors = rootNode.mSums;
        factors = rootNode.getChildIndeces() == 0 ? 0 : factors;
        factors = factors == 0.f ? 0 : 1 / factors;
        factors.y = factors.x + factors.y;
        factors.z = factors.y + factors.z;
        factors.w = factors.z + factors.w;

        if (factors.w == 0.f) // happens if rootNode is a leaf
            return 0;

        float rand = sampleNext1D(sg) * factors.w; // Change to sample1D(sg)
        
        uint4 indexHelper = factors <= rand ? 1u : 0u;
        uint relIndex = indexHelper.x + indexHelper.y + indexHelper.z + indexHelper.w;
        uint absIndex = rootNode.getChildIndex(relIndex);

        childNode = buildDTreeNode(rootNode.getTreeIndex(), absIndex);

        if (childNode.isLeaf() && rootNode.mSums[relIndex] <= totalSum * G_MERGE_FACTOR)
        {
            rootNode.setChildIndex(relIndex, 0);
            rootNode.writeChildDataToTextures();
            return absIndex;
        }

        rootNode = childNode;
    }
}

// Returns true if a new node was added
bool splitDTreeNode(DTreeNode node, uint relativeIndex)
{
    if (node.getChildIndex(relativeIndex) != 0)
        return false;
    if (gDTreeSize[node.getTreeIndex()] >= G_MAX_DTREE_SIZE)
        return false;
    
    DTreeNode newNode = { };
    float newSum = node.mSums[relativeIndex] / 4;
    newNode.mParent = node.getNodeIndex();
    newNode.mSums = float4(newSum);
    newNode.mChildren = uint2(0);
    newNode.setTreeIndex(node.getTreeIndex());
    uint newNodeIndex = gDTreeSize[node.getTreeIndex()];
    newNode.setNodeIndex(newNodeIndex);
    gDTreeSize[node.getTreeIndex()]++;

    newNode.writeToTextures();

    node.setChildIndex(relativeIndex, newNodeIndex);
    node.writeChildDataToTextures();
    
    return true;
}

void addDTreeCell(DTreeNode rootNode, SampleGenerator sg)
{
    if (gDTreeSize[rootNode.getTreeIndex()] >= G_MAX_DTREE_SIZE)
        return;
    float totalSum = rootNode.mSums.x + rootNode.mSums.y + rootNode.mSums.z + rootNode.mSums.w;
    for (int depth = 1; depth < G_MAX_DTREE_DEPTH - 1; depth++)
    {
        float4 factors = rootNode.mSums;
        factors.y = factors.x + factors.y;
        factors.z = factors.y + factors.z;
        factors.w = factors.z + factors.w;

        if (factors.w == 0.f) // happens if rootNode is a leaf
            return;

        float rand = sampleNext1D(sg) * factors.w; // Change to sample1D(sg)
        
        uint4 indexHelper = factors <= rand ? 1u : 0u;
        uint relIndex = indexHelper.x + indexHelper.y + indexHelper.z + indexHelper.w;
        uint absIndex = rootNode.getChildIndex(relIndex);

        if (absIndex == 0 && rootNode.mSums[relIndex] >= totalSum * G_SPLIT_FACTOR)
        {
            splitDTreeNode(rootNode, relIndex);
            return;
        }
        rootNode = buildDTreeNode(rootNode.getTreeIndex(), absIndex);
        if (absIndex == 0)
            return;
    }
}

[numthreads(32, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (DTid.x >= gAmountOfDTrees)
        return;

    SampleGenerator sg = SampleGenerator.create(DTid.xy, gFrameIndex);
    for (int i = 0; i < 2; i++)
        sampleNext3D(sg);
    
    DTreeNode root = buildDTreeNode(DTid.x, 0);

    gFreedNodes[DTid.x] = removeDTreeCell(root, sg);

    addDTreeCell(root, sg);
}
