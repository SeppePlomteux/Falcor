#define G_16_BITMASK_LEFT  0xFFFF0000
#define G_16_BITMASK_RIGHT 0x0000FFFF

Texture1D<uint> gFreedNodes;

RWTexture2D<float4> gDTreeSums;
RWTexture2D<uint2> gDTreeChildren;
RWTexture2D<uint> gDTreeParent;

RWTexture1D<uint> gDTreeSize;

cbuffer CompressBuf
{
    uint2 gRelevantTexSize;
};

uint4 getChildren(uint2 texelPos)
{
    uint2 packedChildren = gDTreeChildren[texelPos];
    uint4 res;
    res.x = packedChildren.x >> 16;
    res.y = packedChildren.x & G_16_BITMASK_RIGHT;
    res.z = packedChildren.y >> 16;
    res.w = packedChildren.y & G_16_BITMASK_RIGHT;
    return res;
}

void storeChildren(uint4 children, uint2 texelPos)
{
    uint2 packedChildren;
    packedChildren.x = (children.x << 16) + (children.y & G_16_BITMASK_RIGHT);
    packedChildren.y = (children.z << 16) + (children.w & G_16_BITMASK_RIGHT);
    gDTreeChildren[texelPos] = packedChildren;
}

uint2 getParents(uint2 texelPos)
{
    uint packedParents = gDTreeParent[texelPos];
    uint2 res;
    res.x = packedParents >> 16;
    res.y = packedParents & G_16_BITMASK_RIGHT;
    return res;
}

void storeParents(uint2 parents, uint2 texelPos)
{
    uint packedParents;
    packedParents = (parents.x << 16) + (parents.y & G_16_BITMASK_RIGHT);
    gDTreeParent[texelPos] = packedParents;
}

[numthreads(32, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (any(DTid.xy >= gRelevantTexSize))
        return;

    uint freedNodes = gFreedNodes[DTid.y];
    bool shouldNotShuffle = DTid.x <= freedNodes || freedNodes == 0;

    uint4 children = getChildren(DTid.xy);
    children = children == freedNodes ? 0 : children;
    children = children > freedNodes ? children - 1 : children;
    storeChildren(children, DTid.xy);

    [branch]
    if (DTid.x % 2 == 0)
    {
        uint2 parentTexelPos = DTid.xy;
        parentTexelPos.x /= 2;
        uint2 parents = getParents(parentTexelPos);
        parents = parents > freedNodes ? parents - 1 : parents;
        storeParents(parents, parentTexelPos);
    }
    
    if (all(shouldNotShuffle))
        return;
    if (any(DTid.x == freedNodes))
        return;

    // ALS FREEDNODE ONEVEN IS, MOET EERSTVOLGENDE THREAD OPPASSEN
    
    float4 sumToCopy = gDTreeSums[DTid.xy];
    uint2 childrenCopy = gDTreeChildren[DTid.xy];
    uint parentCopy;
    if (DTid.x % 2 == 0)
    {
        uint2 parentIndex = DTid.xy;
        parentIndex.x /= 2;
        parentCopy = (gDTreeParent[parentIndex - uint2(1, 0)] & G_16_BITMASK_RIGHT) << 16;
        if (DTid.x = freedNodes + 1)
            parentCopy = gDTreeParent[parentIndex - uint2(1, 0)] & G_16_BITMASK_LEFT;
        parentCopy += (gDTreeParent[parentIndex] & G_16_BITMASK_LEFT) >> 16;
    }
    
    DeviceMemoryBarrier();
    
    uint2 prevIndex = DTid.xy - uint2(1, 0);
    gDTreeSums[prevIndex] = sumToCopy;
    gDTreeChildren[prevIndex] = childrenCopy;
    if (DTid.x == 0)
        gDTreeSize[DTid.y] -= 1;
    if (DTid.x % 2 == 0)
    {
        uint2 parentIndex = DTid.xy;
        parentIndex.x /= 2;
        parentIndex.x--;
        gDTreeParent[parentIndex] = parentCopy;
    }

}
