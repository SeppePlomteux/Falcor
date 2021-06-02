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
#pragma once
#include "Falcor.h"
#include "FalcorExperimental.h"
//#include "SDTree.h"
#include "STreeStump.h"
#include "RenderPasses/Shared/PathTracer/PathTracer.h"

#include <mutex>

using namespace Falcor;

class PPGPass : public PathTracer
{
public:
    using SharedPtr = std::shared_ptr<PPGPass>;

    struct TreeInfoTasks
    {
        //std::function<std::vector<uint8_t>(void)> mpReadPosTask;
        //std::function<std::vector<uint8_t>(void)> mpReadRadianceTask;
        //std::function<std::vector<uint8_t>(void)> mpReadSamplePdfTask;

        std::vector<uint8_t> mpReadPosTask;
        std::vector<uint8_t> mpReadRadianceTask;
        std::vector<uint8_t> mpReadSamplePdfTask;

        float3 mSceneSize; // save here because this can change over time
    };

    /** Create a new render pass object.
        \param[in] pRenderContext The render context.
        \param[in] dict Dictionary of serialized parameters.
        \return A new object, or an exception is thrown if creation failed.
    */
    static SharedPtr create(RenderContext* pRenderContext = nullptr, const Dictionary& dict = {});

    virtual std::string getDesc() override { return "Insert pass description here"; }
    virtual Dictionary getScriptingDictionary() override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pContext, const CompileData& compileData) override {}
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override { return false; }
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

    //void updateSDTree(std::shared_ptr<TreeInfoTasks> tasks);


private:
    PPGPass(const Dictionary& dict);

    void updateTreeTextures(RenderContext* pRenderContext);

    void resetStatisticalWeight(RenderContext* pRenderContext);
    void rescaleTree(RenderContext* pRenderContext);
    void splatIntoTree(RenderContext* pRenderContext, uint2 screenSize);
    void propagateTreeSums(RenderContext* pRenderContext);

    void updateSTreeStatWeight(RenderContext* pRenderContext);
    void updateSTreeStructure(RenderContext* pRenderContext);
    void updateDTreeStructure(RenderContext* pRenderContext);

    RtProgram::SharedPtr mpPPGProg;
    RtProgramVars::SharedPtr mpPPGVars;
    ParameterBlock::SharedPtr mpPPGParamBlock;

    struct
    {
        // Small textures
        Texture::SharedPtr pSTreeMetaDataTex;
        Texture::SharedPtr pDTreeEditDataTex;
        // Two dimentional textures
        Texture::SharedPtr pSTreeTex;
        Texture::SharedPtr pDTreeSumsTex;
        Texture::SharedPtr pDTreeChildrenTex;
        Texture::SharedPtr pDTreeParentTex;
        // One dimentional textures
        Texture::SharedPtr pDTreeSizeTex;
        Texture::SharedPtr pDTreeStatisticalWeightTex;
        Texture::SharedPtr pSTreeStatWeightTex;
        Texture::SharedPtr pDTreeFreedNodes;
    } mTreeTextures;

    struct
    {
        Texture::SharedPtr pPosTex;
        Texture::SharedPtr pRadianceTex;
        Texture::SharedPtr pDirPdfTex;
    } mSampleResultTextures;

    MyAABB mAABB;

    struct
    {
        ComputePass::SharedPtr pBlitDTreePass;
        ComputePass::SharedPtr pBlitSTreePass;

        ComputePass::SharedPtr pResetStatisticalWeightPass;
        ComputePass::SharedPtr pRescaleDTreePass;
        ComputePass::SharedPtr pSplatIntoSDTreePass;
        ComputePass::SharedPtr pPropagateDTreeSumsPass;
        ComputePass::SharedPtr pResetFreedNodesPass;
        ComputePass::SharedPtr pUpdateDTreeStructurePass;
        ComputePass::SharedPtr pCompressDTreePass;
        ComputePass::SharedPtr pRescaleSTreeStatWeightPass;
        ComputePass::SharedPtr pUpdateSTreeStatWeightPass;
        ComputePass::SharedPtr pUpdateSTreeStructurePass;
        ComputePass::SharedPtr pCompressSTreePass;
        ComputePass::SharedPtr pCopyDTreesPass;
    } mSDTreeUpdatePasses;

    struct
    {
        bool resetSDTreeOnLightingChanges = true;

        bool evalDirectLight = false;
        bool includeFirstDirectBounce = true;

        uint amountOfRISSamples = 8u;

        uint maxBounces = 5;

        float streeFactor = 0.9f;
        float streeSplitTreshold = 100000.f;
        float streeMergetreshold = 90000.f;
        bool streeDoMerge = true;

        float dtreeFactor = 0.9882f;
        float dtreeSplitTreshold = 0.038095f;
        float dtreeMergeTreshold = 0.026666666f;
        uint dtreeMaxDepth = 20u;
        bool dtreeDoMerge = true;
    } mRenderSettings;

    void setTracingDefines();
    void setDTreeRescaleDefines();
    void setSTreeRescaleDefines();
    void setBuildDTreeDefines();
    void setBuildSTreeDefines();

    struct
    {
        bool ppgSettingsChanged = true;

        bool streeFactorChanged = true;
        bool streeTresholdChanged = true;

        bool dtreeFactorChanged = true;
        bool dtreeTresholdChanged = true;
    } mSettingsChanged;
};
