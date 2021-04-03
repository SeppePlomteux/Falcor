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
#include "PartialErrorMeasurePass.h"
#include <RenderGraph/RenderPassHelpers.h>

// Don't remove this. it's required for hot-reload to function properly
extern "C" __declspec(dllexport) const char* getProjDir()
{
    return PROJECT_DIR;
}

extern "C" __declspec(dllexport) void getPasses(Falcor::RenderPassLibrary& lib)
{
    lib.registerClass("PartialErrorMeasurePass", PartialErrorMeasurePass::description, PartialErrorMeasurePass::create);
}

const char PartialErrorMeasurePass::description[] = "Pass for error measurement of a partial image";

namespace
{
    const ChannelDesc kColorChannel =
    {
        "color",    "shInputColor", "color"
    };

    const ChannelDesc kOutputChannel =
    {
        "outColor", "shOutput", "Output of the pass"
    };

    const ChannelDesc kWorldPosChannel =
    {
        "worldPos", "shWorldPos",   "World position (xyz) and depth (w)"
    };
    const char kReferenceTexName[] = "shReference";
    const char kOutputTexName[] = "shOutputDiff";

    const char kErrorMetricDefineName[] = "K_ERR_METRIC";

    const Gui::DropdownList kErrorMetricsList =
    {
        { PartialErrorMeasurePass::gErrorMetrics.gAbsoluteError,    "Absolute error" },
        { PartialErrorMeasurePass::gErrorMetrics.gSquaredError,     "Squared error" },
        { PartialErrorMeasurePass::gErrorMetrics.gSMAPE,            "SMAPE (Symmetric Mean Absolute Percentage Error" }
    };
}

PartialErrorMeasurePass::PartialErrorMeasurePass()
{
    Program::DefineList dl;
    dl.add(kErrorMetricDefineName, std::to_string(mUsedMetric));
    mpDiffPass = ComputePass::create("RenderPasses/PartialErrorMeasurePass/ComputeError.slang", "main", dl);
    mpParallelReduction = ComputeParallelReduction::create();
    mpStitchPass = ComputePass::create("RenderPasses/PartialErrorMeasurePass/stitch.ps.hlsl");
}

PartialErrorMeasurePass::SharedPtr PartialErrorMeasurePass::create(RenderContext* pRenderContext, const Dictionary& dict)
{
    SharedPtr pPass = SharedPtr(new PartialErrorMeasurePass);
    return pPass;
}

Dictionary PartialErrorMeasurePass::getScriptingDictionary()
{
    return Dictionary();
}

RenderPassReflection PartialErrorMeasurePass::reflect(const CompileData& compileData)
{
    // Define the required resources here
    RenderPassReflection reflector;
    reflector.addInput(kColorChannel.name, kColorChannel.desc)
        .bindFlags(ResourceBindFlags::ShaderResource | Resource::BindFlags::UnorderedAccess);
    reflector.addInput(kWorldPosChannel.name, kWorldPosChannel.desc)
        .bindFlags(ResourceBindFlags::ShaderResource | Resource::BindFlags::UnorderedAccess);
    reflector.addOutput(kOutputChannel.name, kOutputChannel.desc)
        .bindFlags(ResourceBindFlags::ShaderResource | Resource::BindFlags::UnorderedAccess | Resource::BindFlags::RenderTarget)
        .format(ResourceFormat::RGBA32Float);
    return reflector;
}

void PartialErrorMeasurePass::execute(RenderContext* pRenderContext, const RenderData& renderData)
{
    // Check if pipeline is refreshed, if yes, write prev error to file
    auto pipelineRefreshed = renderData.getDictionary().getValue(kRenderPassRefreshFlags, RenderPassRefreshFlags::None);
    if (pipelineRefreshed != RenderPassRefreshFlags::None)
    {
        addDataToOutput(mPreviousErrors);
        mFrameCount = 0;
    }
    uint2 leftCornerUint;
    uint2 rightCornerUint;
    if (mCompareFullScreen)
    {
        leftCornerUint = { 0u, 0u };
        rightCornerUint = renderData.getDefaultTextureDims() - uint2(1, 1);
    }
    else
    {
        const uint2 dims = renderData.getDefaultTextureDims();
        leftCornerUint = uint2(mLeftUpperCorner.x * dims.x, mLeftUpperCorner.y * dims.y);
        rightCornerUint = uint2(mRightLowerCorner.x * dims.x, mRightLowerCorner.y * dims.y);
    }
    if (!mHasAScene)
        return;
    if (mpReference == nullptr)
        return;
    if (mSceneChanged)
    {
        mSceneChanged = false;
        mFrameCount = 0u;
    }
    if (mFrameCount < mFramesToMeasure)
    {
        PartialErrorMeasurePass::ErrorValues errVals{};

        // do actual image compare
        if (!mpDifference ||
            rightCornerUint.x - leftCornerUint.x + 1 != mpDifference->getWidth() ||
            rightCornerUint.y - leftCornerUint.y + 1 != mpDifference->getHeight())
        {
            mpDifference = Texture::create2D(
                rightCornerUint.x - leftCornerUint.x + 1,
                rightCornerUint.y - leftCornerUint.y + 1,
                ResourceFormat::RGBA32Float,
                1,
                1,
                nullptr,
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        }
        // Map
        mpDiffPass[kColorChannel.texname] = renderData[kColorChannel.name]->asTexture();
        mpDiffPass[kWorldPosChannel.texname] = renderData[kWorldPosChannel.name]->asTexture();
        mpDiffPass[kReferenceTexName] = mpReference;
        mpDiffPass[kOutputTexName] = mpDifference;
        mpDiffPass["buf"]["gToMap"] = uint4(leftCornerUint, rightCornerUint);
        //mpDiffPass["buf"]["gCalcAbsoluteDiff"] = mCalculateAbsoluteError ? 0u : 1u;
        uint3 targetDim = uint3(rightCornerUint - leftCornerUint + 1u, 1u);
        mpDiffPass->execute(pRenderContext, targetDim);
        // Reduce
        float4 error;
        if (!mpParallelReduction->execute(pRenderContext, mpDifference, ComputeParallelReduction::Type::Sum, &error))
        {
            throw std::exception("Error running parallel reduction in ErrorMeasurePass");
        }
        error = error / static_cast<float>(mpDifference->getWidth() * mpDifference->getHeight());
        errVals.mRGBError = error.xyz;
        errVals.mAVGError = (errVals.mRGBError.x + errVals.mRGBError.y + errVals.mRGBError.z) / 3;
        if (mUsedMetric == PartialErrorMeasurePass::gErrorMetrics.gSMAPE)
        {
            errVals.mAVGError = 100*errVals.mAVGError;
            errVals.mRGBError = static_cast<float>(100) * errVals.mRGBError;
        }
        mFrameCount++;
        //addDataToOutput(errVals); TODO change to dynamic behaviour
        mPreviousErrors = errVals;
        // Output
        if (mOutputImage)
        {
            pRenderContext->blit(renderData[kColorChannel.name]->getSRV(), renderData[kOutputChannel.name]->asTexture()->getRTV());
        }
        else
        {
            mpStitchPass["shWindow"] = mpDifference;
            mpStitchPass["shOutput"] = renderData[kOutputChannel.name]->asTexture();
            mpStitchPass["buf"]["gWindowCoords"] = uint4(leftCornerUint, rightCornerUint);
            uint2 kak = renderData.getDefaultTextureDims();
            mpStitchPass["buf"]["gImageSize"] = kak;
            mpStitchPass->execute(pRenderContext, uint3(kak, 1));
        }
    }
}

void PartialErrorMeasurePass::addDataToOutput(PartialErrorMeasurePass::ErrorValues errVals)
{
    if (!mOutputFileStream)
        return;
    mOutputFileStream << errVals.mAVGError << ",";
    mOutputFileStream << errVals.mRGBError.r << "," << errVals.mRGBError.g << "," << errVals.mRGBError.b;
    mOutputFileStream << std::endl;
}

void PartialErrorMeasurePass::setReference()
{
    FileDialogFilterVec filters;
    filters.push_back({ "exr", "High Dynamic Range" });
    filters.push_back({ "pfm", "Portable Float Map" });
    std::string filename;
    if (! openFileDialog(filters, filename))
    {
        return;
    }
    mpReference = Texture::createFromFile(filename, false /* no MIPs */, false /* linear color */);
    if (!mpReference)
    {
        logError("Failed to load texture " + filename);
    }
}

void PartialErrorMeasurePass::setScene(RenderContext* pRenderContext, const Scene::SharedPtr& pScene)
{
    mSceneChanged = true;
    if (pScene != nullptr)
        mHasAScene = true;
    if (mOutputFileStream)
    {
        mOutputFileStream.flush();
        mOutputFileStream.close();
    }
    if (pScene != nullptr)
        setOutputFile();
}

void PartialErrorMeasurePass::setOutputFile()
{
    FileDialogFilterVec filters;
    filters.push_back({ "csv", "CSV Files" });
    std::string filename;
    if (! saveFileDialog(filters, filename))
    {
        return;
    }
    mOutputFileStream = std::ofstream(filename, std::ios::trunc);
    if (!mOutputFileStream)
    {
        logError("Failed to open file " + filename);
    }
    else
    {
        switch (mUsedMetric)
        {
        case PartialErrorMeasurePass::gErrorMetrics.gAbsoluteError:
        {
            mOutputFileStream << "avg_L1_error,red_L1_error,green_L1_error,blue_L1_error" << std::endl;
            break;
        }
        case PartialErrorMeasurePass::gErrorMetrics.gSquaredError:
        {
            mOutputFileStream << "avg_L2_error,red_L2_error,green_L2_error,blue_L2_error" << std::endl;
            break;
        }
        case PartialErrorMeasurePass::gErrorMetrics.gSMAPE:
        {
            mOutputFileStream << "avg_SMAPE_error,red_SMAPE_error,green_SMAPE_error,blue_SMAPE_errror" << std::endl;
            break;
        }
        default:
        {
            should_not_get_here();
        }
        }
        mOutputFileStream << std::scientific;
    }
}

void PartialErrorMeasurePass::renderUI(Gui::Widgets& widget)
{
    if (mpReference == nullptr)
        widget.text("No reference image set!");
    auto compareG = widget.group("Compare subset");
    if (compareG.button("Load reference"))
        setReference();
    compareG.checkbox("Compare entire image", mCompareFullScreen);
    if (!mCompareFullScreen)
    {
        bool dirty = false;
        compareG.dummy("dummy1", { 1, 15 });
        dirty |= compareG.var("Left upper X", mLeftUpperCorner.x, 0.f, 1.f);
        dirty |= compareG.var("Left upper Y", mLeftUpperCorner.y, 0.f, 1.f);
        compareG.tooltip(   "These vars define the upper left corner of the diff window. "
                            "You can also set this by left clicking in the image");
        compareG.dummy("dummy2", { 1, 15 });
        dirty |= compareG.var("Right lower X", mRightLowerCorner.x, 0.f, 1.f);
        dirty |= compareG.var("Right lower Y", mRightLowerCorner.y, 0.f, 1.f);
        compareG.tooltip("These vars define the lower right corner of the diff window. "
            "You can also set this by right clicking in the image");
        if (dirty)
            correctDiffWindow();
    }
    compareG.release();

    auto measureG = widget.group("Measurement options");
    bool dirty = measureG.dropdown("Used error metric", kErrorMetricsList, mUsedMetric);
    if (dirty)
    {
        mpDiffPass->addDefine(kErrorMetricDefineName, std::to_string(mUsedMetric), true);
    }
    measureG.release();

    auto outputG = widget.group("Output options");
    if (outputG.button("Set output file"))
        setOutputFile();
    outputG.tooltip( "Sets the output file."
                    "If a new scene is loaded without changing the output file, this file will not be overwritten.\r\n"
                    "In this case, a new file will be created with the name oldName + '_num.csv',"
                    "where num is a number such that a file with that name doesn't currenly exist");
    outputG.var("Amount of frames to include", mFramesToMeasure);
    outputG.tooltip("Sets the amount of consecutive frames that will be compared to the reference image.");
    outputG.checkbox("Output image", mOutputImage);
    outputG.tooltip("Will pass the provided image to the output, instead of displaying the error window.");
    outputG.release();
}

void PartialErrorMeasurePass::correctDiffWindow()
{
    if (mLeftUpperCorner.x > mRightLowerCorner.x)
    {
        mLeftUpperCorner.x = mRightLowerCorner.x;
    }

    if (mLeftUpperCorner.y > mRightLowerCorner.y)
    {
        mLeftUpperCorner.y = mRightLowerCorner.y;
    }
}

bool PartialErrorMeasurePass::onMouseEvent(const MouseEvent& mouseEvent)
{
    if (mCompareFullScreen)
        return false;
    if (mouseEvent.type == MouseEvent::Type::LeftButtonDown)
    {
        mLeftUpperCorner.x = mouseEvent.pos.x;
        mLeftUpperCorner.y = mouseEvent.pos.y;
        correctDiffWindow();
        return true;
    }
    if (mouseEvent.type == MouseEvent::Type::RightButtonDown)
    {
        mRightLowerCorner.x = mouseEvent.pos.x;
        mRightLowerCorner.y = mouseEvent.pos.y;
        correctDiffWindow();
        return true;
    }
    return false;
}
