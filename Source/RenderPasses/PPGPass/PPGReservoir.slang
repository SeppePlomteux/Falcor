import Utils.Sampling.SampleGenerator;

struct Reservoir
{
    float3 mSample;
    float mWeight;
    uint mSampleCount;
    float mTargetPdf;

    float getMCCorrectingFactor()
    {
        return mWeight / (float) mSampleCount;
    }

    [mutating]void addToReservoir(float3 newSample, float targetPdf, float weight, inout SampleGenerator sg)
    {
        mWeight += weight;
        mSampleCount++;
        if (sampleNext1D(sg) * mWeight < weight)
        {
            mSample = newSample;
            mTargetPdf = targetPdf;
        }
    }
};

Reservoir createEmptyReservoir()
{
    Reservoir res = { };
    res.mSample = float3(0.f);
    res.mWeight = 0.f;
    res.mSampleCount = 0;
    res.mTargetPdf = -1.f;
    return res;
}
