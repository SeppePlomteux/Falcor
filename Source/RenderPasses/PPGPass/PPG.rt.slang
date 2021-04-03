#define G_PI 3.141592653589793
#define G_16_BITMASK_LEFT  0xFFFF0000
#define G_16_BITMASK_RIGHT 0x0000FFFF


/*import Scene.Raytracing;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Experimental.Scene.Material.MaterialShading;
import Experimental.Scene.Lights.LightHelpers;

import RenderPasses.Shared.PathTracer.PathTracerHelpers;*/

import Experimental.Scene.Material.MaterialShading;
import Utils.Math.MathHelpers;
import RenderPasses.Shared.PathTracer.PathTracerHelpers;
import Scene.Scene;
import Scene.Raytracing; // contains HitShaderParams
import Scene.HitInfo;
import RenderPasses.Shared.PathTracer.RayFootprint;

Texture2D<float4> gDTreeSums;
// |ch0ch1| |ch2ch3|
Texture2D<uint2> gDTreeChildren;

Texture2D<uint4> gSTreeData; // x: DTreeIndex, y: axis, zw: child indices

RWTexture2D<float4> gOutputColor;

// pos (x, y, y) (relative)
RWTexture2D<float4> gSamplePos;
// Li  (r, g, b)
RWTexture2D<float4> gSampleRadiance;
// dir (u, v) and pdf
RWTexture2D<float4> gSamplePdf;

// Shading inputs
Texture2D<float4> gWorldPos; // w component contains depth
Texture2D<float4> gShadingNormal;
Texture2D<float4> gGeometricNormal;
Texture2D<float4> gMaterialDiffuseOpacity;
Texture2D<float4> gMaterialSpecularRoughness;
Texture2D<float4> gMaterialEmissive;
Texture2D<float4> gMaterialExtraParams;

struct SharedTracerData
{
    PathTracerParams params;
    EnvMapSampler envMapSampler;
    EmissiveLightSampler emissiveLightSampler;
};

ParameterBlock<SharedTracerData> gData;

cbuffer PPGBuf
{   
    uint gAmountOfSTreeNodesPerRow;
    uint gFrameIndex; // Frame index for initialising samplegenerator
    uint2 _gPadding;
    /**** Memory block stops here ****/
    float3 gSceneMin;
    uint _gPadding2;
    /**** Memory block stops here ****/
    float3 gSceneMax;
};

// Payload is attached to a ray and can be changed by hit and miss shaders.
// Final value of this payload is written back to the caller (TraceRay has payload parameter)
struct ShadowRayPayload
{
    bool mVisible = false;
};

struct ScatterRayPayload // In total 80 bytes
{
    //uint2 mPackedHitInfoData;
    float3 mThrouhgput; // 12 bytes
    uint mSampleTreeIndex; // 4 bytes
    float3 mLTotal;
    uint mSampleNodeIndex;
    float3 mNextSampleOrigin;
    uint mEvalEmissive;
    float3 mNextSampleDir;
    float mSamplePdf;
    SampleGenerator mSampleGenerator; // max 16 Bytes
};

struct DTreeNode
{
    // if mSums == (0, 0, 0, 0), then node is empty
    float4 mSums;
    uint2 mChildren;

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
};

// Decent into the DTree, calculate child index and update p (rescale to [0, 1])
uint decentIntoDTree(inout float2 p)
{
    //bool2 kaas = p < float2(.5f);
    //p = kaas ? p * 2 : mad(2.f, p, -1.f);
    //return mad(2, kaas.y, kaas.x);
    uint2 kaas = p < float2(.5f) ? 1 : 0;
    p = kaas ? p * 2 : mad(2.f, p, -1.f);
    return mad(2, 1 - kaas.y, 1 - kaas.x);
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

struct HemisphereSample
{
    float2 mSample;
    float pdf;

    float3 getSampleAsDir()
    {
        return canonicalToDir(mSample);
    }
};

float calc_pdf(float2 p, const uint dTreeIndex, DTreeNode node)
{
    const uint kMaxIters = 30; // Max DTree depth is 20, so no problem
    bool keepGoing = true;
    float res = 1;
    uint i = 0;
    while (keepGoing)
    {
        float sum = node.mSums.x + node.mSums.y + node.mSums.z + node.mSums.w;
        
        if (sum == 0.f)
        {
            break;
        }
        
        uint relIndex = decentIntoDTree(p);
        uint absIndex = node.getChildIndex(relIndex);
        
        res *= 4 * node.mSums[relIndex] / sum;
        
        if (absIndex == 0)
            keepGoing = false;
        else
            node = buildDTreeNode(dTreeIndex, absIndex);
    }
    res = res / (4 * G_PI);
    return res;
}

HemisphereSample sampleDTree(uint treeIndex, DTreeNode currNode, SampleGenerator sg)
{
    bool keepGoing = true;
    float offsetFactor = 1.f;
    uint absIndex = 0;
    HemisphereSample res;
    res.mSample = float2(0.f, 0.f);
    res.pdf = 1;
    while (keepGoing)
    {
        float rand = sampleNext1D(sg); // Sample a random number
        // sample one of the 4 subspheres
        float4 cummulativeSums = currNode.mSums;
        cummulativeSums.y = cummulativeSums.x + cummulativeSums.y;
        cummulativeSums.z = cummulativeSums.y + cummulativeSums.z;
        cummulativeSums.w = cummulativeSums.z + cummulativeSums.w;

        if (cummulativeSums.w == 0.f)
        {
            break;
        }
        
        rand *= cummulativeSums.w;
        uint4 indexHelper = cummulativeSums <= rand ? 1u : 0u;
        uint relIndex = indexHelper.x + indexHelper.y + indexHelper.z + indexHelper.w;
        absIndex = currNode.getChildIndex(relIndex);

        res.pdf *= 4 * currNode.mSums[relIndex] / cummulativeSums.w;

        float2 relOffset = float2(relIndex & 1 ? 0.5f : 0.f, relIndex & 2 ? 0.5f : 0.f);
        res.mSample = mad(offsetFactor, relOffset, res.mSample);
        offsetFactor *= 0.5f;
        
        if (absIndex == 0)
            keepGoing = false;
        else
        {
            currNode = buildDTreeNode(treeIndex, absIndex);
        }
    }
    res.mSample = saturate(mad(offsetFactor, sampleNext2D(sg), res.mSample));
    res.pdf = res.pdf / (4 * G_PI);
    return res;
}

/*float2 sampleDTree(uint treeIndex, DTreeNode currNode, SampleGenerator sg)
{
    float factor = 1.f; // was 0.5f
    float2 res = float2(0.f);
    while (!currNode.isLeaf())
    {
        float rand = sampleNext1D(sg);
        float4 sums = currNode.mSums;
        sums.y = sums.x + sums.y;
        sums.z = sums.y + sums.z;
        sums.w = sums.z + sums.w;
        if (sums.w == 0.f)
            break;
        rand *= sums.w;
        sums = sums <= rand ? 1u : 0u;
        uint index = sums.x + sums.y + sums.z + sums.w;
        float2 origin = float2(index & 1 ? 0.5 : 0, index & 2 ? 0.5 : 0);
        res = mad(factor, origin, res); // factor*origin + res
        factor *= .5f;
        
        if (currNode.getChildIndex(index) == 0)
        {
            break;
        }
        
        currNode = buildDTreeNode(treeIndex, currNode.getChildIndex(index));
    }

    return mad(factor, sampleNext2D(sg), res);
    

    /*DTreeNode nextNode = buildDTreeNode(treeIndex, currNode.getChildIndex(index));
    
    if (nextNode.isLeaf())
    {
        return origin + 0.5f * float2(0.f); // Generate random 2D number here (at space of 0)
    }
    else
    {
        return origin + 0.5f * sampleDTree(treeIndex, nextNode);
    }
}*/

HemisphereSample sampleDTree(uint treeIndex, SampleGenerator sg)
{
    // check if tree is empty, if so, return uniformly random number
    DTreeNode node = buildDTreeNode(treeIndex, 0);
    if (all(node.mSums == 0.f))
    {
        HemisphereSample res = { };
        res.mSample = sampleNext2D(sg);
        //res.mSample = float2(0.5, 0.5);
        res.pdf = 0.25 / G_PI;
        return res;
    }
    const float uniformPdfProbability = 0.1f;
    float rand = sampleNext1D(sg);
    HemisphereSample res;
    if (rand < uniformPdfProbability)
    {
        res.mSample = sampleNext2D(sg);
        //res.pdf = 1; // nee dus
        res.pdf = (1 - uniformPdfProbability) *
            calc_pdf(res.mSample, treeIndex, buildDTreeNode(treeIndex, 0)) +
            0.25 * uniformPdfProbability / G_PI;
    }
    else
    {
        res = sampleDTree(treeIndex, buildDTreeNode(treeIndex, 0), sg);
        res.pdf = (1 - uniformPdfProbability) * res.pdf + 0.25 * uniformPdfProbability / G_PI;
    }
    return res;
}

uint sampleSTree(float3 loc, float3 min, float3 size, STreeNode node)
{
    while (!node.isLeaf())
    {
        size[node.mAxis] /= 2;
        uint index = loc[node.mAxis] < min[node.mAxis] + size[node.mAxis] ? 0 : 1;
        if (index == 1)
            min[node.mAxis] += size[node.mAxis];
        
        /*uint index = loc[node.mAxis] < size[node.mAxis] ? 0 : 1;
        if (index == 1)
            loc[node.mAxis] -= size[node.mAxis];*/

        node = buildSTreeNode(node.mChildren[index]);
    }
    return node.mDTreeIndex;
    //if (node.isLeaf())
    //   return node.mDTreeIndex;
    //return sampleSTree(loc, size, buildSTreeNode(node.mChildren[index]));
}

// Loc must be relative!! (must lie between (0,0,0) and size)
HemisphereSample sampleSTree(float3 loc, SampleGenerator sg)
{
    // decent into STree to find leaf node
    uint dTreeInd = sampleSTree(loc, gSceneMin, gSceneMax - gSceneMin, buildSTreeNode(0));
    // sample from DTree in leaf node
    return sampleDTree(dTreeInd, sg);
}

/*float calc_pdf(float3 dir, float3 loc, float3 size)
{
    uint dTreeInd = sampleSTree(loc, size, buildSTreeNode(0));
    return calc_pdf(dirToCanonical(dir), dTreeInd, buildDTreeNode(dTreeInd, 0));
}*/

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
    for (int i = 0; i < 2; i++) // TODO test
        sampleNext2D(payload.mSampleGenerator);
    
    VertexData v = getVertexData(hitParams, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(hitParams.getGlobalHitID());
    ShadingData sd = prepareShadingData(v, materialID, gScene.materials[materialID],
        gScene.materialResources[materialID], -WorldRayDirection(), 0.f);
    
    float3 rayOrigin = sd.computeNewRayOrigin();
    
    /*SceneLightSample sls; //
    bool sampleSucceeded = sampleSceneLights(
        gData.params, gData.envMapSampler, gData.emissiveLightSampler, sd, rayOrigin, 1, sg, sls
    );

    RayDesc ray = { };
    ray.Origin = absPos;
    ray.Direction = sampleDir;
    ray.TMin = 1e-3f; // Maybe change
    ray.TMax = kRayTMax; // kRayTMax is defined somewhere*/
    
    //if (payload.mSamplePdf >= 0)
    payload.mLTotal += payload.mEvalEmissive ? payload.mThrouhgput * sd.emissive : float3(0.f);
    payload.mEvalEmissive = 1;
    
    HemisphereSample nextSample = sampleSTree(rayOrigin, payload.mSampleGenerator);
    
    payload.mNextSampleDir = nextSample.getSampleAsDir();
    payload.mNextSampleOrigin = rayOrigin;
    payload.mThrouhgput *= evalBSDFCosine(sd, payload.mNextSampleDir) * rcp(nextSample.pdf);
    
    payload.mSamplePdf = nextSample.pdf;

    payload.mSampleTreeIndex = 0;

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
    inout ShadowRayPayload rayData : SV_RayPayload, // Payload defined above
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

MaterialParams loadMaterialParams(uint2 pixelPos)
{
    MaterialParams matParams;
    matParams.diffuseOpacity = gMaterialDiffuseOpacity[pixelPos];
    matParams.specularRoughness = gMaterialSpecularRoughness[pixelPos];
    matParams.emissive = gMaterialEmissive[pixelPos];
    matParams.extraParams = gMaterialExtraParams[pixelPos];

    return matParams;
}

struct SDTreeSample
{
    float3 mOrigin;
    float mPdf;
    
    float3 mRadiance;
    uint mTreeIndex;
    
    float2 mDirection;
    uint mNodeIndex;
};

[shader("raygeneration")]
void main()
{
    const uint2 screenPos = DispatchRaysIndex().xy;
    const uint2 screenSize = DispatchRaysDimensions().xy;
    
    const float3 absPos = gWorldPos[screenPos].xyz;
 
    float3 primaryRayDir = gScene.camera.computeRayPinhole(screenPos, screenSize).dir;

    float3 normal = gShadingNormal[screenPos].xyz;
    float4 tangent = float4(perp_stark(normal), 1);
    float3 geoNormal = gGeometricNormal[screenPos].xyz;
    GeometryParams geoParams = prepareGeometryParams(absPos.xyz, -primaryRayDir, normal, tangent, geoNormal);

    // Load material parameters from G-buffer
    MaterialParams matParams = loadMaterialParams(screenPos);

    // Prepare shading data struct
    ShadingData sd = prepareShadingData(geoParams, matParams);

    float4 sampleDataPos = float4(0.f);
    float4 sampleDataRadiance = float4(0.f);
    float4 sampleDataDir = float4(0.f, 0.f, -1.f, 0.f);
    
    float3 cameraContribution = float3(0.f, 0.f, 0.f);
    
    if (gWorldPos[screenPos].w != 0)
    {
        ScatterRayPayload payload = { };
        payload.mSampleGenerator = SampleGenerator.create(screenPos, gFrameIndex);

        for (int i = 0; i < 2; i++) // TODO test
            sampleNext2D(payload.mSampleGenerator);
        //SampleGenerator sg = SampleGenerator.create(screenPos, gFrameIndex);
        //float3 relPos = absPos - gSceneMin;
            HemisphereSample nextSample = sampleSTree(absPos, payload.mSampleGenerator);
        float3 sampleDir = nextSample.getSampleAsDir();
        float pdf = nextSample.pdf;
        //float3 toCamera = float3(0.f); // TODO init to direction of shading point to camera
        
        RayDesc ray = { };
        ray.Origin = absPos;
        ray.Direction = sampleDir;
        ray.TMin = 1e-3f; // Maybe change
        ray.TMax = kRayTMax; // kRayTMax is defined somewhere
        //ScatterRayPayload payload = { };
        payload.mLTotal = sd.emissive;
        payload.mThrouhgput = evalBSDFCosine(sd, sampleDir) * rcp(pdf);
        payload.mNextSampleDir = sampleDir;
        payload.mNextSampleOrigin = absPos;
        payload.mSamplePdf = pdf;
        payload.mEvalEmissive = 0;
        //payload.mSampleGenerator = gSampleGenerator;
        //payload.mPackedHitInfoData = HitInfo::kInvalidIndex;
        
        //TraceRay(gRtScene, RAY_FLAG_NONE, 0xff, 0, hitProgramCount, 0, ray, payload);

        const uint kMaxBounces = 3; // warining: waaaaay too large
        bool keepGoing = true;
        uint i = 0;
        float3 pathPrefixThroughput = payload.mThrouhgput;
        float3 pathPrefixLTotal = payload.mLTotal;
        float3 pathPrefixDir = payload.mNextSampleDir;
        float3 pathPrefixOrigin = payload.mNextSampleOrigin;
        float pathPrefixPdf = payload.mSamplePdf;
        payload.mThrouhgput = float3(1.f);
        payload.mLTotal = float3(0.f);

        uint pathIndex = sampleNext1D(payload.mSampleGenerator) * kMaxBounces;
        pathIndex = max(pathIndex, kMaxBounces - 1); // should not ever happen
        //pathIndex = 0;
        
        while (keepGoing)
        {
            if (i == pathIndex)
            {
                pathPrefixLTotal += pathPrefixThroughput * payload.mLTotal;
                pathPrefixThroughput *= payload.mThrouhgput;
                pathPrefixDir = payload.mNextSampleDir;
                pathPrefixOrigin = payload.mNextSampleOrigin;
                pathPrefixPdf = payload.mSamplePdf;
                payload.mThrouhgput = float3(1.f);
                payload.mLTotal = float3(0.f);
            }
            
            ray = { };
            ray.Origin = payload.mNextSampleOrigin;
            ray.Direction = payload.mNextSampleDir;
            ray.TMin = 1e-3f;
            ray.TMax = kRayTMax;

            payload.mNextSampleDir = float3(0.f, 0.f, 0.f);

            TraceRay(gRtScene, RAY_FLAG_NONE, 0xff, 0, hitProgramCount, 0, ray, payload);
            
            keepGoing = (i++ < (kMaxBounces - 1)) && any(payload.mNextSampleDir != 0.f);
        }

        
        // always interpret a hit, even if ray missed everything
        //HitInfo hit;
        //hit.decode(payload.mPackedHitInfoData);
        //const uint materialID = gScene.getMaterialID(hit.meshInstanceID);
        //StaticVertexData svd[3];
        //VertexData vertexData = gScene.getVertexData(hit, svd);

        //RayFootprint rfp = { }; // assumes no ray footprint

        //ShadingData sd_emissive = prepareShadingData(vertexData, rfp, svd, hit, absPos, normalize(sampleDir)); // is fucked
    
        //ShadingData sd_emissive = prepareShadingData(vertexData, materialID, gScene.materials[materialID],
        //    gScene.materialResources[materialID], -sampleDir, 0.f);
    
        

        //float3 emissive = sd.emissive;
        //float3 brdf = evalBSDFCosine(sd, sampleDir); // eval brdf*cos of camera point
        ////float3 test = emissive + brdf * payload.mEmissive * rcp(pdf);
        ////float3 test = isfinite(payload.mEmissive) ? 1 : 0;
        // bool validHit = any(payload.mNextSampleDir != float3(0.f)); // hits are always valid!!
        //cameraContribution = validHit ? emissive + brdf * payload.mEmissive * rcp(pdf) : emissive;
        cameraContribution = pathPrefixLTotal + pathPrefixThroughput * payload.mLTotal;
        ////cameraContribution = validHit ? emissive + brdf * sd_emissive.emissive * rcp(pdf) : emissive;

        sampleDataPos = /*validHit ?*/ float4(pathPrefixOrigin, 0.f) /*: float4(0.f)*/;
        sampleDataRadiance = /*validHit ?*/ float4(payload.mLTotal, 0.f) /*: float4(0.f)*/;
        //sampleDataRadiance = validHit ? float4(sd_emissive.emissive, 0.f) : float4(0.f);
        sampleDataDir = /*validHit ?*/ float4(dirToCanonical(pathPrefixDir), pathPrefixPdf, 0.f) /*: float4(0.f, 0.f, -1.f, 0.f)*/;
        
    }
    //AllMemoryBarrier();
    //gSamplePos[screenPos] = float4(relPos, 0.f);
    //gSampleRadiance[screenPos] = float4(payload.mEmissive, 0.f);
    //gSamplePdf[screenPos] = float4(dirToCanonical(sampleDir), pdf, 0.f);
    
    gSamplePos[screenPos] = sampleDataPos;
    gSampleRadiance[screenPos] = sampleDataRadiance;
    gSamplePdf[screenPos] = sampleDataDir;

    gOutputColor[screenPos] = float4(cameraContribution, 1.f);
    //AllMemoryBarrier();
}
