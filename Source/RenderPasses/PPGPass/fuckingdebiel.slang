#define G_PI 3.141592653589793
#define G_16_BITMASK_LEFT  0xFFFF0000
#define G_16_BITMASK_RIGHT 0x0000FFFF

//import Scene.Raytracing; // contains HitShaderParams
struct HitShaderParams
{
    uint _mMember; // doesnt really matter what this is
    
    /** Returns the global hit ID (= mesh instance ID in the scene).
        This function can only be called from a ray tracing hit program.
        \return Global hit ID.
    */
    uint getGlobalHitID()
    {
        return 0; // obviously not correct
    }
};
//import Scene.HitInfo;
// This file contains: (remove later, here for IDE support while developing
struct HitInfo
{
    uint meshInstanceID; ///< Mesh instance ID at hit.
    uint primitiveIndex; ///< Primitive index at hit.
    float2 barycentrics; ///< Barycentric coordinates at ray hit, always in [0,1].

    static const uint kInvalidIndex = 0xffffffff;

    /** Return the barycentric weights.
    */
    float3 getBarycentricWeights()
    {
        return float3(1.f - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
    }

    // Encode a HitInfo struct into a 64 bit value (works for some weird reason)
    uint2 encode()
    {
        return uint2(0);
    }

    // Decode a 64 bit value into a HitInfo struct
    [mutating]bool decode(uint2 packed)
    {
        return false;
    }
};
Texture2D<float4> gDTreeSums;
// |ch0ch1| |ch2ch3|
Texture2D<uint2> gDTreeChildren;

Texture2D<uint4> gSTreeData; // x: DTreeIndex, y: axis, zw: child indices

// pos (x, y, y) (relative)
RWTexture2D<float3> gSamplePos;
// Li  (r, g, b)
RWTexture2D<float3> gSampleRadiance;
// dir (u, v) and pdf
RWTexture2D<float3> gSamplePdf;

cbuffer PPGBuf
{
    uint gMaxDTreeNodes;
    uint gAmountOfDTrees;
    
    uint gAmountOfSTreeNodesPerRow;
    uint gFrameIndex; // Frame index for initialising samplegenerator
    /**** Memory block stops here ****/
    float3 gSceneBounds;
    uint _gPadding;
    /**** Memory block stops here ****/
};

// Payload is attached to a ray and can be changed by hit and miss shaders.
// Final value of this payload is written back to the caller (TraceRay has payload parameter)
struct ShadowRayPayload
{
    bool mVisible = false;
};

struct ScatterRayPayload
{
    uint2 mPackedHitInfoData;
};

struct DTreeNode
{
    // if mSums == (0, 0, 0, 0), then node is empty
    float4 mSums;
    uint2 mChildren;

    // Encode a leaf as a node with child indices 0 (which is the root)
    bool isLeaf()
    {
        return !any(mChildren);
    }

    uint getChildIndex(uint localIndex)
    {
        uint packedData = localIndex <= 1 ? mChildren.x : mChildren.y;
        return localIndex & 1 ? packedData & G_16_BITMASK_RIGHT : packedData >> 16;
    }
};

// Decent into the DTree, calculate child index and update p (rescale to [0, 1])
uint decentIntoDTree(inout float2 p)
{
    bool2 kaas = p < float2(.5f);
    p = kaas ? p * 2 : mad(2.f, p, -1.f);
    return mad(2, kaas.y, kaas.x);
}

DTreeNode buildDTreeNode(uint treeIndex, uint nodeIndex)
{
    DTreeNode res = { };
    uint2 texIndex = uint2(nodeIndex, treeIndex);
    res.mSums = gDTreeSums[texIndex];
    res.mChildren = gDTreeChildren[texIndex];
    return res;
}

struct STreeNode
{
    uint mDTreeIndex;
    uint mAxis;
    uint2 mChildren;

    bool isLeaf()
    {
        return !any(mChildren);
    }
};

STreeNode buildSTreeNode(uint nodeIndex)
{
    STreeNode res = { };
    uint2 texIndex = uint2(nodeIndex / gAmountOfSTreeNodesPerRow, nodeIndex % gAmountOfSTreeNodesPerRow);
    uint4 packedData = gSTreeData[texIndex];
    res.mDTreeIndex = packedData.x;
    res.mAxis = packedData.y;
    res.mChildren = packedData.zw;
}

float2 sampleDTree(uint treeIndex, DTreeNode currNode)
{
    float factor = .5f;
    float2 res = float2(0.f);
    while (!currNode.isLeaf())
    {
        float rand = 0; // generate random number here
        float4 sums = currNode.mSums;
        sums.y = sums.x + sums.y;
        sums.z = sums.y + sums.z;
        sums.w = sums.z + sums.w;
        rand *= sums.w;
        sums = currNode.mSums <= rand ? 1u : 0u;
        uint index = sums.x + sums.y + sums.z + sums.w;
        float2 origin = float2(index & 1 ? 0.5 : 0, 0) + float2(0, index & 2 ? 0.5 : 0);
        res = mad(factor, origin, res); // factor*origin + res
        factor *= .5f;

        currNode = buildDTreeNode(treeIndex, currNode.getChildIndex(index));
    }

    return mad(factor, float2(0.f), res); // Generate random 2D number here
    

    /*DTreeNode nextNode = buildDTreeNode(treeIndex, currNode.getChildIndex(index));
    
    if (nextNode.isLeaf())
    {
        return origin + 0.5f * float2(0.f); // Generate random 2D number here (at space of 0)
    }
    else
    {
        return origin + 0.5f * sampleDTree(treeIndex, nextNode);
    }*/
}

float2 sampleDTree(uint treeIndex)
{
    // check if tree is empty, if so, return uniformly random number
    DTreeNode node = buildDTreeNode(treeIndex, 0);
    if (all(node.mSums == 0.f))
        return float2(0.f); // generate uniform random number here
    return saturate(sampleDTree(treeIndex, buildDTreeNode(treeIndex, 0)));
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

uint sampleSTree(float3 loc, float3 size, STreeNode node)
{
    while (!node.isLeaf())
    {
        size[node.mAxis] /= 2;
        uint index = loc[node.mAxis] < size[node.mAxis] ? 0 : 1;
        if (index)
            loc[node.mAxis] -= size[node.mAxis];

        node = buildSTreeNode(node.mChildren[index]);
    }
    return node.mDTreeIndex;
    //if (node.isLeaf())
    //   return node.mDTreeIndex;
    //return sampleSTree(loc, size, buildSTreeNode(node.mChildren[index]));
}

// Loc must be relative!! (must lie between (0,0,0) and size)
float3 sampleSTree(float3 loc, float3 size)
{
    // decent into STree to find leaf node
    uint dTreeInd = sampleSTree(loc, size, buildSTreeNode(0));
    // sample from DTree in leaf node
    return canonicalToDir(sampleDTree(dTreeInd));
}

float pdf(float2 p, const uint dTreeIndex, DTreeNode node)
{
    const uint childRelativeIndex = decentIntoDTree(p);
    /* p is rescaled to [0, 1] at this point */
    float localPdf = 4 * node.mSums[childRelativeIndex] /
    (node.mSums[0] + node.mSums[1] + node.mSums[2] + node.mSums[3]);
    localPdf = isfinite(localPdf) ? localPdf : 0;
    uint childAbsoluteIndex = node.getChildIndex(childRelativeIndex);
    // Check if node is leaf, if yes, return result
    if (node.isLeaf())
    {
        return localPdf;
    }
    else
    {
        return localPdf * pdf(p, dTreeIndex, buildDTreeNode(dTreeIndex, childAbsoluteIndex));
    }

}

float pdf(float3 dir, float3 loc, float3 size)
{
    uint dTreeInd = sampleSTree(loc, size, buildSTreeNode(0));
    return pdf(dirToCanonical(dir), dTreeInd, buildDTreeNode(dTreeInd, 0));
}

/*******************************/
/***** SCATTER RAY SHADERS *****/
/*******************************/

// Scatter ray anyhit shader
[shader("anyhit")]
void scatterRayAnyHit(
    uniform HitShaderParams hitParams, // Contains some hit information (provided by Falcor)
    inout ScatterRayPayload payload : SV_RayPayload, // Payload defined above
    BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes) // Intersection Attributes of hardware raytracing hardware
{
    // possible ignore hit if transparent, for now, do nothing
}

// Scatter ray closest hit
// This is the only scatter ray shader that does any work
[shader("closesthit")]
void scatterRayClosestHit(
    uniform HitShaderParams hitParams, // Contains some hit information (provided by Falcor)
    inout ScatterRayPayload payload : SV_RayPayload, // Payload defined above
    BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes) // Intersection Attributes of hardware raytracing hardware
{
    HitInfo toPack;
    toPack.meshInstanceID = hitParams.getGlobalHitID();
    toPack.primitiveIndex = Primitivendex(); // change int
    toPack.barycentrics = attribs.barycentrics;
    payload.mPackedHitInfoData = toPack.encode();
}

// Scatter ray miss
[shader("miss")]
void scatterRayMiss(inout ScatterRayPayload rayPayload : SV_RayPayload)
{
    // Do nothing, payload was initilised as invalid!
}

/*******************************/
/***** SHADOW  RAY SHADERS *****/
/*******************************/

// Shadow ray anyhit shader
[shader("anyhit")]
void shadowRayAnyHit(
    uniform HitShaderParams hitParams, // Contains some hit information (provided by Falcor)
    inout ShadowRayData rayData : SV_RayPayload, // Payload defined above
    BuiltInTriangleIntersectionAttributes attribs : SV_IntersectionAttributes) // Intersection Attributes of hardware raytracing hardware
{
    // Found a hit, light source is not visible
    // Possible ignore hit if material is transparent (alpha == 0.f)
    AcceptHitAndEndSearch();
}

// Shadow ray closest hit (not needed for shadow rays)
//[shader("closesthit")]
//void shadowRayClosestHit( /* args */)
//{
//    // Not needed
//}

// Shadow ray miss
[shader("miss")]
void shadowRayMiss(inout ShadowRayPayload rayData : SV_RayPayload)
{
    // No intersections found (or found and ignored), light source is visible.
    rayData.mVisible = true;
}

[shader("raygeneration")]
void main()
{
    // Generate rays right here biatch
}
