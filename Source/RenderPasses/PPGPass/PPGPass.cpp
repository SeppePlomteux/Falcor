/***************************************************************************
 # Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#include "PPGPass.h"

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("PPGPass", "Render Pass Template", PPGPass::create);
}

namespace
{
    const uint kMaxRecursionDepth = 3U; // was 1
    const uint kMaxPayloadSize = 80U; // was 8
    const uint kMaxAttributeSize = 32U; // was 8

    //size_t kMaxSamplesInSDTree = 1920 * 1080 * 1024;

    const float kSTreeSplitTresHold = 328016.6f;
    const float kSTreeMergeTresHold = 295215.f;
    const float kSTreeStatWeightFactor = 0.9964941f;

    //int kMaxAvailableThreads =  std::thread::hardware_concurrency() - 1; // leave out one thread for main loop

    const uint kSTreeTexWidth = 400;
    const uint kSTreeTexHeight = 400;
    const uint kDTreeTexWidth = 1000;
    const uint kDTreeTexHeight = 8192;

    ResourceBindFlags kDefaultBindFlags = ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess;

    ChannelList kShadingInputChannels = {
        {"pos",         "gWorldPos",                    "World space position (xyz) and depth (w)"},
        {"shNormal",    "gShadingNormal",               "World space shading normal (xyz)"},
        {"geoNormal",   "gGeometricNormal",             "World space geometric normal (xyz)"},
        {"diffuse",     "gMaterialDiffuseOpacity",      "Diffuse brdf component (rgb) and material opacity (a)"},
        {"specular",    "gMaterialSpecularRoughness",   "Specular roughness brdf component (rgb)"},
        {"emissive",    "gMaterialEmissive",            "Emissive brdf component (rgb)"},
        {"extra",       "gMaterialExtraParams",         "Extra material parameters"}
    };

    ChannelList kOutputChannels = {
        {"color",       "gOutputColor",                 "The output color"}
    };

    struct TextureDesc
    {
        const std::string mShaderName;
        const ResourceFormat mFormat;
        const ResourceBindFlags mBindflags = kDefaultBindFlags;
    };

    const struct
    {
        const TextureDesc kSTreeMetaData = { "gSTreeMetaData", ResourceFormat::R32Uint };
        const TextureDesc kSTreeData = { "gSTreeData", ResourceFormat::RGBA32Uint };
        const TextureDesc kSTreeStatWeight = { "gSTreeStatWeight", ResourceFormat::R32Float };
        const TextureDesc kDTreeSums = { "gDTreeSums", ResourceFormat::RGBA32Float };
        const TextureDesc kDTreeChildren = { "gDTreeChildren", ResourceFormat::RG32Uint };
        const TextureDesc kDTreeParent = { "gDTreeParent", ResourceFormat::R32Uint};

        const TextureDesc kDTreeSize = { "gDTreeSize", ResourceFormat::R32Uint };
        const TextureDesc kDTreeStatisticalWeight = { "gDTreeStatisticalWeight", ResourceFormat::R32Uint };
        const TextureDesc kDTreeEditData = { "gDTreeEditData", ResourceFormat::RG32Uint };

        const TextureDesc kSamplePos = { "gSamplePos", ResourceFormat::RGBA32Float };
        const TextureDesc kSampleRadiance = { "gSampleRadiance", ResourceFormat::RGBA32Float };
        const TextureDesc kSampleDirPdf = { "gSamplePdf", ResourceFormat::RGBA32Float };
    } kInternalChannels;
}

PPGPass::PPGPass(const Dictionary& dict) : PathTracer(dict, kOutputChannels)
{
    RtProgram::Desc description;
    description.addShaderLibrary("RenderPasses/PPGPass/PPG.rt.slang").setRayGen("main");
    description.addHitGroup(0, "scatterRayClosestHit", "scatterRayAnyHit").addMiss(0, "scatterRayMiss");
    description.addHitGroup(1, "", "shadowRayAnyHit").addMiss(1, "shadowRayMiss");
    description.setMaxTraceRecursionDepth(kMaxRecursionDepth);
    mpPPGProg = RtProgram::create(description, kMaxPayloadSize, kMaxAttributeSize);

    mSharedParams.useMIS = 0;
    mSharedParams.rayFootprintMode = 0;
    mSharedParams.useLegacyBSDF = 1;

    mTreeTextures.pSTreeTex = nullptr;
    mTreeTextures.pDTreeSumsTex = nullptr;
    mTreeTextures.pDTreeChildrenTex = nullptr;
    mTreeTextures.pDTreeParentTex = nullptr;

    mTreeTextures.pDTreeSizeTex = nullptr;
    mTreeTextures.pDTreeStatisticalWeightTex = nullptr;

    mSDTreeUpdatePasses.pBlitSTreePass = ComputePass::create("RenderPasses/PPGPass/BlitSTreeTex.cs.hlsl", "main");
    mSDTreeUpdatePasses.pBlitDTreePass = ComputePass::create("RenderPasses/PPGPass/BlitDTreeTex.cs.hlsl", "main", {}, false);
    mSDTreeUpdatePasses.pBlitDTreePass->addDefine("G_IS_NEW_TEX", "1", true);
    mSDTreeUpdatePasses.pResetStatisticalWeightPass = ComputePass::create("RenderPasses/PPGPass/ResetStatisticalWeightTex.cs.hlsl", "main");
    mSDTreeUpdatePasses.pRescaleDTreePass = ComputePass::create("RenderPasses/PPGPass/RescaleDTreeSums.cs.hlsl", "main", {}, false);
    mSDTreeUpdatePasses.pSplatIntoSDTreePass = ComputePass::create("RenderPasses/PPGPass/SplatIntoSDTree.cs.hlsl", "main");
    mSDTreeUpdatePasses.pPropagateDTreeSumsPass = ComputePass::create("RenderPasses/PPGPass/PropagateDTreeSums.cs.hlsl", "main");
    mSDTreeUpdatePasses.pResetFreedNodesPass = ComputePass::create("RenderPasses/PPGPass/ResetFreedNodesTex.cs.hlsl", "main");
    mSDTreeUpdatePasses.pUpdateDTreeStructurePass = ComputePass::create("RenderPasses/PPGPass/BuildDTree.cs.hlsl", "main", {}, false);
    mSDTreeUpdatePasses.pCompressDTreePass = ComputePass::create("RenderPasses/PPGPass/CompressDTreeTex.cs.hlsl", "main");
    mSDTreeUpdatePasses.pRescaleSTreeStatWeightPass = ComputePass::create("RenderPasses/PPGPass/RescaleSTreeStatWeight.cs.hlsl", "main", {}, false);
    mSDTreeUpdatePasses.pUpdateSTreeStatWeightPass = ComputePass::create("RenderPasses/PPGPass/UpdateSTreeStatWeight.cs.hlsl", "main");
    mSDTreeUpdatePasses.pUpdateSTreeStructurePass = ComputePass::create("RenderPasses/PPGPass/BuildSTree.cs.hlsl", "main", {}, false);
    mSDTreeUpdatePasses.pCompressSTreePass = ComputePass::create("RenderPasses/PPGPass/CompressSTreeTex.cs.hlsl", "main");
    mSDTreeUpdatePasses.pCopyDTreesPass = ComputePass::create("RenderPasses/PPGPass/CopyDTreeTexRows.cs.hlsl", "main");
    
    //mpResetStatisticalWeightPass = ComputePass::create("RenderPasses/PPGPass/ResetStatisticalWeightTex.cs.hlsl", "main");
    //mpResetMutexPass = ComputePass::create("RenderPasses/PPGPass/ResetMutexTex.cs.hlsl", "main");
    //mpSplatIntoSDTreePass = ComputePass::create("RenderPasses/PPGPass/SplatIntoSDTree.cs.hlsl", "main");
}

PPGPass::SharedPtr PPGPass::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new PPGPass(dict));
    return pPass;
}

Dictionary PPGPass::getScriptingDictionary()
{
    return Dictionary();
}

RenderPassReflection PPGPass::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    addRenderPassInputs(reflector, kShadingInputChannels, kDefaultBindFlags | ResourceBindFlags::RenderTarget);
    addRenderPassOutputs(reflector, kOutputChannels);
    return reflector;
}


std::string bool_to_str(bool b)
{
    if (b)
        return "1";
    return "0";
}

void PPGPass::setTracingDefines()
{
    if (!mSettingsChanged.ppgSettingsChanged)
        return;
    mSettingsChanged.ppgSettingsChanged = false;
    mpPPGVars = nullptr;

    Program::DefineList dl;
    dl.add("K_EVAL_DIRECT", bool_to_str(mRenderSettings.evalDirectLight));
    dl.add("K_EVAL_FIRST_DIRECT_BOUNCE", bool_to_str(mRenderSettings.includeFirstDirectBounce));
    dl.add("K_HEMISPHERE_SAMPLE_ITERATIONS", std::to_string(mRenderSettings.amountOfRISSamples));
    dl.add("K_MAX_BOUNCES", std::to_string(mRenderSettings.maxBounces));

    mpPPGProg->addDefines(dl);
}

void PPGPass::setDTreeRescaleDefines()
{
    if (!mSettingsChanged.dtreeFactorChanged)
        return;
    mSettingsChanged.dtreeFactorChanged = false;

    mSDTreeUpdatePasses.pRescaleDTreePass->addDefine("K_SCALE_FACTOR", std::to_string(mRenderSettings.dtreeFactor), true);
}

void PPGPass::setSTreeRescaleDefines()
{
    if (!mSettingsChanged.streeFactorChanged)
        return;
    mSettingsChanged.streeFactorChanged = false;

    mSDTreeUpdatePasses.pRescaleSTreeStatWeightPass->addDefine("K_SCALE_FACTOR", std::to_string(mRenderSettings.streeFactor), true);
}

void PPGPass::setBuildDTreeDefines()
{
    if (!mSettingsChanged.dtreeTresholdChanged)
        return;
    mSettingsChanged.dtreeTresholdChanged = false;

    mSDTreeUpdatePasses.pUpdateDTreeStructurePass->addDefine("G_SPLIT_FACTOR", std::to_string(mRenderSettings.dtreeSplitTreshold));
    mSDTreeUpdatePasses.pUpdateDTreeStructurePass->addDefine("G_MERGE_FACTOR", std::to_string(mRenderSettings.dtreeMergeTreshold));
    mSDTreeUpdatePasses.pUpdateDTreeStructurePass->addDefine("G_MAX_DTREE_DEPTH", std::to_string(mRenderSettings.dtreeMaxDepth));
    mSDTreeUpdatePasses.pUpdateDTreeStructurePass->addDefine("K_DO_MERGE", bool_to_str(mRenderSettings.dtreeDoMerge), true);
}

void PPGPass::setBuildSTreeDefines()
{
    if (!mSettingsChanged.streeTresholdChanged)
        return;
    mSettingsChanged.streeTresholdChanged = false;

    mSDTreeUpdatePasses.pUpdateSTreeStructurePass->addDefine("G_SPLIT_TRESHOLD", std::to_string(mRenderSettings.streeSplitTreshold));
    mSDTreeUpdatePasses.pUpdateSTreeStructurePass->addDefine("G_MERGE_TRESHOLD", std::to_string(mRenderSettings.streeMergetreshold));
    mSDTreeUpdatePasses.pUpdateSTreeStructurePass->addDefine("K_DO_MERGE", bool_to_str(mRenderSettings.streeDoMerge), true);
}

template<typename T>
std::vector<T> convertVector(std::vector<uint8_t>& original)
{
    const size_t sizeOfSingleElem = sizeof(T);
    const size_t sizeOfFinalVec = original.size() / sizeOfSingleElem;
    const size_t sizeInBytes = sizeOfFinalVec * sizeOfSingleElem;

    T* newData = new T[sizeOfFinalVec];

    std::memcpy(newData, original.data(), sizeInBytes);

    auto res = std::vector<T>(newData, newData + sizeOfFinalVec);

    delete[] newData;

    return res;
}

template<typename T>
std::vector<T> convertVector(std::vector<uint8_t>&& original)
{
    const size_t sizeOfSingleElem = sizeof(T);
    const size_t sizeOfFinalVec = original.size() / sizeOfSingleElem;
    const size_t sizeInBytes = sizeOfFinalVec * sizeOfSingleElem;

    T* newData = new T[sizeOfFinalVec];

    std::memcpy(newData, original.data(), sizeInBytes);

    auto res = std::vector<T>(newData, newData + sizeOfFinalVec);

    delete[] newData;

    return res;
}

void PPGPass::updateTreeTextures(RenderContext* pRenderContext)
{
    if (!mpScene)
        return;
    // remake all textures and reset STree as well
    mSDTreeUpdatePasses.pBlitDTreePass->addDefine("G_IS_NEW_TEX", "1", true);

    uint sTreeWidth = kSTreeTexWidth;
    uint sTreeHeight = kSTreeTexHeight;
    uint dTreeWidth = kDTreeTexWidth;
    uint dTreeHeight = kDTreeTexHeight;


    if (mTreeTextures.pSTreeTex == nullptr)
    {
        mTreeTextures.pSTreeTex = Texture::create2D(sTreeWidth, sTreeHeight,
            kInternalChannels.kSTreeData.mFormat, 1, 1, nullptr,
            kInternalChannels.kSTreeData.mBindflags);
        mTreeTextures.pSTreeStatWeightTex = Texture::create2D(sTreeWidth, sTreeHeight,
            kInternalChannels.kSTreeStatWeight.mFormat, 1, 1, nullptr,
            kInternalChannels.kSTreeStatWeight.mBindflags);
        mTreeTextures.pSTreeMetaDataTex = Texture::create1D(2,
            kInternalChannels.kSTreeMetaData.mFormat, 1, 1, nullptr,
            kInternalChannels.kSTreeMetaData.mBindflags);

        mTreeTextures.pDTreeSumsTex = Texture::create2D(dTreeWidth, kDTreeTexHeight,
            kInternalChannels.kDTreeSums.mFormat, 1, 1, nullptr,
            kInternalChannels.kDTreeSums.mBindflags);
        mTreeTextures.pDTreeChildrenTex = Texture::create2D(dTreeWidth, kDTreeTexHeight,
            kInternalChannels.kDTreeChildren.mFormat, 1, 1, nullptr,
            kInternalChannels.kDTreeChildren.mBindflags);
        mTreeTextures.pDTreeParentTex = Texture::create2D((dTreeWidth + 1) / 2, kDTreeTexHeight,
            kInternalChannels.kDTreeParent.mFormat, 1, 1, nullptr,
            kInternalChannels.kDTreeParent.mBindflags);
        mTreeTextures.pDTreeSizeTex = Texture::create1D(kDTreeTexHeight,
            kInternalChannels.kDTreeSize.mFormat, 1, 1, nullptr,
            kInternalChannels.kDTreeSize.mBindflags);

        mTreeTextures.pDTreeStatisticalWeightTex = Texture::create1D(kDTreeTexHeight,
            kInternalChannels.kDTreeStatisticalWeight.mFormat, 1, 1, nullptr,
            kInternalChannels.kDTreeStatisticalWeight.mBindflags);
        mTreeTextures.pDTreeFreedNodes = Texture::create1D(kDTreeTexHeight,
            ResourceFormat::R32Uint, 1, 1, nullptr,
            kDefaultBindFlags);
        mTreeTextures.pDTreeEditDataTex = Texture::create1D(3,
            kInternalChannels.kDTreeEditData.mFormat, 1, 1, nullptr,
            kInternalChannels.kDTreeEditData.mBindflags);
    }

    auto& sPass = mSDTreeUpdatePasses.pBlitSTreePass;
    sPass["BlitBuf"]["gNewTexSize"] = uint2(sTreeWidth, sTreeHeight);

    sPass[kInternalChannels.kSTreeMetaData.mShaderName] = mTreeTextures.pSTreeMetaDataTex;
    sPass[kInternalChannels.kSTreeData.mShaderName] = mTreeTextures.pSTreeTex;
    sPass[kInternalChannels.kSTreeStatWeight.mShaderName] = mTreeTextures.pSTreeStatWeightTex;

    sPass->execute(pRenderContext, uint3(sTreeWidth, sTreeHeight, 1));

    auto& pass = mSDTreeUpdatePasses.pBlitDTreePass;
    pass["BlitBuf"]["gOldRelevantTexSize"] = uint2(0, 0);
    pass["BlitBuf"]["gNewTexSize"] = uint2(dTreeWidth, dTreeHeight);
    pass[kInternalChannels.kDTreeSums.mShaderName] = mTreeTextures.pDTreeSumsTex;
    pass[kInternalChannels.kDTreeChildren.mShaderName] = mTreeTextures.pDTreeChildrenTex;
    pass[kInternalChannels.kDTreeParent.mShaderName] = mTreeTextures.pDTreeParentTex;
    pass[kInternalChannels.kDTreeSize.mShaderName] = mTreeTextures.pDTreeSizeTex;

    pass->execute(pRenderContext, uint3(dTreeWidth, dTreeHeight, 1));

    mSDTreeUpdatePasses.pBlitDTreePass->addDefine("G_IS_NEW_TEX", "0", true);
    return;
}

void PPGPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!beginFrame(pRenderContext, renderData))
        return;

    //std::cout << mpTree->getEstimatedAmountOfDTrees() << std::endl;
    //std::cout << mpTree->getEstimatedSTreeSize() << std::endl;

    auto pipelineRefreshed = renderData.getDictionary().getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
    if (mTreeTextures.pSTreeTex == nullptr || (mRenderSettings.resetSDTreeOnLightingChanges && pipelineRefreshed != RenderPassRefreshFlags::None))
    {
        updateTreeTextures(pRenderContext);
    }


    uint2 screenSize = renderData.getDefaultTextureDims();
    if (mSampleResultTextures.pPosTex == nullptr ||
        mSampleResultTextures.pPosTex->getWidth() != screenSize.x ||
        mSampleResultTextures.pPosTex->getHeight() != screenSize.y)
    {
        mSampleResultTextures.pPosTex = Texture::create2D(screenSize.x, screenSize.y,
            ResourceFormat::RGBA32Float, 1, 1, nullptr, kDefaultBindFlags); // was kDefaultBindFlags
        mSampleResultTextures.pRadianceTex = Texture::create2D(screenSize.x, screenSize.y,
            ResourceFormat::RGBA32Float, 1, 1, nullptr, kDefaultBindFlags);
        mSampleResultTextures.pDirPdfTex = Texture::create2D(screenSize.x, screenSize.y,
            ResourceFormat::RGBA32Float, 1, 1, nullptr, kDefaultBindFlags);
    }

    setTracingDefines();
    setStaticParams(mpPPGProg.get());

    if (mUseEmissiveSampler)
    {
        assert(mpEmissiveSampler);
        if (mpPPGProg->addDefines(mpEmissiveSampler->getDefines()))
            mpPPGVars = nullptr;
    }

    if (mpPPGVars == nullptr || mOptionsChanged)
    {
        mpPPGProg->addDefines(mpSampleGenerator->getDefines());
        mpPPGVars = RtProgramVars::create(mpPPGProg, mpScene); // this inits compilation

        bool success = mpSampleGenerator->setShaderData(mpPPGVars->getRootVar());
        if (!success)
        {
            throw std::exception("Failed to bin sample generator");
        }

        // Create parameter block for shared data
        ProgramReflection::SharedConstPtr pReflection = mpPPGProg->getReflector();
        ParameterBlockReflection::SharedConstPtr pParamBlockReflection = pReflection->getParameterBlock("gData");
        assert(pParamBlockReflection);
        mpPPGParamBlock = ParameterBlock::create(pParamBlockReflection);
        assert(mpPPGParamBlock);

        // Bind all static resources to parameterblock now
        // Bind environment map if present
        if (mpEnvMapSampler)
            mpEnvMapSampler->setShaderData(mpPPGParamBlock["envMapSampler"]);

        // Bind the parameterblock to global shader variables (map to GPU memory)
        mpPPGVars->setParameterBlock("gData", mpPPGParamBlock);
    }

    auto& pParamBlock = mpPPGParamBlock;
    assert(pParamBlock);

    // Upload configuration struct
    pParamBlock["params"].setBlob(mSharedParams);

    // Bind emissive light sampler
    if (mUseEmissiveSampler)
    {
        assert(mpEmissiveSampler);
        bool success = mpEmissiveSampler->setShaderData(pParamBlock["emissiveLightSampler"]);
        if (!success)
            throw std::exception("Failed to bind emissive light sampler");
    }

    auto bindResources = [&](const ChannelDesc& desc, const RtProgramVars::SharedPtr& vars)
    {
        if (!desc.texname.empty())
        {
            vars[desc.texname] = renderData[desc.name]->asTexture();
        }
    };
    for (auto& channel : kShadingInputChannels) bindResources(channel, mpPPGVars);
    for (auto& channel : kOutputChannels) bindResources(channel, mpPPGVars);
    //for (auto& channel : kInternalChannels) bindResources(channel, mpPPGVars);

    // Bind SD-Tree textures
    mpPPGVars["gSTreeData"] = mTreeTextures.pSTreeTex;
    mpPPGVars["gDTreeSums"] = mTreeTextures.pDTreeSumsTex;
    mpPPGVars["gDTreeChildren"] = mTreeTextures.pDTreeChildrenTex;

    // Bind internal sample result textures
    mpPPGVars["gSamplePos"] = mSampleResultTextures.pPosTex;
    mpPPGVars["gSampleRadiance"] = mSampleResultTextures.pRadianceTex;
    mpPPGVars["gSamplePdf"] = mSampleResultTextures.pDirPdfTex;

    mpPPGVars["PPGBuf"]["gAmountOfSTreeNodesPerRow"] = mTreeTextures.pSTreeTex->getWidth();
    mpPPGVars["PPGBuf"]["gFrameIndex"] = mSharedParams.frameCount;

    mpPPGVars["PPGBuf"]["gSceneMin"] = mAABB.mMin;
    mpPPGVars["PPGBuf"]["gSceneMax"] = mAABB.mMax;

    // Raytrace the scene
    mpScene->raytrace(pRenderContext, mpPPGProg.get(), mpPPGVars, uint3(screenSize, 1));

    resetStatisticalWeight(pRenderContext);

    setDTreeRescaleDefines();
    rescaleTree(pRenderContext);

    splatIntoTree(pRenderContext, screenSize);

    propagateTreeSums(pRenderContext);

    updateDTreeStructure(pRenderContext);

    updateSTreeStatWeight(pRenderContext);

    updateSTreeStructure(pRenderContext);

    endFrame(pRenderContext, renderData);
}

void PPGPass::resetStatisticalWeight(RenderContext* pRenderContext)
{
    auto& pass = mSDTreeUpdatePasses.pResetStatisticalWeightPass;

    pass["ResetBuf"]["gTexSize"] = mTreeTextures.pDTreeStatisticalWeightTex->getWidth();
    pass[kInternalChannels.kDTreeStatisticalWeight.mShaderName] = mTreeTextures.pDTreeStatisticalWeightTex;

    pass->execute(pRenderContext, uint3(mTreeTextures.pDTreeStatisticalWeightTex->getWidth(), 1, 1));
}

void PPGPass::rescaleTree(RenderContext* pRenderContext)
{
    auto& pass = mSDTreeUpdatePasses.pRescaleDTreePass;

    uint2 texSize;
    texSize.x = mTreeTextures.pDTreeSumsTex->getWidth();
    texSize.y = mTreeTextures.pDTreeSumsTex->getHeight();

    pass[kInternalChannels.kDTreeSums.mShaderName] = mTreeTextures.pDTreeSumsTex;
    pass[kInternalChannels.kSTreeMetaData.mShaderName] = mTreeTextures.pSTreeMetaDataTex;

    pass->execute(pRenderContext, uint3(texSize, 1));
}

void PPGPass::splatIntoTree(RenderContext* pRenderContext, uint2 screenSize)
{
    auto& pass = mSDTreeUpdatePasses.pSplatIntoSDTreePass;

    pass["SplatBuf"]["gScreenSize"] = screenSize;
    pass["SplatBuf"]["gAmountOfSTreeNodesPerRow"] = mTreeTextures.pSTreeTex->getWidth();
    pass["SplatBuf"]["gSceneMin"] = mAABB.mMin;
    pass["SplatBuf"]["gSceneMax"] = mAABB.mMax;

    pass[kInternalChannels.kSTreeData.mShaderName] = mTreeTextures.pSTreeTex;
    pass[kInternalChannels.kDTreeSums.mShaderName] = mTreeTextures.pDTreeSumsTex;
    pass[kInternalChannels.kDTreeChildren.mShaderName] = mTreeTextures.pDTreeChildrenTex;
    pass[kInternalChannels.kDTreeStatisticalWeight.mShaderName] = mTreeTextures.pDTreeStatisticalWeightTex;

    pass[kInternalChannels.kSamplePos.mShaderName] = mSampleResultTextures.pPosTex;
    pass[kInternalChannels.kSampleRadiance.mShaderName] = mSampleResultTextures.pRadianceTex;
    pass[kInternalChannels.kSampleDirPdf.mShaderName] = mSampleResultTextures.pDirPdfTex;

    pass->execute(pRenderContext, uint3(screenSize, 1));
}

void PPGPass::propagateTreeSums(RenderContext* pRenderContext)
{
    auto& pass = mSDTreeUpdatePasses.pPropagateDTreeSumsPass;

    pass[kInternalChannels.kDTreeSums.mShaderName] = mTreeTextures.pDTreeSumsTex;
    pass[kInternalChannels.kDTreeChildren.mShaderName] = mTreeTextures.pDTreeChildrenTex;
    pass[kInternalChannels.kDTreeParent.mShaderName] = mTreeTextures.pDTreeParentTex;

    pass[kInternalChannels.kDTreeSize.mShaderName] = mTreeTextures.pDTreeSizeTex;
    pass[kInternalChannels.kSTreeMetaData.mShaderName] = mTreeTextures.pSTreeMetaDataTex;

    uint2 texSize;
    texSize.x = mTreeTextures.pDTreeSumsTex->getWidth();
    texSize.y = mTreeTextures.pDTreeSumsTex->getHeight();

    pass->execute(pRenderContext, uint3(texSize, 1));
}

void PPGPass::updateSTreeStatWeight(RenderContext* pRenderContext)
{
    uint2 texSize;

    setSTreeRescaleDefines();
    texSize.x = mTreeTextures.pSTreeStatWeightTex->getWidth();
    texSize.y = mTreeTextures.pSTreeStatWeightTex->getHeight();
    
    auto& rescalePass = mSDTreeUpdatePasses.pRescaleSTreeStatWeightPass;
    rescalePass[kInternalChannels.kSTreeMetaData.mShaderName] = mTreeTextures.pSTreeMetaDataTex;
    rescalePass[kInternalChannels.kSTreeStatWeight.mShaderName] = mTreeTextures.pSTreeStatWeightTex;

    rescalePass->execute(pRenderContext, uint3(texSize, 1));

    auto& updatePass = mSDTreeUpdatePasses.pUpdateSTreeStatWeightPass;
    updatePass[kInternalChannels.kSTreeMetaData.mShaderName] = mTreeTextures.pSTreeMetaDataTex;
    updatePass[kInternalChannels.kSTreeData.mShaderName] = mTreeTextures.pSTreeTex;
    updatePass[kInternalChannels.kSTreeStatWeight.mShaderName] = mTreeTextures.pSTreeStatWeightTex;

    updatePass[kInternalChannels.kDTreeStatisticalWeight.mShaderName] = mTreeTextures.pDTreeStatisticalWeightTex;

    updatePass->execute(pRenderContext, uint3(texSize, 1));
}

void PPGPass::updateSTreeStructure(RenderContext* pRenderContext)
{
    auto& buildPass = mSDTreeUpdatePasses.pUpdateSTreeStructurePass;

    if (buildPass->getVars() == nullptr || mSettingsChanged.streeTresholdChanged)
    {
        for (auto& def : mpSampleGenerator->getDefines())
        {
            buildPass->addDefine(def.first, def.second, false);
        }
        setBuildSTreeDefines();

        bool success = mpSampleGenerator->setShaderData(buildPass->getRootVar());
        if (!success)
        {
            throw std::exception("Failed to bind sample generator");
        }
    }

    buildPass["BuildBuf"]["gFrameCount"] = mSharedParams.frameCount++;

    buildPass[kInternalChannels.kSTreeMetaData.mShaderName] = mTreeTextures.pSTreeMetaDataTex;
    buildPass[kInternalChannels.kSTreeData.mShaderName] = mTreeTextures.pSTreeTex;
    buildPass[kInternalChannels.kSTreeStatWeight.mShaderName] = mTreeTextures.pSTreeStatWeightTex;
    buildPass[kInternalChannels.kDTreeEditData.mShaderName] = mTreeTextures.pDTreeEditDataTex;

    buildPass->execute(pRenderContext, uint3(2, 1, 1));

    auto& compressPass = mSDTreeUpdatePasses.pCompressSTreePass;
    compressPass[kInternalChannels.kSTreeMetaData.mShaderName] = mTreeTextures.pSTreeMetaDataTex;
    compressPass[kInternalChannels.kSTreeData.mShaderName] = mTreeTextures.pSTreeTex;
    compressPass[kInternalChannels.kSTreeStatWeight.mShaderName] = mTreeTextures.pSTreeStatWeightTex;
    compressPass[kInternalChannels.kDTreeEditData.mShaderName] = mTreeTextures.pDTreeEditDataTex;

    uint2 texSize;

    texSize.x = mTreeTextures.pSTreeTex->getWidth();
    texSize.y = mTreeTextures.pSTreeTex->getHeight();

    compressPass->execute(pRenderContext, uint3(texSize, 1));

    auto& dTreeUpdatePass = mSDTreeUpdatePasses.pCopyDTreesPass;
    dTreeUpdatePass[kInternalChannels.kDTreeSums.mShaderName] = mTreeTextures.pDTreeSumsTex;
    dTreeUpdatePass[kInternalChannels.kDTreeChildren.mShaderName] = mTreeTextures.pDTreeChildrenTex;
    dTreeUpdatePass[kInternalChannels.kDTreeParent.mShaderName] = mTreeTextures.pDTreeParentTex;
    dTreeUpdatePass[kInternalChannels.kDTreeSize.mShaderName] = mTreeTextures.pDTreeSizeTex;
    dTreeUpdatePass[kInternalChannels.kSTreeMetaData.mShaderName] = mTreeTextures.pSTreeMetaDataTex;

    texSize.x = mTreeTextures.pDTreeSumsTex->getWidth();
    texSize.y = mTreeTextures.pDTreeSumsTex->getHeight();

    dTreeUpdatePass->execute(pRenderContext, uint3(texSize, 1));
}

void PPGPass::updateDTreeStructure(RenderContext* pRenderContext)
{
    auto& resetPass = mSDTreeUpdatePasses.pResetFreedNodesPass;

    resetPass["ResetBuf"]["gTexSize"] = mTreeTextures.pDTreeFreedNodes->getWidth();
    resetPass["gFreedNodes"] = mTreeTextures.pDTreeFreedNodes;

    resetPass->execute(pRenderContext, uint3(mTreeTextures.pDTreeFreedNodes->getWidth(), 1, 1));

    auto& buildPass = mSDTreeUpdatePasses.pUpdateDTreeStructurePass;

    if (buildPass->getVars() == nullptr || mSettingsChanged.dtreeTresholdChanged)
    {
        for (auto& def : mpSampleGenerator->getDefines())
        {
            buildPass->addDefine(def.first, def.second, false);
        }

        setBuildDTreeDefines();

        bool success = mpSampleGenerator->setShaderData(buildPass->getRootVar());
        if (!success)
        {
            throw std::exception("Failed to bin sample generator");
        }
    }

    buildPass["BuildBuf"]["gFrameIndex"] = mSharedParams.frameCount++;

    buildPass[kInternalChannels.kDTreeSums.mShaderName] = mTreeTextures.pDTreeSumsTex;
    buildPass[kInternalChannels.kDTreeChildren.mShaderName] = mTreeTextures.pDTreeChildrenTex;
    buildPass[kInternalChannels.kDTreeParent.mShaderName] = mTreeTextures.pDTreeParentTex;

    buildPass[kInternalChannels.kDTreeSize.mShaderName] = mTreeTextures.pDTreeSizeTex;
    buildPass[kInternalChannels.kSTreeMetaData.mShaderName] = mTreeTextures.pSTreeMetaDataTex;
    buildPass["gFreedNodes"] = mTreeTextures.pDTreeFreedNodes;

    buildPass->execute(pRenderContext, uint3(mTreeTextures.pDTreeSumsTex->getHeight(), 1, 1));

    uint2 texSize;
    texSize.x = mTreeTextures.pDTreeSumsTex->getWidth();
    texSize.y = mTreeTextures.pDTreeSumsTex->getHeight();

    auto& dTreeCompressPass = mSDTreeUpdatePasses.pCompressDTreePass;

    dTreeCompressPass[kInternalChannels.kDTreeSums.mShaderName] = mTreeTextures.pDTreeSumsTex;
    dTreeCompressPass[kInternalChannels.kDTreeChildren.mShaderName] = mTreeTextures.pDTreeChildrenTex;
    dTreeCompressPass[kInternalChannels.kDTreeParent.mShaderName] = mTreeTextures.pDTreeParentTex;

    dTreeCompressPass[kInternalChannels.kDTreeSize.mShaderName] = mTreeTextures.pDTreeSizeTex;
    dTreeCompressPass["gFreedNodes"] = mTreeTextures.pDTreeFreedNodes;
    dTreeCompressPass[kInternalChannels.kSTreeMetaData.mShaderName] = mTreeTextures.pSTreeMetaDataTex;

    //dTreeCompressPass->execute(pRenderContext, uint3(texSize, 1));
}

void PPGPass::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    PathTracer::setScene(pRenderContext, pScene);

    mTreeTextures.pSTreeTex = nullptr; // reset one texture so all are recreated

    mpPPGVars = nullptr;

    mpPPGProg->addDefines(pScene->getSceneDefines());

    const auto& kaas = pScene->getSceneBounds();
    std::cout << "Scene min: " << kaas.minPoint.x << ", " << kaas.minPoint.y << ", " << kaas.minPoint.z << std::endl;
    std::cout << "Scene max: " << kaas.maxPoint.x << ", " << kaas.maxPoint.y << ", " << kaas.maxPoint.z << std::endl;

    const auto& aabb_falcor = pScene->getSceneBounds();
    float3 delta = aabb_falcor.extent() * 0.1f;
    mAABB = MyAABB(aabb_falcor.minPoint - delta, aabb_falcor.maxPoint + delta);
}

void PPGPass::renderUI(Gui::Widgets& widget)
{
    widget.checkbox("Reset acceleration structures when lighting options change", mRenderSettings.resetSDTreeOnLightingChanges);
    auto tracingGroup = widget.group("Ray Tracing Settings");
    mSettingsChanged.ppgSettingsChanged |= tracingGroup.checkbox("Use Next Event Estimation", mRenderSettings.evalDirectLight);
    mSettingsChanged.ppgSettingsChanged |= tracingGroup.checkbox("Include first direct light bounce", mRenderSettings.includeFirstDirectBounce);
    mSettingsChanged.ppgSettingsChanged |= tracingGroup.var("Amount of RIS candidate samples", mRenderSettings.amountOfRISSamples, 1u, 32u);
    mSettingsChanged.ppgSettingsChanged |= tracingGroup.var("Max amount of bounces", mRenderSettings.maxBounces, 1u, 32u);
    tracingGroup.release();

    auto streeGroup = widget.group("STree Settings");
    mSettingsChanged.streeFactorChanged |= streeGroup.var("STree rescale factor", mRenderSettings.streeFactor, 0.f, 1.f);
    mSettingsChanged.streeTresholdChanged |= streeGroup.var("Stree split treshold", mRenderSettings.streeSplitTreshold, 1000.f, 1000000.f);
    mSettingsChanged.streeTresholdChanged |= streeGroup.checkbox("Merge STree nodes", mRenderSettings.streeDoMerge);
    if (mRenderSettings.streeDoMerge)
        mSettingsChanged.streeTresholdChanged |= streeGroup.var("STree merge treshold", mRenderSettings.streeMergetreshold, 1000.f, 1000000.f);
    streeGroup.release();

    auto dtreeGroup = widget.group("DTree Settings");
    mSettingsChanged.dtreeFactorChanged |= dtreeGroup.var("DTree rescale factor", mRenderSettings.dtreeFactor, 0.f, 1.f);
    mSettingsChanged.dtreeTresholdChanged |= dtreeGroup.var("Max dtree depth", mRenderSettings.dtreeMaxDepth, 2u, 40u);
    mSettingsChanged.dtreeTresholdChanged |= dtreeGroup.var("DTree split treshold", mRenderSettings.dtreeSplitTreshold, 0.f, 1.f);
    mSettingsChanged.dtreeTresholdChanged |= dtreeGroup.checkbox("Merge DTree nodes", mRenderSettings.dtreeDoMerge);
    if (mRenderSettings.dtreeDoMerge)
        mSettingsChanged.dtreeTresholdChanged |= dtreeGroup.var("DTree merge treshold", mRenderSettings.dtreeMergeTreshold, 0.f, 1.f);
    dtreeGroup.release();
}
