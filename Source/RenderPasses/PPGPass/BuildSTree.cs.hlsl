#define G_2_BITMASK_RIGHT 0x00000003 // 0b0000.0000|0000.0000|0000.0000|0000.0011
#define G_30_BITMASK_LEFT 0xFFFFFFFC // 0b1111.1111|1111.1111|1111.1111|1111.1100
#define K_MAX_DTREES 8192
//#define G_SPLIT_TRESHOLD 100000
//#define G_MERGE_TRESHOLD 90000
#define MIN_PROB 1e-20

//#define K_DO_MERGE 1

import Utils.Sampling.SampleGenerator;

RWTexture1D<uint> gSTreeMetaData;
RWTexture2D<uint4> gSTreeData;
RWTexture2D<float> gSTreeStatWeight;

RWTexture1D<uint2> gDTreeEditData;

cbuffer BuildBuf
{
    uint gFrameCount;
};

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
        return (mAxis & G_30_BITMASK_LEFT) >> 2;
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
        uint4 newVal = uint4(mDTreeIndex, mAxis, mChildren.x, mChildren.y);
        gSTreeData[mTexelPos] = newVal;
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

bool canSplit(STreeNode node)
{
    return node.isLeaf() && node.mStatWeight >= G_SPLIT_TRESHOLD;
}

bool canMerge(STreeNode node, uint2 texSize)
{
    if (!(node.mStatWeight > G_MERGE_TRESHOLD && all(node.mChildren != 0))) // we need children to merge
        return false;
    uint4 leftChild = gSTreeData[toTexCoords(node.mChildren.x, texSize)];
    uint4 rightChild = gSTreeData[toTexCoords(node.mChildren.y, texSize)];
    return all(leftChild.zw == 0) && all(rightChild.zw == 0);
}

[numthreads(1, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (any(DTid.xy >= uint2(1, 1)))
        return;
    bool doSplitting = DTid.x == 0;

    uint2 texelSize;
    gSTreeData.GetDimensions(texelSize.x, texelSize.y);
    
    STreeNode currentNode = buildSTreeNode(uint2(0, 0), texelSize);

    SampleGenerator sg = SampleGenerator.create(DTid.xy, gFrameCount);
    
    gDTreeEditData[0] = uint2(0);
    if (gSTreeMetaData[1] < K_MAX_DTREES)
    {
        while (true)
        {
            if (canSplit(currentNode))
            {
                uint newIndex;
                InterlockedAdd(gSTreeMetaData[0], 1, newIndex);
                currentNode.mChildren.x = newIndex;
                STreeNode newNode;
                newNode.setAxis((currentNode.getAxis() + 1) % 3);
                newNode.mChildren = uint2(0, 0);
                newNode.mDTreeIndex = currentNode.mDTreeIndex;
                newNode.mNodeIndex = newIndex;
                newNode.mStatWeight = currentNode.mStatWeight / 2.f;
                newNode.mTexelPos = toTexCoords(newNode.mNodeIndex, texelSize);
                newNode.setParent(currentNode.mNodeIndex);
                newNode.persist();
                InterlockedAdd(gSTreeMetaData[0], 1, newIndex);
                currentNode.mChildren.y = newIndex;
                newNode.setAxis((currentNode.getAxis() + 1) % 3);
                newNode.mChildren = uint2(0, 0);
                InterlockedAdd(gSTreeMetaData[1], 1, newNode.mDTreeIndex);
                newNode.mNodeIndex = newIndex;
                newNode.mStatWeight = currentNode.mStatWeight / 2.f;
                newNode.mTexelPos = toTexCoords(newNode.mNodeIndex, texelSize);
                newNode.setParent(currentNode.mNodeIndex);
                newNode.persist();

                gDTreeEditData[0] = uint2(currentNode.mDTreeIndex, newNode.mDTreeIndex);
                
                currentNode.mDTreeIndex = 0;
                currentNode.persist();
                return;
            }

            if (any(currentNode.mChildren == 0))
                return;
            
            float2 cumSums;
            cumSums.x = gSTreeStatWeight[toTexCoords(currentNode.mChildren.x, texelSize)];
            cumSums.y = currentNode.mStatWeight;
            
            float rand = sampleNext1D(sg) * cumSums.y;
            
            if (rand < cumSums.x)
            {
                currentNode = buildSTreeNode(toTexCoords(currentNode.mChildren.x, texelSize), texelSize);
            }
            else
            {
                currentNode = buildSTreeNode(toTexCoords(currentNode.mChildren.y, texelSize), texelSize);
            }
        }
    }
    gDTreeEditData[1] = uint2(0);
    gDTreeEditData[2] = uint2(0);
    if (K_DO_MERGE)
    {
        while (true)
        {
            if (canMerge(currentNode, texelSize))
            {
                //InterlockedAdd(gSTreeMetaData[0], -2);
                gSTreeMetaData[0] -= 2;
                //InterlockedAdd(gSTreeMetaData[1], -1);
                gSTreeMetaData[1] -= 1;
                gDTreeEditData[1] = currentNode.mChildren;
                gDTreeEditData[2] = uint2(gSTreeData[toTexCoords(currentNode.mChildren.y, texelSize)].x, 0);
                currentNode.mChildren = uint2(0);
                currentNode.mDTreeIndex = gSTreeData[toTexCoords(currentNode.mChildren.x, texelSize)].x;
                currentNode.persist();
                return;
            }
            
            if (any(currentNode.mChildren == 0))
                return;
            
            float2 cumSums;
            cumSums.x = 1.f / gSTreeStatWeight[toTexCoords(currentNode.mChildren.x, texelSize)];
            cumSums.x = isnan(cumSums.x) ? MIN_PROB : cumSums.x;
            cumSums.y = cumSums.x + 1.f / gSTreeStatWeight[toTexCoords(currentNode.mChildren.y, texelSize)];
            cumSums.y = isnan(cumSums.y) ? cumSums.x + MIN_PROB : cumSums.y;

            float rand = sampleNext1D(sg) * cumSums.y;

            if (rand < cumSums.x)
            {
                currentNode = buildSTreeNode(toTexCoords(currentNode.mChildren.x, texelSize), texelSize);
            }
            else
            {
                currentNode = buildSTreeNode(toTexCoords(currentNode.mChildren.y, texelSize), texelSize);
           
            }
        }
    }
}
