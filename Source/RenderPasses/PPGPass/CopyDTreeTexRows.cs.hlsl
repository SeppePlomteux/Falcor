
RWTexture2D<float4> gDTreeSums;
RWTexture2D<uint2> gDTreeChildren;
RWTexture2D<uint> gDTreeParent;

RWTexture1D<uint> gDTreeSize;

cbuffer CopyBuf
{
    uint gSourceRow;
    uint gDestRow;
    uint gTexWidth;
};

[numthreads(32, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (DTid.x >= gTexWidth)
        return;
    gDTreeSums[uint2(DTid.x, gDestRow)] = gDTreeSums[uint2(DTid.x, gSourceRow)];
    gDTreeChildren[uint2(DTid.x, gDestRow)] = gDTreeChildren[uint2(DTid.x, gSourceRow)];
    if (DTid.x % 2 == 0) // Only one in two threads does this assignment
    {
        gDTreeParent[uint2(DTid.x / 2, gDestRow)] = gDTreeParent[uint2(DTid.x / 2, gSourceRow)];
    }
    if (DTid.x == 0) // only one thread does this assignment
        gDTreeSize[gDestRow] = gDTreeSize[gSourceRow];
}
