RWTexture2D<uint4> gSTreeData;

cbuffer CompressBuf
{
    uint2 gRelevantTexSize;
    uint gFullTexWidth;
    uint _padding0;
    /* Block border */
    uint gFirstChangedNodeIndex;
    uint gFirstChangedNodeDTreeIndex;
    uint2 gFirstChangedNodeChildren;
    /* Block border */
    uint gSecondChangedNodeIndex;
    uint gSecondChangedNodeDTreeIndex;
    uint2 gSecondChangedNodeChildren;
    /* Block border */
    uint gFirstNewNodeIndex;
    uint gSecondNewNodeIndex;
    uint2 _padding1;
    /* Block border */
    uint4 gFirstNewNodeData;
    /* Block border */
    uint4 gSecondNewNodeData;
};

[numthreads(8, 4, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (any(DTid.xy >= gRelevantTexSize))
        return;
    
    uint linearIndex = DTid.y * gFullTexWidth + DTid.x;
    if (linearIndex == gFirstChangedNodeIndex)
    {
        uint4 dataToPut = gSTreeData[DTid.xy];
        dataToPut.x = gFirstChangedNodeDTreeIndex;
        dataToPut.zw = gFirstChangedNodeChildren;
        gSTreeData[DTid.xy] = dataToPut;
    }
    else if (linearIndex == gSecondChangedNodeIndex)
    {
        uint4 dataToPut = gSTreeData[DTid.xy];
        dataToPut.x = gSecondChangedNodeDTreeIndex;
        dataToPut.zw = gSecondChangedNodeChildren;
        gSTreeData[DTid.xy] = dataToPut;
    }
    else if (linearIndex == gFirstNewNodeIndex)
    {
        gSTreeData[DTid.xy] = gFirstNewNodeData;
    }
    else if (linearIndex == gSecondNewNodeIndex)
    {
        gSTreeData[DTid.xy] = gSecondNewNodeData;
    }
}
