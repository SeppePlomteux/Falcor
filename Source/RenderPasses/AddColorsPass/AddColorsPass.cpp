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
#include "AddColorsPass.h"
#include <RenderGraph/RenderPassHelpers.h>


namespace
{
    const char kDesc[] = "Adds two color textures together";

    ChannelList kInputChannels =
    {
        {"firstInColor",    "gFirstInputColor",     "The first input color."},
        {"secondInColor",   "gSecondInputColor",    "The second input color."}
    };

    ChannelList kOutputChannels =
    {
        {"outColor",        "gOutputColor",         "The output color"}
    };

    const char kIgnoreNonFiniteValsDefineName[] = "K_IGNORE_NON_FINITE_VALS";
}

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("AddColorsPass", kDesc, AddColorsPass::create);
}

AddColorsPass::AddColorsPass()
{
    Program::DefineList dl;
    dl.add(kIgnoreNonFiniteValsDefineName, "0");
    mIgnoreNonFiniteVals = false;
    mpAddPass = ComputePass::create("RenderPasses/AddColorsPass/ComputeSum.cs.hlsl", "main", dl);
}

AddColorsPass::SharedPtr AddColorsPass::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new AddColorsPass);
    return pPass;
}

std::string AddColorsPass::getDesc() { return kDesc; }

Dictionary AddColorsPass::getScriptingDictionary()
{
    return Dictionary();
}

RenderPassReflection AddColorsPass::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    addRenderPassInputs(reflector, kInputChannels);
    addRenderPassOutputs(reflector, kOutputChannels, ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
    return reflector;
}

void AddColorsPass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    uint2 screenSize = renderData.getDefaultTextureDims();

    mpAddPass["SumBuf"]["gTextureSize"] = screenSize;

    auto bindResources = [&](const ChannelList& desc)
    {
        for (auto& ch : desc)
            mpAddPass[ch.texname] = renderData[ch.name]->asTexture();
    };
    bindResources(kInputChannels);
    bindResources(kOutputChannels);

    mpAddPass->execute(pRenderContext, uint3(screenSize, 1));
}

void AddColorsPass::renderUI(Gui::Widgets& widget)
{
    bool dirty = false;
    dirty |= widget.checkbox("Ignore non-finite values", mIgnoreNonFiniteVals);
    if (dirty)
        mpAddPass->addDefine(kIgnoreNonFiniteValsDefineName, mIgnoreNonFiniteVals ? "1" : "0", true);
}
