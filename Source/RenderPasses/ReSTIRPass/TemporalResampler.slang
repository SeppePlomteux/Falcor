import Utils.Math.MathHelpers;
import RenderPasses.Reservoir;
import Utils.Sampling.SampleGenerator;
import Experimental.Scene.Material.MaterialShading;

// Inputs of current frame
Texture2D<float4> shPackedInputReservoirData1;
Texture2D<float4> shPackedInputReservoirData2;
Texture2D<uint> shPackedInputReservoirData3;
Texture2D<float2> shMotionVectors;

// Temporally accumulated inputs
Texture2D<float4> shAccumulatedReservoirData1;
Texture2D<float4> shAccumulatedReservoirData2;
Texture2D<uint>   shAccumulatedReservoirData3;

// Outputs
RWTexture2D<float4> shPackedOutputReservoirData1;
RWTexture2D<float4> shPackedOutputReservoirData2;
RWTexture2D<uint>   shPackedOutputReservoirData3;

cbuffer TemporalBuffer
{
    uint gFrameCount;
};

[numthreads(8, 4, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    uint2 pixelPosition = DTid.xy;
    uint2 dimensions;
    shPackedInputReservoirData1.GetDimensions(dimensions.x, dimensions.y);
    // Build current reservoir
    Reservoir r = createReservoir(  shPackedInputReservoirData1[pixelPosition],
                                    shPackedInputReservoirData2[pixelPosition],
                                    shPackedInputReservoirData3[pixelPosition]);
    
    // compute projeced pixel
    int2 toSample = (int2) round((float2) pixelPosition + dimensions*shMotionVectors[pixelPosition]);
   
    if (any(toSample < int2(0, 0)) || any(toSample >= dimensions))
    {
        //shPackedOutputReservoirData1[pixelPosition] = float4(0, 0, 0, 0);
        //shPackedOutputReservoirData2[pixelPosition] = float4(0, 0, 0, 0);
        //shPackedOutputReservoirData3[pixelPosition] = 0;
        shPackedOutputReservoirData1[pixelPosition] = shPackedInputReservoirData1[pixelPosition];
        //shPackedOutputReservoirData1[pixelPosition] = float4(10, 10, 10, 1);
        shPackedOutputReservoirData2[pixelPosition] = shPackedInputReservoirData2[pixelPosition];
        shPackedOutputReservoirData3[pixelPosition] = shPackedInputReservoirData3[pixelPosition];
        return; // Hier ontstaat wel dikke divergence, ma op zich kunnen de divergent threads niksen
    }
    // Build corresponding reservoir
    Reservoir other = createReservoir(  shAccumulatedReservoirData1[toSample],
                                        shAccumulatedReservoirData2[toSample],
                                        shAccumulatedReservoirData3[toSample]);
    // Adjust RadianceWeight to remain unbiased
    if (r.pdf == -1)
    {
        float factor = other.mTotalWeight / (r.mTotalWeight + other.mTotalWeight);
        factor = isfinite(factor) ? factor : 1;
        other.mIncommingRadiance *= factor;
        r.mTotalWeight = 0;
    }
    // Combine both reservoirs, include temporal weighting here
    SampleGenerator sg = SampleGenerator.create(pixelPosition, gFrameCount);
    float factor = min(((float) r.mTotalSampleCount) * 20 / ((float) other.mTotalSampleCount), 1);
    factor = isfinite(factor) && factor != 0 ? factor : 1;
    other.mTotalWeight *= factor;
    other.mTotalSampleCount = other.mTotalSampleCount*factor;
    combineReservoirs(r, other, sampleNext1D(sg));
    // Export resulting reservoir to output
    float4 data1;
    float4 data2;
    uint data3;
    //if (gFrameCount < 10u)
        extractPackedData(r, data1, data2, data3);
    //else
    //    extractPackedData(other, data1, data2, data3);
    shPackedOutputReservoirData1[pixelPosition] = data1;
    shPackedOutputReservoirData2[pixelPosition] = data2;
    shPackedOutputReservoirData3[pixelPosition] = data3;
}
