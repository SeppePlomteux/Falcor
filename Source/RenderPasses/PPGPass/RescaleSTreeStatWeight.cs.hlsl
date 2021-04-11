#define K_SCALE_FACTOR 0.9f

Texture1D<uint> gSTreeMetaData;
RWTexture2D<float> gSTreeStatWeight;

uint toLinearIndex(uint2 index, uint2 texSize)
{
    return index.y * texSize.x + index.x;
}

uint2 toTexCoords(uint lineairIndex, uint2 texSize)
{
    return uint2(lineairIndex % texSize.x, lineairIndex / texSize.x);
}

[numthreads(8, 4, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint2 texSize;
    gSTreeStatWeight.GetDimensions(texSize.x, texSize.y);
    if (any(DTid.xy >= texSize))
        return;
    uint amountOfSTreeNodes = gSTreeMetaData[0];
    uint lineairIndex = toLinearIndex(DTid.xy, texSize);

    if (lineairIndex > amountOfSTreeNodes)
        return;

    gSTreeStatWeight[DTid.xy] *= K_SCALE_FACTOR;
}
