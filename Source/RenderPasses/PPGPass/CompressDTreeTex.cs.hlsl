#define G_16_BITMASK_LEFT  0xFFFF0000
#define G_16_BITMASK_RIGHT 0x0000FFFF

Texture1D<uint> gFreedNodes;

RWTexture2D<float4> gDTreeSums;
RWTexture2D<uint2> gDTreeChildren;
RWTexture2D<uint> gDTreeParent;

RWTexture1D<uint> gDTreeSize;

Texture1D<uint> gSTreeMetaData;

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

uint getParent(const uint treeIndex, const uint nodeIndex)
{
    uint2 parentTexelPos = uint2(nodeIndex / 2, treeIndex);
    uint packetParents = gDTreeParent[parentTexelPos];
    return (nodeIndex % 2 == 0) ? (packetParents >> 16) : (packetParents & G_16_BITMASK_RIGHT);
}

void storeParent(const uint treeIndex, const uint nodeIndex, const uint newParent)
{
    uint2 parentTexelPos = uint2(nodeIndex / 2, treeIndex);
    uint packetParents = gDTreeParent[parentTexelPos];
    if (nodeIndex % 2 == 0)
    {
        packetParents = (newParent << 16) + (packetParents & G_16_BITMASK_RIGHT);
    }
    else
    {
        packetParents = (packetParents & G_16_BITMASK_LEFT) + (newParent & G_16_BITMASK_RIGHT);
    }
    gDTreeParent[parentTexelPos] = packetParents;
}

uint updateNodeIndex(const uint oldIndex, const uint freedNode)
{
    return (0 < freedNode && freedNode < oldIndex) ? oldIndex - 1 : oldIndex;
}

uint2 updateNodeIndex(const uint2 oldIndex, const uint freedNode)
{
    return (0 < freedNode && freedNode < oldIndex) ? oldIndex - 1 : oldIndex;
}

uint4 updateNodeIndex(const uint4 oldIndex, const uint freedNode)
{
    return (0 < freedNode && freedNode < oldIndex) ? oldIndex - 1 : oldIndex;
}

[numthreads(32, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (DTid.z > 0)
        return;
    if (DTid.y >= gSTreeMetaData[1])
        return;
    uint size = gDTreeSize[DTid.y];
    if (DTid.x >= size)
        return;

    uint freedNode = gFreedNodes[DTid.y];
    
    if (freedNode == 0)
        return;

    if (DTid.x == freedNode)
        return;
    
    float4 oldSums = gDTreeSums[DTid.xy];
    uint4 oldChildren = getChildren(DTid.xy);
    oldChildren = updateNodeIndex(oldChildren, freedNode);
    uint oldParent = getParent(DTid.y, DTid.x);
    oldParent = updateNodeIndex(oldParent, freedNode);
    
    DeviceMemoryBarrier();
    
    uint2 newIndex = uint2(updateNodeIndex(DTid.x, freedNode), DTid.y);
    gDTreeSums[newIndex] = oldSums;
    storeChildren(oldChildren, newIndex);

    if (newIndex.x % 2 == 0)
    {
        storeParent(DTid.y, newIndex.x, oldParent);
    }
    DeviceMemoryBarrier();
    if (newIndex.x % 2 != 0)
    {
        storeParent(DTid.y, newIndex.x, oldParent);
    }
    
    DeviceMemoryBarrier();

    if (DTid.x == size - 1)
    {
        gDTreeChildren[DTid.xy] = uint2(0);
        gDTreeSums[DTid.xy] = float4(0.f);
        storeParent(DTid.y, DTid.x, 0);
        gDTreeSize[DTid.y] -= 1;
    }
}
