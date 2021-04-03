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
#include "ResetPipelinePass.h"

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("ResetPipelinePass", "Render Pass Template", ResetPipelinePass::create);
}

char ResetPipelinePass::description[] = "Resets the pipeline to simplify testing";

ResetPipelinePass::SharedPtr ResetPipelinePass::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new ResetPipelinePass);
    return pPass;
}

Dictionary ResetPipelinePass::getScriptingDictionary()
{
    return Dictionary();
}

RenderPassReflection ResetPipelinePass::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInputOutput("color", "The color texture. Is passed trough unchanged");
    return reflector;
}

void ResetPipelinePass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    if (!mHasScene)
        return;
    if (mFrameIndex >= mMaxFrames)
    {
        mFrameIndex = 0;
        mMaxFrames++;
        auto currentFlags = renderData.getDictionary().getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
        renderData.getDictionary()[kRenderPassRefreshFlags] = currentFlags | RenderPassRefreshFlags::RenderOptionsChanged;
    }
    else
    {
        mFrameIndex++;
    }
}

void ResetPipelinePass::renderUI(Gui::Widgets& widget)
{
}

void ResetPipelinePass::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mFrameIndex = 0;
    mMaxFrames = 1;
    mHasScene = pScene != nullptr;
}
