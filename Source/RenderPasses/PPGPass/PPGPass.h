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

    void updateSDTree(std::shared_ptr<TreeInfoTasks> tasks);


private:
    PPGPass(const Dictionary& dict);

    void updateTreeTextures(RenderContext* pRenderContext);

    void resetStatisticalWeight(RenderContext* pRenderContext);
    void rescaleTree(RenderContext* pRenderContext);
    void splatIntoTree(RenderContext* pRenderContext, uint2 screenSize);
    void propagateTreeSums(RenderContext* pRenderContext);
    void updateTree(RenderContext* pRenderContext, std::vector<uint8_t>& statWeightVec);


    //size_t mCurrentSamplesPerPixel = 0;
    //size_t mMaxSamplesPerPixel = 1;

    volatile bool mCurrentlyUpdatingTree = false;

    RtProgram::SharedPtr mpPPGProg;
    RtProgramVars::SharedPtr mpPPGVars;
    ParameterBlock::SharedPtr mpPPGParamBlock;

    struct
    {
        // Two dimentional textures
        Texture::SharedPtr pSTreeTex;
        Texture::SharedPtr pDTreeSumsTex;
        Texture::SharedPtr pDTreeChildrenTex;
        Texture::SharedPtr pDTreeParentTex;
        // One dimentional textures
        Texture::SharedPtr pDTreeSizeTex;
        Texture::SharedPtr pDTreeStatisticalWeightTex;
    } mTreeTextures;

    /*struct
    {
        Texture::SharedPtr pDTreeSumsTex;
        Texture::SharedPtr pDTreeChildrenTex;
        Texture::SharedPtr pDTreeStatisticalWeightTex;
        Texture::SharedPtr pDTreeMutex;
    } mBuildingTreeTextures;*/

    struct
    {
        Texture::SharedPtr pPosTex;
        Texture::SharedPtr pRadianceTex;
        Texture::SharedPtr pDirPdfTex;
    } mSampleResultTextures;

    //STree::SharedPtr mpTree;
    STreeStump::SharedPtr mpTree;

    //std::unique_ptr<std::thread> mpWorkingThread; // If Falcor exits, unique pointer ensures that working thread is stopped

    struct
    {
        ComputePass::SharedPtr pBlitDTreePass;
        ComputePass::SharedPtr pBlitSTreePass;

        ComputePass::SharedPtr pResetStatisticalWeightPass;
        ComputePass::SharedPtr pRescaleDTreePass;
        ComputePass::SharedPtr pSplatIntoSDTreePass;
        ComputePass::SharedPtr pPropagateDTreeSumsPass;
        ComputePass::SharedPtr pUpdateDTreeStructurePass;
        ComputePass::SharedPtr pCompressDTreePass;
        ComputePass::SharedPtr pCompressSTreePass;
        ComputePass::SharedPtr pCopySingleDTreePass;
    } mSDTreeUpdatePasses;

    //ComputePass::SharedPtr mpResetStatisticalWeightPass;
    //ComputePass::SharedPtr mpResetMutexPass;
    //ComputePass::SharedPtr mpSplatIntoSDTreePass;

    //void rebuildTree();
    //void updateDTreeBuilding(DTreeTexData data);
};
