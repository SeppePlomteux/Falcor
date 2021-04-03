Texture2D<float4> shWindow;
RWTexture2D<float4> shOutput;

cbuffer buf
{
    uint4 gWindowCoords;
    uint2 gImageSize;
};

[numthreads(16, 16, 1)]
void main(uint3 pos : SV_DispatchThreadID)
{
    uint2 pixelPos = pos.xy;
    if (any(pixelPos.xy > gImageSize))
        return;
    if (all(pixelPos >= gWindowCoords.xy) && all(pixelPos <= gWindowCoords.zw))
    {
        shOutput[pixelPos] =  shWindow[pixelPos - gWindowCoords.xy];
        //shOutput[pixelPos] = float4(1.f, 1.f, 1.f, 1.f);
    }
    else
    {
        shOutput[pixelPos] = float4(0.f, 0.f, 0.f, 0.f);
    }
}
