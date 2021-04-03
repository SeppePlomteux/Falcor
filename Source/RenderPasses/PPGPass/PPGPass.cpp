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

}

PPGPass::PPGPass(const Dictionary& dict)
{
    RtProgram::Desc description;
    description.addShaderLibrary("RenderPasses/PPGPass/PPG.rt.slang").setRayGen("rayGen");
    description.addHitGroup(0, "scatterClosestHit", "scatterAnyHit").addMiss(0, "scatterMiss");
    description.addHitGroup(1, "", "shadowAnyHit").addMiss(1, "shadowMiss");
    //description.addDefine("MAX_BOUNCES", std::to_string(3));
    description.setMaxTraceRecursionDepth(kMaxRecursionDepth);
    mpPPGProg = RtProgram::create(description, kMaxPayloadSize, kMaxAttributeSize);

    mpSampleGenerator = SampleGenerator::create(SAMPLE_GENERATOR_UNIFORM);
    assert(mpSampleGenerator);
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
    addRenderPassInputs(reflector, kShadingInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels);
    return reflector;
}

void PPGPass::execute(RenderContext* pRenderContext, const RenderData& renderData) {

    // Update refresh flag if options that affect the output have changed.
    auto& dict = renderData.getDictionary();

    // If we have no scene, just clear the outputs and return.
    if (!mpScene)
    {
        for (auto it : kOutputChannels)
        {
            Texture* pDst = renderData[it.name]->asTexture().get();
            if (pDst) pRenderContext->clearTexture(pDst);
        }
        return;
    }

    // Request the light collection if emissive lights are enabled.
    if (mpScene->getRenderSettings().useEmissiveLights)
    {
        mpScene->getLightCollection(pRenderContext);
    }

    // Configure depth-of-field.

    // Specialize program.
    // These defines should not modify the program vars. Do not trigger program vars re-creation.
    //mpPPGProg->addDefine("MAX_BOUNCES", std::to_string(3));
    //mpPPGProg->addDefine("COMPUTE_DIRECT", 0 ? "1" : "0");
    //mpPPGProg->addDefine("USE_ANALYTIC_LIGHTS", mpScene->useAnalyticLights() ? "1" : "0");
    //mpPPGProg->addDefine("USE_EMISSIVE_LIGHTS", mpScene->useEmissiveLights() ? "1" : "0");
    //mpPPGProg->addDefine("USE_ENV_LIGHT", mpScene->useEnvLight() ? "1" : "0");
    //mpPPGProg->addDefine("USE_ENV_BACKGROUND", mpScene->useEnvBackground() ? "1" : "0");

    // For optional I/O resources, set 'is_valid_<name>' defines to inform the program of which ones it can access.
    // TODO: This should be moved to a more general mechanism using Slang.
    //mTracer.pProgram->addDefines(getValidResourceDefines(kInputChannels, renderData));
    //mTracer.pProgram->addDefines(getValidResourceDefines(kOutputChannels, renderData));

    // Prepare program vars. This may trigger shader compilation.
    // The program should have all necessary defines set at this point.
    if (!mpPPGVars)
    {
        assert(mpScene);
        assert(mpPPGProg);

        // Configure program.
        //mpPPGProg->addDefines(mpSampleGenerator->getDefines());

        // Create program variables for the current program/scene.
        // This may trigger shader compilation. If it fails, throw an exception to abort rendering.
        mpPPGVars = RtProgramVars::create(mpPPGProg, mpScene);

        // Bind utility classes into shared data.
        //auto pGlobalVars = mpPPGVars->getRootVar();
        //bool success = mpSampleGenerator->setShaderData(pGlobalVars);
        //if (!success) throw std::exception("Failed to bind sample generator");
    }
    assert(mpPPGVars);

    // Set constants.
    auto pVars = mpPPGVars;
    //pVars["CB"]["gFrameCount"] = mFrameCount;
    //pVars["CB"]["gPRNGDimension"] = dict.keyExists(kRenderPassPRNGDimension) ? dict[kRenderPassPRNGDimension] : 0u;

    // Bind I/O buffers. These needs to be done per-frame as the buffers may change anytime.
    auto bind = [&](const ChannelDesc& desc)
    {
        if (!desc.texname.empty())
        {
            auto pGlobalVars = mpPPGVars;
            pGlobalVars[desc.texname] = renderData[desc.name]->asTexture();
        }
    };
    //for (auto channel : kInputChannels) bind(channel);
    //for (auto channel : kOutputChannels) bind(channel);

    // Get dimensions of ray dispatch.
    const uint2 targetDim = renderData.getDefaultTextureDims();
    assert(targetDim.x > 0 && targetDim.y > 0);

    // Spawn the rays.
    mpScene->raytrace(pRenderContext, mpPPGProg.get(), mpPPGVars, uint3(targetDim, 1));

}

void PPGPass::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mpScene = pScene;

    //mpWorkingThread = nullptr; // stop potentially running thread

    mpPPGVars = nullptr;

    mpPPGProg->addDefines(pScene->getSceneDefines());
    //mpTree = STree::SharedPtr(new STree(AABB(aabb_falcor.getMinPos() - delta, aabb_falcor.getMaxPos() + delta)));
}

void PPGPass::renderUI(Gui::Widgets& widget)
{
}
