#define G_2_BITMASK_RIGHT 0x00000003 // 0b0000.0000|0000.0000|0000.0000|0000.0011
#define G_30_BITMASK_LEFT 0xFFFFFFFC // 0b1111.1111|1111.1111|1111.1111|1111.1100

#define G_PI 3.141592653589793
#define G_16_BITMASK_LEFT  0xFFFF0000
#define G_16_BITMASK_RIGHT 0x0000FFFF

//#define K_EVAL_DIRECT 0
//#define K_EVAL_FIRST_DIRECT_BOUNCE 1
//#define K_HEMISPHERE_SAMPLE_ITERATIONS 8

/*import Scene.Raytracing;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import Experimental.Scene.Material.MaterialShading;
import Experimental.Scene.Lights.LightHelpers;

import RenderPasses.Shared.PathTracer.PathTracerHelpers;*/

import Experimental.Scene.Material.MaterialShading;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGenerator;
import RenderPasses.Shared.PathTracer.PathTracerHelpers;
//import Scene.Scene;
import Scene.Raytracing; // contains HitShaderParams
import Scene.HitInfo;
import RenderPasses.Shared.PathTracer.RayFootprint;
import PPGReservoir;

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
    PackedHitInfo mPackedHitInfoData;
    /*float3 mThrouhgput; // 12 bytes
    uint mSampleTreeIndex; // 4 bytes
    float3 mLTotal;
    uint mSampleNodeIndex;
    float3 mNextSampleOrigin;
    uint mEvalEmissive;
    float3 mNextSampleDir;
    float mSamplePdf;
    SampleGenerator mSampleGenerator; // max 16 Bytes*/
};

struct MyPathData
{
    float3 mThroughput;
    float mSamplePdf;
    float3 mLTotal;
    float3 mCurrentRayOrigin;
    float3 mCurrentRayDir;
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

    uint getAxis()
    {
        return mAxis & G_2_BITMASK_RIGHT;
    }

    uint getParent()
    {
        return (mAxis & G_30_BITMASK_LEFT) >> 2;
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

float calc_pdf(float2 p, const uint dTreeIndex, DTreeNode node, out float radianceEstimate)
{
    const uint kMaxIters = 30; // Max DTree depth is 20, so no problem
    bool keepGoing = true;
    float res = 1;
    float sterradians = 4.f * G_PI;
    uint i = 0;
    uint relIndex;
    while (keepGoing)
    {
        float sum = node.mSums.x + node.mSums.y + node.mSums.z + node.mSums.w;
        
        if (sum == 0.f)
        {
            break;
        }
        
        relIndex = decentIntoDTree(p);
        uint absIndex = node.getChildIndex(relIndex);
        
        res *= 4 * node.mSums[relIndex] / sum;
        sterradians /= 4.f;
        
        if (absIndex == 0)
            keepGoing = false;
        else
            node = buildDTreeNode(dTreeIndex, absIndex);
    }
    res = res / (4 * G_PI);
    radianceEstimate = node.mSums[relIndex] / sterradians;
    return res;
}

HemisphereSample sampleDTree(uint treeIndex, DTreeNode currNode, inout SampleGenerator sg, out float radianceEstimate)
{
    bool keepGoing = true;
    float offsetFactor = 1.f;
    float steradians = 4.f * G_PI;
    uint absIndex = 0, relIndex;
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
        relIndex = indexHelper.x + indexHelper.y + indexHelper.z + indexHelper.w;
        absIndex = currNode.getChildIndex(relIndex);

        res.pdf *= 4 * currNode.mSums[relIndex] / cummulativeSums.w;
        steradians /= 4;

        float2 relOffset = float2((relIndex & 1) ? 0.5f : 0.f, (relIndex & 2) ? 0.5f : 0.f);
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
    radianceEstimate = currNode.mSums[relIndex] / steradians;
    return res;
}

float sum(const float3 components)
{
    return components.x + components.y + components.z;
}

HemisphereSample sampleHemisphere(uint treeIndex, const ShadingData sd, inout SampleGenerator sg)
{
    Reservoir r = createEmptyReservoir();
    DTreeNode node = buildDTreeNode(treeIndex, 0);
    
    float3 sampleAsDir;
    float radianceEstimate;
    float bsdfPdf, treePdf, targetPdf;
    // Loop because both calc_pdf and sampleDTree are large functions
    [loop]
    for (int i = 0; i < K_HEMISPHERE_SAMPLE_ITERATIONS; i++)
    {
        if (sampleNext1D(sg) < 0.5f)
        {
            BSDFSample bsdfSample = { };
            bool valid = sampleBSDF(sd, sg, bsdfSample);
            sampleAsDir = bsdfSample.wi;
            treePdf = valid ? calc_pdf(dirToCanonical(bsdfSample.wi), treeIndex, node, radianceEstimate) : 1;
            bsdfPdf = valid ? bsdfSample.pdf : 1;
            targetPdf = valid ? sum(bsdfSample.weight) * bsdfSample.pdf * max(radianceEstimate, 1e-3) : 0;
        }
        else
        {
            HemisphereSample treeSample = sampleDTree(treeIndex, node, sg, radianceEstimate);
            sampleAsDir = normalize(treeSample.getSampleAsDir());
            bsdfPdf = evalPdfBSDF(sd, sampleAsDir);
            treePdf = treeSample.pdf;
            targetPdf = sum(evalBSDFCosine(sd, sampleAsDir)) * max(radianceEstimate, 1e-3);
        }
        float candidatePdf = 0.5f * bsdfPdf + 0.5f * treePdf;
        float weight = (candidatePdf > 0) ? (targetPdf / candidatePdf) : 0;
        r.addToReservoir(sampleAsDir, targetPdf, weight, sg);
    }

    HemisphereSample hemSample = { };
    hemSample.mSample = dirToCanonical(r.mSample);
    hemSample.pdf = r.mTargetPdf * rcp(r.getMCCorrectingFactor());
    return hemSample;
}

HemisphereSample sampleDTree(uint treeIndex, const ShadingData sd, inout SampleGenerator sg)
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
    //const float uniformPdfProbability = 0.f;
    //float rand = sampleNext1D(sg);
    HemisphereSample res;
    /*if (rand < uniformPdfProbability)
    {
        res.mSample = sampleNext2D(sg);
        //res.pdf = 1; // nee dus
        res.pdf = (1 - uniformPdfProbability) *
            calc_pdf(res.mSample, treeIndex, buildDTreeNode(treeIndex, 0)) +
            0.25 * uniformPdfProbability / G_PI;
    }*/
    //else
    //{
        res = sampleHemisphere(treeIndex, sd, sg);
        res.pdf = /*(1 - uniformPdfProbability) **/ res.pdf /*+ 0.25 * uniformPdfProbability / G_PI*/;
    //}
    return res;
}

uint sampleSTree(float3 loc, float3 min, float3 size, STreeNode node)
{
    while (!node.isLeaf())
    {
        size[node.getAxis()] /= 2;
        uint index = loc[node.getAxis()] < min[node.getAxis()] + size[node.getAxis()] ? 0 : 1;
        if (index == 1)
            min[node.getAxis()] += size[node.getAxis()];
        
        /*uint index = loc[node.getAxis()] < size[node.getAxis()] ? 0 : 1;
        if (index == 1)
            loc[node.getAxis()] -= size[node.getAxis()];*/

        node = buildSTreeNode(node.mChildren[index]);
    }
    return node.mDTreeIndex;
    //if (node.isLeaf())
    //   return node.mDTreeIndex;
    //return sampleSTree(loc, size, buildSTreeNode(node.mChildren[index]));
}

// Loc must be relative!! (must lie between (0,0,0) and size)
HemisphereSample sampleSTree(float3 loc, const ShadingData sd, inout SampleGenerator sg)
{
    // decent into STree to find leaf node
    uint dTreeInd = sampleSTree(loc, gSceneMin, gSceneMax - gSceneMin, buildSTreeNode(0));
    // sample from DTree in leaf node
    return sampleDTree(dTreeInd, sd, sg);
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
    HitInfo info = { };
    info.instanceID = hitParams.getGlobalHitID();
    info.primitiveIndex = PrimitiveIndex();
    info.barycentrics = attribs.barycentrics;
    payload.mPackedHitInfoData = info.encode();
}

// Scatter ray miss
[shader("miss")]
void scatterRayMiss(inout ScatterRayPayload rayPayload : SV_RayPayload)
{
    // Do nothing, payload was initilised as invalid!
    // Eval environment map if direct light sampling is disabled.
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
        SampleGenerator sg = SampleGenerator.create(screenPos, gFrameIndex);

        for (int i = 0; i < 2; i++)
            sampleNext2D(sg);
        
        
        //ScatterRayPayload payload = { };
        MyPathData pathData = { };
        pathData.mLTotal = K_EVAL_FIRST_DIRECT_BOUNCE ? sd.emissive : 0;
        pathData.mSamplePdf = -1;
        pathData.mThroughput = 1;
        pathData.mCurrentRayDir = float3(0.f);
        pathData.mCurrentRayOrigin = absPos;

        const uint kMaxBounces = K_MAX_BOUNCES;
        //bool keepGoing = true;
        uint i = 0;
        MyPathData prefixPath = { };
        prefixPath.mThroughput = 1;
        prefixPath.mLTotal = 0;
        prefixPath.mCurrentRayOrigin = pathData.mCurrentRayOrigin;
        prefixPath.mCurrentRayDir = pathData.mCurrentRayDir;
        prefixPath.mSamplePdf = pathData.mSamplePdf;

        uint pathIndex = sampleNext1D(sg) * kMaxBounces;
        pathIndex = max(pathIndex, kMaxBounces - 1); // should not ever happen
        //pathIndex = 0;
        
        RayDesc ray = { };
        while (true)
        {
            if (K_EVAL_DIRECT)
            {
                if (K_EVAL_FIRST_DIRECT_BOUNCE || i > 0) // Written to aid compiler in optimisation
                {
                    SceneLightSample sls = { };
                    bool valid = sampleSceneLights(gData.params, gData.envMapSampler, gData.emissiveLightSampler, sd, pathData.mCurrentRayOrigin, 1, sg, sls);
                    ShadowRayPayload shadowRayPayload = { };
                    shadowRayPayload.mVisible = false;
                
                    ray = {
                    };
                    ray.Origin = pathData.mCurrentRayOrigin;
                    ray.Direction = sls.dir;
                    ray.TMin = 1e-3f;
                    ray.TMax = sls.distance;

                    TraceRay(gRtScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff, 1, hitProgramCount, 1, ray, shadowRayPayload);

                    if (valid && shadowRayPayload.mVisible)
                    {
                        pathData.mLTotal += pathData.mThroughput * sls.Li * evalBSDFCosine(sd, sls.dir);
                    }
                }
            }

            if (i++ >= kMaxBounces)
                break;

            HemisphereSample nextSample = sampleSTree(pathData.mCurrentRayOrigin, sd, sg);
    
            pathData.mCurrentRayDir = nextSample.getSampleAsDir();

            if (dot(pathData.mCurrentRayDir, sd.faceN) < 0.f)
            {
                break;
            }
    
            pathData.mThroughput *= evalBSDFCosine(sd, pathData.mCurrentRayDir) * rcp(nextSample.pdf);
    
            pathData.mSamplePdf = nextSample.pdf;
            
            if (i == 0 || i == pathIndex)
            {
                prefixPath.mLTotal += prefixPath.mThroughput * pathData.mLTotal;
                prefixPath.mThroughput *= pathData.mThroughput;
                prefixPath.mCurrentRayOrigin = pathData.mCurrentRayOrigin;
                prefixPath.mCurrentRayDir = pathData.mCurrentRayDir;
                prefixPath.mSamplePdf = pathData.mSamplePdf;
                
                pathData.mThroughput = float3(1.f);
                pathData.mLTotal = float3(0.f);
            }
            
            ray = { };
            ray.Origin = pathData.mCurrentRayOrigin;
            ray.Direction = pathData.mCurrentRayDir;
            ray.TMin = 1e-3f;
            ray.TMax = kRayTMax;

            payload.mPackedHitInfoData = {
                HitInfo::kInvalidIndex
            }; // cuz k den
            
            TraceRay(gRtScene, RAY_FLAG_NONE, 0xff, 0, hitProgramCount, 0, ray, payload);

            if (payload.mPackedHitInfoData.x == HitInfo::kInvalidIndex)
               break;
            
            HitInfo hit = { };
            hit.decode(payload.mPackedHitInfoData);
            VertexData v = gScene.getVertexData(hit);
            const uint materialID = gScene.getMaterialID(hit.instanceID);
            sd = prepareShadingData(v, materialID, gScene.materials[materialID],
                gScene.materialResources[materialID], -ray.Direction, 0.f);
            
            float3 rayOrigin = sd.computeNewRayOrigin();
            pathData.mCurrentRayOrigin = rayOrigin;

            if (K_EVAL_FIRST_DIRECT_BOUNCE || i != 1)
                pathData.mLTotal += (!K_EVAL_DIRECT) ? pathData.mThroughput * sd.emissive : float3(0.f);
        }
            
        cameraContribution = prefixPath.mLTotal + prefixPath.mThroughput * pathData.mLTotal;

        sampleDataPos = float4(prefixPath.mCurrentRayOrigin, 0.f);
        sampleDataRadiance = float4(pathData.mLTotal, 0.f);
        sampleDataDir = float4(dirToCanonical(prefixPath.mCurrentRayDir), prefixPath.mSamplePdf, 0.f);
        
    }
    else
    {
        // eval env map
    }
    
    gSamplePos[screenPos] = sampleDataPos;
    gSampleRadiance[screenPos] = sampleDataRadiance;
    gSamplePdf[screenPos] = sampleDataDir;

    gOutputColor[screenPos] = float4(cameraContribution, 1.f);
}
