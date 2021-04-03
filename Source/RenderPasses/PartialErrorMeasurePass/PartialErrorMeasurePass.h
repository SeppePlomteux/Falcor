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
#include "Utils/Algorithm/ComputeParallelReduction.h"
#include "ErrorMetrics.slangh"

using namespace Falcor;

class PartialErrorMeasurePass : public RenderPass
{
public:
    using SharedPtr = std::shared_ptr<PartialErrorMeasurePass>;
    using ErrorMetric = uint;

    static const struct
    {
        static const uint gAbsoluteError = K_ABSOLUTE_ERROR;
        static const uint gSquaredError = K_SQUARED_ERROR;
        static const uint gSMAPE = K_SMAPE;
    } gErrorMetrics;

    /** Create a new render pass object.
        \param[in] pRenderContext The render context.
        \param[in] dict Dictionary of serialized parameters.
        \return A new object, or an exception is thrown if creation failed.
    */
    static SharedPtr create(RenderContext* pRenderContext = nullptr, const Dictionary& dict = {});
    static const char description[];

    virtual std::string getDesc() override { return description; }
    virtual Dictionary getScriptingDictionary() override;
    virtual RenderPassReflection reflect(const CompileData& compileData) override;
    virtual void compile(RenderContext* pContext, const CompileData& compileData) override {}
    virtual void execute(RenderContext* pRenderContext, const RenderData& renderData) override;
    virtual void renderUI(Gui::Widgets& widget) override;
    virtual void setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene) override;
    virtual bool onMouseEvent(const MouseEvent& mouseEvent) override;
    virtual bool onKeyEvent(const KeyboardEvent& keyEvent) override { return false; }

private:
    typedef struct
    {
        float3 mRGBError;
        float mAVGError;
    } ErrorValues;

    PartialErrorMeasurePass();
    void setReference();
    void setOutputFile();
    void addDataToOutput(ErrorValues errVals);
    void correctDiffWindow();

    bool mCompareFullScreen{ true };
    // Both these corners are relative between 0 and 1
    float2 mLeftUpperCorner{ 0, 0 };
    float2 mRightLowerCorner{ 0, 0 };

    //bool mCalculateAbsoluteError{ false };
    ErrorMetric mUsedMetric = gErrorMetrics.gSMAPE;

    bool mOutputImage{ true };

    uint mFramesToMeasure{ 10000 };
    uint mFrameCount{ 0 };

    bool mSceneChanged{ false };
    bool mHasAScene{ false };

    ErrorValues mPreviousErrors{ {-1, -1, -1}, -1 }; // init as invalid
    
    std::ofstream mOutputFileStream;

    Texture::SharedPtr mpReference;
    Texture::SharedPtr mpDifference;

    ComputePass::SharedPtr mpDiffPass;
    ComputeParallelReduction::SharedPtr mpParallelReduction;
    ComputePass::SharedPtr mpStitchPass;
};
