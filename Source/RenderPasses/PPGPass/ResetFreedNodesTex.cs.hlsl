RWTexture1D<uint> gFreedNodes;

cbuffer ResetBuf
{
    uint gTexSize;
};

[numthreads(32, 1, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (DTid.x >= gTexSize)
        return;

    gFreedNodes[DTid.x] = 0.f;
}
