struct Reservoir
{
    float3 mIncommingRadiance;
    float mTotalWeight;
    /* ---- block ends here ---- */
    float3 mSamplePoint;
    float pdf;
    /* ---- block ends here ---- */
    uint mTotalSampleCount;
};

Reservoir createReservoir()
{
    Reservoir result;
    result.mIncommingRadiance = float3(0);
    result.mTotalWeight = 0;

    result.mSamplePoint = float3(0);
    result.pdf = 0;
    
    result.mTotalSampleCount = 0;
    return result;
}

/**
    packedData1 = incomming radiance and total weight
    packedData2 = sample point and total sample count
*/
Reservoir createReservoir(const float4 packedData1, const float4 packedData2, const uint packedData3)
{
    Reservoir result;
    result.mIncommingRadiance = packedData1.xyz;
    result.mTotalWeight = packedData1.w;

    result.mSamplePoint = packedData2.xyz;
    result.pdf = packedData2.w;

    result.mTotalSampleCount = packedData3;
    return result;
}

/**
    packedData1 = incomming radiance and total weight
    packedData2 = sample point and total sample count
*/
void extractPackedData(const Reservoir r, out float4 packedData1, out float4 packedData2, out uint packedData3)
{
    packedData1.xyz = r.mIncommingRadiance;
    packedData1.w = r.mTotalWeight;

    packedData2.xyz = r.mSamplePoint;
    packedData2.w = r.pdf;

    packedData3 = r.mTotalSampleCount;
}


void updateReservoir(inout Reservoir r, const float3 newSamplePoint, const float3 newIncommingRadiance, const float weight, const float pdf, const float rand)
{
    r.mTotalSampleCount++;
    r.mTotalWeight += weight;
    
    /*if (rand < weight / r.mTotalWeight)
    {
        r.mSamplePoint = newSamplePoint;
        r.mIncommingRadiance = newIncommingRadiance;
        r.pdf = pdf;
    }*/

    // ALTERNATIVE
    
    bool change = rand < weight / r.mTotalWeight;
    bool noChange = !change;
    r.mSamplePoint = change * newSamplePoint + noChange * r.mSamplePoint;
    r.mIncommingRadiance = change * newIncommingRadiance + noChange * r.mIncommingRadiance;
    r.pdf = change * pdf + noChange * r.pdf;
}

void combineReservoirs(inout Reservoir rDest, const Reservoir rSource, const float rand)
{
    rDest.mTotalSampleCount += rSource.mTotalSampleCount;
    rDest.mTotalWeight += rSource.mTotalWeight;

    if (rand < rSource.mTotalWeight / rDest.mTotalWeight)
    {
        rDest.mSamplePoint = rSource.mSamplePoint;
        rDest.mIncommingRadiance = rSource.mIncommingRadiance;
        rDest.pdf = rSource.pdf;
    }
}
