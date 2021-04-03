
RWTexture1D<uint> gDTreeStatisticalWeight;

cbuffer ResetBuf
{
    uint gAmountOfDTrees;
};

[numthreads(32, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (DTid.x >= gAmountOfDTrees)
        return;
    gDTreeStatisticalWeight[DTid.x] = 0U;
}
