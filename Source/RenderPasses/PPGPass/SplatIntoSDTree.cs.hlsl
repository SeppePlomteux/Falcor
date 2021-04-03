#define G_PI 3.141592653589793
#define G_16_BITMASK_LEFT  0xFFFF0000
#define G_16_BITMASK_RIGHT 0x0000FFFF

RWTexture2D<float4> gDTreeSums;
RWTexture2D<uint2> gDTreeChildren;
RWTexture1D<uint> gDTreeStatisticalWeight;
//RWTexture2D<uint> gDTreeBuildingMutex;
RWTexture2D<uint4> gSTreeData; // x: DTreeIndex, y: axis, zw: child indices

// pos (x, y, y) (relative)
Texture2D<float4> gSamplePos;
// Li  (r, g, b)
Texture2D<float4> gSampleRadiance;
// dir (u, v) and pdf
Texture2D<float4> gSamplePdf;

cbuffer SplatBuf
{
    uint2 gScreenSize;
    uint gAmountOfSTreeNodesPerRow;
    uint _padding0;
    
    float3 gSceneMin;
    uint _padding1;
    
    float3 gSceneMax;
    uint _padding2;
};

/*struct Mutex
{
    uint2 mTexelPos;
    uint mIsAquired;

    [mutating]void aquire()
    {
        if (mIsAquired)
            return;
        mIsAquired = true;
        uint original = 1;
        while (original == 1)
            InterlockedCompareExchange(gDTreeBuildingMutex[mTexelPos], 0, 1, original);
    }

    [mutating]void release()
    {
        if (!mIsAquired)
            return;
        mIsAquired = false;
        uint _original;
        InterlockedExchange(gDTreeBuildingMutex[mTexelPos], 0, _original);
    }
};*/

/*Mutex buildMutex(uint dTreeIndex, uint nodeIndex)
{
    Mutex res;
    res.mTexelPos = uint2(nodeIndex, dTreeIndex);
    res.mIsAquired = false;
}*/

struct DTreeNode
{
    // if mSums == (0, 0, 0, 0), then node is empty
    float4 mSums;
    uint2 mChildren;
    uint2 mTexelPos;

    // Encode a leaf as a node with child indices 0 (which is the root)
    bool isLeaf()
    {
        return all(mChildren == 0);
    }

    uint getChildIndex(uint localIndex)
    {
        uint packedData = localIndex <= 1 ? mChildren.x : mChildren.y;
        return localIndex & 1 ? packedData & G_16_BITMASK_RIGHT : packedData >> 16;
    }

    void addToSum(uint localChildIndex, float toAdd)
    {
        //float4 newVal = float4(0.f);
        //newVal[localChildIndex] = toAdd;

        switch (localChildIndex)
        {
            case 0:
            {
                gDTreeSums[mTexelPos].x += toAdd;
                break;
            }
            case 1:
            {
                gDTreeSums[mTexelPos].y += toAdd;
                break;
            }
            case 2:
            {
                gDTreeSums[mTexelPos].z += toAdd;
                break;
            }
            case 3:
            {
                gDTreeSums[mTexelPos].w += toAdd;
                break;
            }
        }

        //uint original;
        //do
        //{
        //    InterlockedCompareExchange(gDTreeBuildingMutex[mTexelPos], 0, 1, original);
        //} while (original == 1)

        //gDTreeSums[mTexelPos] += newVal;

        //InterlockedExchange(gDTreeBuildingMutex[mTexelPos], 0, original);
    }
};

// Decent into the DTree, calculate child index and update p (rescale to [0, 1])
uint decentIntoDTree(inout float2 p)
{
    uint2 kaas = p < float2(.5f) ? 1 : 0;
    p = kaas ? p * 2 : mad(2.f, p, -1.f);
    return mad(2, 1 - kaas.y, 1 - kaas.x);
}

DTreeNode buildDTreeNode(uint treeIndex, uint nodeIndex)
{
    DTreeNode res = { };
    res.mTexelPos = uint2(nodeIndex, treeIndex);
    //res.mSums = gDTreeSums[res.mTexelPos];
    res.mChildren = gDTreeChildren[res.mTexelPos];
    return res;
}

struct STreeNode
{
    uint mDTreeIndex;
    uint mAxis;
    uint2 mChildren;

    bool isLeaf()
    {
        return all(mChildren == 0);
    }
};

STreeNode buildSTreeNode(uint nodeIndex)
{
    STreeNode res = { };
    uint2 texIndex = uint2(nodeIndex % gAmountOfSTreeNodesPerRow, nodeIndex / gAmountOfSTreeNodesPerRow);
    uint4 packedData = gSTreeData[texIndex];
    res.mDTreeIndex = packedData.x;
    res.mAxis = packedData.y;
    res.mChildren = packedData.zw;
    return res;
}

float3 canonicalToDir(float2 p)
{
    const float cosTheta = 2 * p.x - 1;
    const float phi = 2 * G_PI * p.y;

    const float sinTheta = sqrt(1 - cosTheta * cosTheta);
    float sinPhi = sin(phi);
    float cosPhi = cos(phi);

    return float3(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
}

float2 dirToCanonical(float3 dir)
{
    if (!all(isfinite(dir)))
    {
        return float2(0.f, 0.f);
    }

    const float cosTheta = clamp(dir.z, -1.f, 1.f);
    float phi = atan2(dir.y, dir.x);
    phi = phi < 0 ? phi + 2.f * G_PI : phi;
    
    return float2((cosTheta + 1) / 2, phi / (2 * G_PI));
}

// Returns a DTree index
uint getDTreeIndex(float3 loc, float3 min, float3 size, STreeNode node)
{
    while (!node.isLeaf())
    {
        size[node.mAxis] /= 2;
        uint index = loc[node.mAxis] < min[node.mAxis] + size[node.mAxis] ? 0 : 1;
        //index = 1 - index;
        if (index == 1)
            min[node.mAxis] += size[node.mAxis];

        node = buildSTreeNode(node.mChildren[index]);
    }
    return node.mDTreeIndex;
}

void updateDTreeSums(uint dTreeIndex, float2 dir, float toAdd)
{
    DTreeNode node = buildDTreeNode(dTreeIndex, 0);
    //dir = dirToCanonical(canonicalToDir(dir));
    //Mutex mutex = buildMutex(dTreeIndex, 0);
    // dir = float2(1.f) - dir;
    bool valid = true;
    uint absoluteChildIndex = 0;
    uint relativeChildIndex = 0;
    while (valid)
    {
        relativeChildIndex = decentIntoDTree(dir);
        absoluteChildIndex = node.getChildIndex(relativeChildIndex);
        
        //uint2 texelPos = uint2(absoluteChildIndex, dTreeIndex);
        
        //mutex.aquire();
        //mutex.release();
        
        if (absoluteChildIndex == 0)
            valid = false;
        else
        {
            node = buildDTreeNode(dTreeIndex, absoluteChildIndex);
        }
    }
    node.addToSum(relativeChildIndex, toAdd);
}



[numthreads(8, 4, 1)]
void main( uint3 DTid : SV_DispatchThreadID )
{
    if (any(DTid.xy >= gScreenSize))
        return;
    uint2 screenPos = DTid.xy;
    float3 pos = gSamplePos[screenPos].xyz;
    float3 irradiance = gSampleRadiance[screenPos].xyz;
    float2 dir = gSamplePdf[screenPos].xy;
    float pdf = gSamplePdf[screenPos].z;
    
    irradiance /= pdf;

    if (pdf <= 0) // check for invalid sample
        return;

    if (any(irradiance < 0))
        return;

    if (any(!isfinite(irradiance)))
        return;
    
    uint dTreeIndex = getDTreeIndex(pos, gSceneMin, gSceneMax - gSceneMin, buildSTreeNode(0));
    //uint2 texSize;
    //gDTreeSums.GetDimensions(texSize.x, texSize.y);
    //dTreeIndex = (texSize.y - 1) - dTreeIndex;
    InterlockedAdd(gDTreeStatisticalWeight[dTreeIndex], 1U);

    if (all(irradiance == 0)) // don't stress texture unit with useless writes
        return;
    
    updateDTreeSums(dTreeIndex, dir, irradiance.x + irradiance.y + irradiance.z);
}
