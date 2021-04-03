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
#include "ReSTIRPass.h"

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("ReSTIRPass", ReSTIRPass::description, ReSTIRPass::create);
}

namespace
{
    const uint kMaxRecursionDepth = 1;
    const uint kMaxPayloadSize = 4;
    const uint kMaxAttributeSize = 16;

    const char kParameterBlockName[] = "gData";
    const char kPathTracerParamsMemberName[] = "params";
    const char kEnvMapSamplerMemberName[] = "envMapSampler";
    const char kEmissiveLightSamplerMemberName[] = "emissiveLightSampler";

    const char kTemporalShName[] = "shAccumulatedReservoirData";

    const ChannelList kRGBAInputChannels =
    {
        {"position",                "shWorldPos",       "World-space position (xyz) and foreground flag (w)"},
        {"shadingNormal",           "shShadingNormal",  "World-space shading normal (xyz)"},
        {"gometricNormal",          "shGeoNormal",      "World-space geometrical normal (xyz)"},
        {"diffColorOp",             "shDiffColorOp",    "Diffuse color (xyz) and opacity (w)"},
        {"specRoughness",           "shSpecRoughness",  "Specular roughness (xyz)"},
        {"emissiveColor",           "shEmissiveColor",  "Emissive color (xyz)"},
        {"extraMatParams",          "shMatParams",      "Extra material parameters"},
        {"viewVec",                 "shViewVec",        "World space view vector"}
    };

    const ChannelDesc kNormals =
    {
        "normals",                  "shNormals",        "Geometric normals"
    };
    //const ChannelDesc kDepthBuffer =
    //{
    //    "depth",                    "shDepth",          "Depth buffer"
    //};

    const ChannelDesc kMotionVecs =
    {
        "motionVecs",               "shMotionVectors",  "Motion vectors"
    };

    const ChannelDesc kReservoirInput1 =
    {
        "packedInputResData1",      "shPackedInputReservoirData1", "Part 1 of the packed input reservoir data"
    };
    const ChannelDesc kReservoirInput2 =
    {
        "packedInputResData2",      "shPackedInputReservoirData2", "Part 2 of the packed input reservoir data"
    };
    const ChannelDesc kReservoirInput3 =
    {
        "packedInputResData3",      "shPackedInputReservoirData3", "Part 3 of the packed input reservoir data"
    };

    const ChannelDesc kReservoirOutput1 =
    {
        "packedOutputResData1",     "shPackedOutputReservoirData1", "Part 1 of the packed output reservoir data"
    };
    const ChannelDesc kReservoirOutput2 =
    {
        "packedOutputResData2",     "shPackedOutputReservoirData2", "Part 2 of the packed output reservoir data"
    };
    const ChannelDesc kReservoirOutput3 =
    {
        "packedOutputResData3",     "shPackedOutputReservoirData3", "Part 3 of the packed output reservoir data"
    };

    const ChannelDesc kShadowRayChannel =
    {
        "shadowRayOrigin",          "shShadowRayOrigin",            "Origin of the shadow ray to be traced in a later shader" // meeibie useless
    };

    const ChannelList kRGBAOutputChannels =
    {
        {"color",                   "shOutputColor",                "The output color"}
    };
}

const char ReSTIRPass::description[] = "Reservoir based SpatioTemporal Reservoir Resampling for dynamic direct lighting";

ReSTIRPass::ReSTIRPass(const Dictionary& dict) : PathTracer(dict, kRGBAOutputChannels)
{
    // samping pass initialization
    RtProgram::Desc desc;
    desc.addShaderLibrary("RenderPasses/ReSTIRPass/ReservoirSampler.slang");
    desc.addHitGroup(0, "scatterClosestHit", "scatterAnyHit").addMiss(0, "scatterMiss");
    desc.addHitGroup(1, "", "shadowRayHit").addMiss(1, "shadowRayMiss");
    desc.setRayGen("main");
    desc.setMaxTraceRecursionDepth(kMaxRecursionDepth);
    mGenerationProgram.pProgram = RtProgram::create(desc, kMaxPayloadSize, kMaxAttributeSize);
    // Temporal resamping initialization
    Program::DefineList temporalDl;
    temporalDl.add(mpSampleGenerator->getDefines());
    mpTemporalResamplePass = ComputePass::create("RenderPasses/ReSTIRPass/TemporalResampler.slang", "main", temporalDl);
    // Spatial resampling initialization
    Program::DefineList spatialDl;
    spatialDl.add("AMOUNT_OF_SAMPLES", std::to_string(mAmountOfSpatialSamples));
    spatialDl.add(mpSampleGenerator->getDefines());
    mpSpatialResamplePass = ComputePass::create("RenderPasses/ReSTIRPass/SpatialResampler.slang", "main", spatialDl, false);
    // Final tracing initialization
    RtProgram::Desc description;
    description.addShaderLibrary("RenderPasses/ReSTIRPass/ReservoirTracing.slang");
    description.addHitGroup(0, "scatterClosestHit", "scatterAnyHit").addMiss(0, "scatterMiss");
    description.addHitGroup(1, "", "shadowRayHit").addMiss(1, "shadowRayMiss");
    description.setRayGen("main");
    description.setMaxTraceRecursionDepth(kMaxRecursionDepth);
    mTracingProgram.pProgram = RtProgram::create(description, kMaxPayloadSize, kMaxAttributeSize);

    mSharedParams.useMIS = 0;
    mSelectedEmissiveSampler = EmissiveLightSamplerType::Uniform;
}

ReSTIRPass::SharedPtr ReSTIRPass::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new ReSTIRPass(dict));
    return pPass;
}

RenderPassReflection ReSTIRPass::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    addRenderPassInputs(reflector, kRGBAInputChannels);
    addRenderPassOutputs(reflector, kRGBAOutputChannels);
    Resource::BindFlags reservoirBindFlags =
        Resource::BindFlags::RenderTarget |
        Resource::BindFlags::ShaderResource |
        Resource::BindFlags::UnorderedAccess;
    Resource::BindFlags kaas =
        Resource::BindFlags::ShaderResource |
        Resource::BindFlags::UnorderedAccess;
    // Internal reservoir input channels
    reflector.addInternal(kReservoirInput1.name, kReservoirInput1.desc)
        .format(ResourceFormat::RGBA32Float)
        .bindFlags(reservoirBindFlags);
    reflector.addInternal(kReservoirInput2.name, kReservoirInput2.desc)
        .format(ResourceFormat::RGBA32Float)
        .bindFlags(reservoirBindFlags);
    reflector.addInternal(kReservoirInput3.name, kReservoirInput3.desc)
        .format(ResourceFormat::R32Uint)
        .bindFlags(reservoirBindFlags);
    /*
    reflector.addInput(kReservoirInput1.name, kReservoirInput1.desc)
        .format(ResourceFormat::RGBA32Float)
        .bindFlags(reservoirBindFlags);
    reflector.addInput(kReservoirInput2.name, kReservoirInput2.desc)
        .format(ResourceFormat::RGBA32Float)
        .bindFlags(reservoirBindFlags);
    reflector.addInput(kReservoirInput3.name, kReservoirInput3.desc)
        .format(ResourceFormat::R32Uint)
        .bindFlags(reservoirBindFlags);
    */
    // Internal reservoir output channels
    reflector.addInternal(kReservoirOutput1.name, kReservoirOutput1.desc)
        .format(ResourceFormat::RGBA32Float)
        .bindFlags(kaas);
    reflector.addInternal(kReservoirOutput2.name, kReservoirOutput2.desc)
        .format(ResourceFormat::RGBA32Float)
        .bindFlags(kaas);
    reflector.addInternal(kReservoirOutput3.name, kReservoirOutput3.desc)
        .format(ResourceFormat::R32Uint)
        .bindFlags(kaas);
    // Shadow ray channel
    reflector.addInternal(kShadowRayChannel.name, kShadowRayChannel.desc)
        .format(ResourceFormat::RGBA32Float)
        .bindFlags(kaas);
    // Inputs unique to second pass
    reflector.addInput(kNormals.name, kNormals.desc);
    //reflector.addInput(kDepthBuffer.name, kDepthBuffer.desc).format(ResourceFormat::D32Float);
    // Inputs unique to thrid pass
    reflector.addInput(kMotionVecs.name, kMotionVecs.desc).format(ResourceFormat::RG32Float);
    return reflector;
}

void ReSTIRPass::setupMeasurements()
{
    if (mCurrentFrameIndex == mMaxFrames && mpScene != nullptr)
    {
        mOptionsChanged = true;
        mCurrentFrameIndex = 0;
        mMaxFrames++;
    }
    else if (mpScene != nullptr)
    {
        mCurrentFrameIndex++;
    }
}

void ReSTIRPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // renderData holds the requested resources
    // auto& pTexture = renderData["src"]->asTexture();
    if (mAutoResetPipeline)
    {
        setupMeasurements();
    }
    beginFrame(pRenderContext, renderData);
    if (mpScene == nullptr)
    {
        for (auto& ch : kRGBAOutputChannels)
        {
            Texture::SharedPtr pDest = renderData[ch.name]->asTexture();
            if (pDest)
                pRenderContext->clearTexture(pDest.get());
        }
        return;
    }
    const uint2 targetDim = renderData.getDefaultTextureDims();

    executeSamplingPass(pRenderContext, renderData, targetDim);

    // Start of second pass
    if (mDoTemporalResampling)
    {
        executeTemporalPass(pRenderContext, renderData, targetDim);
    }

    // Start of third pass
    if (mAmountOfSpatialPasses > 0)
        executeSpatialPass(pRenderContext, renderData, targetDim);

    // Start of fourth pass
    executeTracingPass(pRenderContext, renderData, targetDim);

    endFrame(pRenderContext, renderData);
}

void ReSTIRPass::executeSamplingPass(RenderContext* pRenderContext, const RenderData& renderData, uint2 targetDim)
{
    setStaticParams(mGenerationProgram.pProgram.get());

    if (mUseEmissiveSampler)
    {
        assert(mpEmissiveSampler);
        if (mpEmissiveSampler->prepareProgram(mGenerationProgram.pProgram.get()))
            mGenerationProgram.pVars = nullptr;
    }

    assert(mpScene);
    assert(mGenerationProgram.pProgram);

    // Prepare shader variables
    if (mGenerationProgram.pVars == nullptr)
        initStaticSamplingVars();
    initDynamicSamplingVars();
    assert(mGenerationProgram.pVars);

    // Set constants at this point
    // Bind IO buffers of first pass
    const auto& pGlobalVars = mGenerationProgram.pVars;
    auto bindRT = [&](const ChannelDesc& desc, const RtProgramVars::SharedPtr& vars)
    {
        if (!desc.texname.empty())
        {
            vars[desc.texname] = renderData[desc.name]->asTexture();
        }
    };
    for (auto &channel : kRGBAInputChannels) bindRT(channel, pGlobalVars);
    pGlobalVars[kReservoirOutput1.texname] = renderData[kReservoirOutput1.name]->asTexture();
    pGlobalVars[kReservoirOutput2.texname] = renderData[kReservoirOutput2.name]->asTexture();
    pGlobalVars[kReservoirOutput3.texname] = renderData[kReservoirOutput3.name]->asTexture();
    pGlobalVars[kShadowRayChannel.texname] = renderData[kShadowRayChannel.name]->asTexture();

    // Render the scene
    assert(targetDim.x > 0 && targetDim.y > 0);

    mpScene->raytrace(pRenderContext, mGenerationProgram.pProgram.get(), mGenerationProgram.pVars, uint3(targetDim, 1));
    mSharedParams.frameCount++;
    // Copy reservoir output to input
    copyReservoirData(pRenderContext, renderData);
}

void ReSTIRPass::executeTemporalPass(RenderContext* pRenderContext, const RenderData& renderData, uint2 targetDim)
{
    bool buffersValid = mpTemporalCache[0] != nullptr;
    if (buffersValid)
    {
        buffersValid &= targetDim.x == mpTemporalCache[0]->asTexture()->getWidth();
        buffersValid &= targetDim.y == mpTemporalCache[0]->asTexture()->getHeight();
    }
    if (!buffersValid || mSceneChanged)
    {
        allocateTemporalBuffers(targetDim);
        mSceneChanged = false;
    }
    // Bind IO buffers of second pass
    mpTemporalResamplePass["TemporalBuffer"]["gFrameCount"] = mSharedParams.frameCount;

    mpTemporalResamplePass[kReservoirInput1.texname] = renderData[kReservoirInput1.name]->asTexture();
    mpTemporalResamplePass[kReservoirInput2.texname] = renderData[kReservoirInput2.name]->asTexture();
    mpTemporalResamplePass[kReservoirInput3.texname] = renderData[kReservoirInput3.name]->asTexture();
    mpTemporalResamplePass[kMotionVecs.texname] = renderData[kMotionVecs.name]->asTexture();

    mpTemporalResamplePass[kTemporalShName + std::string("1")] = mpTemporalCache[0]->asTexture();
    mpTemporalResamplePass[kTemporalShName + std::string("2")] = mpTemporalCache[1]->asTexture();
    mpTemporalResamplePass[kTemporalShName + std::string("3")] = mpTemporalCache[2]->asTexture();

    mpTemporalResamplePass[kReservoirOutput1.texname] = renderData[kReservoirOutput1.name]->asTexture();
    mpTemporalResamplePass[kReservoirOutput2.texname] = renderData[kReservoirOutput2.name]->asTexture();
    mpTemporalResamplePass[kReservoirOutput3.texname] = renderData[kReservoirOutput3.name]->asTexture();

    mpTemporalResamplePass->execute(pRenderContext, uint3(targetDim, 1));
    // Copy output to internal buffers
    pRenderContext->blit(renderData[kReservoirOutput1.name]->asTexture()->getSRV(), mpTemporalCache[0]->getRTV());
    pRenderContext->blit(renderData[kReservoirOutput2.name]->asTexture()->getSRV(), mpTemporalCache[1]->getRTV());
    pRenderContext->blit(renderData[kReservoirOutput3.name]->asTexture()->getSRV(), mpTemporalCache[2]->getRTV());

    mSharedParams.frameCount++;
    // Copy reservoir output to input
    copyReservoirData(pRenderContext, renderData);
}

void ReSTIRPass::executeSpatialPass(RenderContext* pRenderContext, const RenderData& renderData, uint2 targetDim)
{
    // Bind IO buffers of third pass
    mpSpatialResamplePass[kReservoirInput1.texname] = renderData[kReservoirInput1.name]->asTexture();
    mpSpatialResamplePass[kReservoirInput2.texname] = renderData[kReservoirInput2.name]->asTexture();
    mpSpatialResamplePass[kReservoirInput3.texname] = renderData[kReservoirInput3.name]->asTexture();
    //mpSpatialResamplePass[kNormals.texname] = renderData[kNormals.name]->asTexture();
    //mpSpatialResamplePass[kDepthBuffer.texname] = renderData[kDepthBuffer.name]->asTexture();
    mpSpatialResamplePass[kShadowRayChannel.texname] = renderData[kShadowRayChannel.name]->asTexture();

    auto bindCompute = [&](const ChannelDesc& desc, const ComputePass::SharedPtr& computePass)
    {
        if (!desc.texname.empty())
        {
            computePass[desc.texname] = renderData[desc.name]->asTexture();
        }
    };
    for (auto& ch : kRGBAInputChannels) bindCompute(ch, mpSpatialResamplePass);

    mpSpatialResamplePass["SharedBuffer"]["gImageSize"] = targetDim;

    mpSpatialResamplePass[kReservoirOutput1.texname] = renderData[kReservoirOutput1.name]->asTexture();
    mpSpatialResamplePass[kReservoirOutput2.texname] = renderData[kReservoirOutput2.name]->asTexture();
    mpSpatialResamplePass[kReservoirOutput3.texname] = renderData[kReservoirOutput3.name]->asTexture();

    for (uint i = 0; i < mAmountOfSpatialPasses; i++)
    {
        mpSpatialResamplePass["SharedBuffer"]["gFrameCount"] = mSharedParams.frameCount;

        mpSpatialResamplePass->execute(pRenderContext, uint3(targetDim, 1));

        if (i < mAmountOfSpatialPasses - 1)
        {
            copyReservoirData(pRenderContext, renderData);

            mpSpatialResamplePass[kReservoirInput1.texname] = renderData[kReservoirInput1.name]->asTexture();
            mpSpatialResamplePass[kReservoirInput2.texname] = renderData[kReservoirInput2.name]->asTexture();
            mpSpatialResamplePass[kReservoirInput3.texname] = renderData[kReservoirInput3.name]->asTexture();
        }

        mSharedParams.frameCount++;
    }
    // Copy reservoir output to input
    copyReservoirData(pRenderContext, renderData);
}

void ReSTIRPass::executeTracingPass(RenderContext* pRenderContext, const RenderData& renderData, uint2 targetDim)
{
    setStaticParams(mTracingProgram.pProgram.get());
    if (!mTracingProgram.pVars)
        initStaticTracingVars();
    // Bind IO buffers of fourth pass
    auto bindRT = [&](const ChannelDesc& desc, const RtProgramVars::SharedPtr& vars)
    {
        if (!desc.texname.empty())
        {
            vars[desc.texname] = renderData[desc.name]->asTexture();
        }
    };
    auto& pTracingVars = mTracingProgram.pVars;
    for (auto& ch : kRGBAInputChannels) bindRT(ch, pTracingVars);
    pTracingVars[kReservoirInput1.texname] = renderData[kReservoirInput1.name]->asTexture();
    pTracingVars[kReservoirInput2.texname] = renderData[kReservoirInput2.name]->asTexture();
    pTracingVars[kReservoirInput3.texname] = renderData[kReservoirInput3.name]->asTexture();
    pTracingVars[kShadowRayChannel.texname] = renderData[kShadowRayChannel.name]->asTexture();

    for (auto& ch : kRGBAOutputChannels) bindRT(ch, pTracingVars);

    mpScene->raytrace(pRenderContext, mTracingProgram.pProgram.get(), mTracingProgram.pVars, uint3(targetDim, 1));

    mSharedParams.frameCount++;
}

void ReSTIRPass::copyReservoirData(RenderContext* pRenderContext, const RenderData& renderData)
{
    pRenderContext->blit(
        renderData[kReservoirOutput1.name]->asTexture()->getSRV(),
        renderData[kReservoirInput1.name]->asTexture()->getRTV()
    );
    pRenderContext->blit(
        renderData[kReservoirOutput2.name]->asTexture()->getSRV(),
        renderData[kReservoirInput2.name]->asTexture()->getRTV()
    );
    pRenderContext->blit(
        renderData[kReservoirOutput3.name]->asTexture()->getSRV(),
        renderData[kReservoirInput3.name]->asTexture()->getRTV()
    );
}

void ReSTIRPass::allocateTemporalBuffers(uint2 size)
{
    Resource::BindFlags flags = Resource::BindFlags::RenderTarget | Resource::BindFlags::ShaderResource;
    mpTemporalCache[0] = Texture::create2D(size.x, size.y, ResourceFormat::RGBA32Float, 1, 1, nullptr, flags);
    mpTemporalCache[1] = Texture::create2D(size.x, size.y, ResourceFormat::RGBA32Float, 1, 1, nullptr, flags);
    mpTemporalCache[2] = Texture::create2D(size.x, size.y, ResourceFormat::R32Uint, 1, 1, nullptr, flags);
    //mpPrevReservoir[2] = Texture::create2D(width, height, ResourceFormat::RGBA32Float, 1, 1, nullptr, Resource::BindFlags::RenderTarget | Resource::BindFlags::ShaderResource);
}

void ReSTIRPass::initStaticSamplingVars()
{
    mGenerationProgram.pProgram->addDefines(mpSampleGenerator->getDefines());
    mGenerationProgram.pProgram->addDefine("SAMPLES_PER_RESERVOIR", std::to_string(mSamplesPerReservoir));
    mGenerationProgram.pProgram->addDefine("DO_VISIBILITY_REUSE", mDoVisibilityReuse ? "1" : "0");

    mGenerationProgram.pVars = RtProgramVars::create(mGenerationProgram.pProgram, mpScene);

    // Configure program.

    // Bind utility classes into shared data.
    auto pGlobalVars = mGenerationProgram.pVars->getRootVar();
    bool success = mpSampleGenerator->setShaderData(pGlobalVars);
    if (!success) throw std::exception("Failed to bind sample generator");

    // Create parameter block for shared data
    ProgramReflection::SharedConstPtr pReflection = mGenerationProgram.pProgram->getReflector();
    ParameterBlockReflection::SharedConstPtr pParamBlockReflection = pReflection->getParameterBlock(kParameterBlockName);
    assert(pParamBlockReflection);
    mGenerationProgram.pParamBlock = ParameterBlock::create(pParamBlockReflection);
    assert(mGenerationProgram.pParamBlock);

    // Bind all static resources to parameterblock now
    // Bind environment map if present
    if (mpEnvMapSampler)
        mpEnvMapSampler->setShaderData(mGenerationProgram.pParamBlock[kEnvMapSamplerMemberName]);

    // Bind the parameterblock to global shader variables (map to GPU memory)
    mGenerationProgram.pVars->setParameterBlock(kParameterBlockName, mGenerationProgram.pParamBlock);
}

void ReSTIRPass::initDynamicSamplingVars()
{
    auto& pParamBlock = mGenerationProgram.pParamBlock;
    assert(pParamBlock);

    // Upload configuration struct
    pParamBlock[kPathTracerParamsMemberName].setBlob(mSharedParams);

    // Bind emissive light sampler
    if (mUseEmissiveSampler)
    {
        assert(mpEmissiveSampler);
        bool success = mpEmissiveSampler->setShaderData(pParamBlock[kEmissiveLightSamplerMemberName]);
        if (!success)
            throw std::exception("Failed to bind emissive light sampler");
    }
}

void ReSTIRPass::initStaticTracingVars()
{
    mTracingProgram.pProgram->addDefines(mpSampleGenerator->getDefines());
    mTracingProgram.pProgram->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    mTracingProgram.pProgram->addDefine("DO_VISIBILITY_REUSE", mDoVisibilityReuse ? "1" : "0");

    mTracingProgram.pProgram->addDefine("CLAMP_RESULT", mClampResult ? "1" : "0");
    mTracingProgram.pProgram->addDefine("MAX_CLAMP", std::to_string(mMaxClampValue));

    mTracingProgram.pVars = RtProgramVars::create(mTracingProgram.pProgram, mpScene);
}

void ReSTIRPass::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    PathTracer::setScene(pRenderContext, pScene);
    mSceneChanged = true;

    mGenerationProgram.pVars = nullptr;
    mTracingProgram.pVars = nullptr;
    if (pScene)
    {
        pScene->setCameraController(Scene::CameraControllerType::FirstPerson);
        //pScene->toggleCameraAnimation(false);
        pScene->getCamera()->setIsAnimated(mEnableCameraAnimation);
        //pScene->setIsAnimated(false);
        mGenerationProgram.pProgram->addDefines(pScene->getSceneDefines());
        mTracingProgram.pProgram->addDefines(pScene->getSceneDefines());
        Shader::DefineList keeden = pScene->getSceneDefines();
        for (auto& define : keeden)
        {
            mpSpatialResamplePass->addDefine(define.first, define.second, false);
            //throw std::exception("haha lol");
        }
        mpSpatialResamplePass->setVars(nullptr);
    }

    mMaxFrames = 1;
    mCurrentFrameIndex = 0;
}

void ReSTIRPass::renderUI(Gui::Widgets& widget)
{
    // Zorg ervoor dat hier bepaalde zever gekozen kan worden
    bool recreateSamplingVars = false;
    bool recreateSpatialVars = false;
    bool recreateTracingVars = false;
    // Configure camera
    if (widget.checkbox("Enable camera animation", mEnableCameraAnimation) && mpScene != nullptr)
        mpScene->getCamera()->setIsAnimated(mEnableCameraAnimation); // Dees is uiterst lelijk
    // Configure initial sampling step
    recreateSamplingVars |= widget.var("Amount of candidate samples", mSamplesPerReservoir, 1u, 128u);
    widget.tooltip("The amount of candidate samples used for the creation of each initial reservoir");
    // configure visibility reust
    bool temp = widget.checkbox("Enable visibility reuse", mDoVisibilityReuse);
    widget.tooltip("Enable visbility reuse.");
    recreateSamplingVars |= temp;
    recreateTracingVars |= temp;
    // Configure temporal resampling
    widget.checkbox("Use temporal resampling", mDoTemporalResampling);
    widget.tooltip("Enable temporal resampling. This resampling step does not introduce significant bias.", true);
    // Configure spatial resampling
    widget.var("Amount of spatial resample passes", mAmountOfSpatialPasses, 0u, 8u);
    widget.tooltip("Set amount of spatial resampling passes. Setting this to 0 disables spatial resampling. This resampling step introduces bias");
    if (mAmountOfSpatialPasses > 0)
    {
        recreateSpatialVars |= widget.var("Amount of spatial samples", mAmountOfSpatialSamples, 1u, 512u);
        widget.tooltip("Amount of samples each spatial resampling pass will use.");
    }
    recreateTracingVars |= widget.checkbox("Clamp results", mClampResult);
    if (mClampResult)
        recreateTracingVars |= widget.var("Max clamped value", mMaxClampValue, 0.f, FLT_MAX);

    temp = widget.checkbox("Enable legacy BSDF code", mSharedParams.useLegacyBSDF);
    widget.tooltip("Needed for specular materials of wavefront based models to work");
    recreateSamplingVars |= temp;
    recreateTracingVars |= temp;

    widget.checkbox("Automatically reset pipeline", mAutoResetPipeline);
    widget.tooltip("Enable to reset the timeline after n frames, where each iteration n will be incremented (useful for gathering error data)");

    if (recreateSamplingVars)
        mGenerationProgram.pVars = nullptr;
    if (recreateSpatialVars) // Dees staat hier echt uiterst lelijk
        mpSpatialResamplePass->addDefine("AMOUNT_OF_SAMPLES", std::to_string(mAmountOfSpatialSamples), mpScene != nullptr);
    if (recreateTracingVars)
        mTracingProgram.pVars = nullptr;

    if (recreateSamplingVars || recreateSpatialVars || recreateTracingVars)
        mOptionsChanged = true;
}
