
RWTexture1D<uint> gDTreeStatisticalWeight;

cbuffer ResetBuf
{
    uint gTexSize;
};

[numthreads(32, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (DTid.x >= gTexSize)
        return;
    gDTreeStatisticalWeight[DTid.x] = 0U;
}
