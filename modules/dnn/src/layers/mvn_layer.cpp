/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                           License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2013, OpenCV Foundation, all rights reserved.
// Copyright (C) 2017, Intel Corporation, all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include "../precomp.hpp"
#include "layers_common.hpp"
#include <opencv2/dnn/shape_utils.hpp>
#include "math_functions.hpp"
#include "opencl_kernels_dnn.hpp"

namespace cv
{
namespace dnn
{

class MVNLayerImpl : public MVNLayer
{
public:
    MVNLayerImpl(const LayerParams& params)
    {
        setParamsFrom(params);
        normVariance = params.get<bool>("normalize_variance", true);
        acrossChannels = params.get<bool>("across_channels", false);
        eps = params.get<double>("eps", 1e-9);
    }

#ifdef HAVE_OPENCL
    bool forward_ocl(InputArrayOfArrays inputs_, OutputArrayOfArrays outputs_, OutputArrayOfArrays internals_)
    {
        std::vector<UMat> inputs;
        std::vector<UMat> outputs;

        inputs_.getUMatVector(inputs);
        outputs_.getUMatVector(outputs);

        for (size_t inpIdx = 0; inpIdx < inputs.size(); inpIdx++)
        {
            UMat &inpBlob = inputs[inpIdx];
            UMat &outBlob = outputs[inpIdx];

            int splitDim = (acrossChannels) ? 1 : 2;
            int i, newRows = 1;
            for( i = 0; i < splitDim; i++ )
                newRows *= inpBlob.size[i];

            MatShape s = shape(newRows, inpBlob.total() / newRows);
            UMat& inpMat = inpBlob;
            UMat& outMat = outBlob;
            UMat oneMat = UMat::ones(s[1], 1, CV_32F);
            UMat meanMat = UMat(s[0], 1, CV_32F);
            UMat devMat  = UMat(s[0], 1, CV_32F);
            UMat tmpMat  = UMat(s[0], s[1], CV_32F);
            float alpha = 1.0f / s[1];

            bool ret = ocl4dnn::ocl4dnnGEMV<float>(ocl4dnn::CblasNoTrans, s[0], s[1], alpha,
                                                   inpMat, 0, oneMat, 0, 0.0f, meanMat, 0);
            if (!ret)
                return false;

            int number = (s[1] % 8 == 0) ? 8 : ((s[1] % 4 == 0) ? 4 : 1);
            size_t global[] = { (size_t)s[0], (size_t)(s[1] / number) };
            String buildopt = format("-DNUM=%d ", number);
            if (normVariance)
            {
                String kname = format("calc_mean%d", number);
                ocl::Kernel kernel(kname.c_str(), ocl::dnn::mvn_oclsrc, buildopt);
                if (kernel.empty())
                    return false;

                kernel.set(0, ocl::KernelArg::PtrReadOnly(inpMat));
                kernel.set(1, (int)s[0]);
                kernel.set(2, (int)s[1]);
                kernel.set(3, ocl::KernelArg::PtrReadOnly(meanMat));
                kernel.set(4, ocl::KernelArg::PtrWriteOnly(tmpMat));
                ret = kernel.run(2, global, NULL, false);
                if (!ret)
                    return false;

                ret = ocl4dnn::ocl4dnnGEMV<float>(ocl4dnn::CblasNoTrans, s[0], s[1], alpha,
                                                  tmpMat, 0, oneMat, 0, 0.0f, devMat, 0);
                if (!ret)
                    return false;
            }

            String kname = format("mvn%d", number);
            if (normVariance)
                buildopt += "-DNORM_VARIANCE";
            ocl::Kernel kernel1(kname.c_str(), ocl::dnn::mvn_oclsrc, buildopt);
            if (kernel1.empty())
                return false;
            kernel1.set(0, ocl::KernelArg::PtrReadOnly(inpMat));
            kernel1.set(1, (int)s[0]);
            kernel1.set(2, (int)s[1]);
            kernel1.set(3, (float)eps);
            kernel1.set(4, ocl::KernelArg::PtrReadOnly(meanMat));
            kernel1.set(5, ocl::KernelArg::PtrReadOnly(devMat));
            kernel1.set(6, ocl::KernelArg::PtrWriteOnly(outMat));
            ret = kernel1.run(2, global, NULL, false);
            if (!ret)
                return false;
        }
        return true;
    }
#endif

    void forward(InputArrayOfArrays inputs_arr, OutputArrayOfArrays outputs_arr, OutputArrayOfArrays internals_arr)
    {
        CV_TRACE_FUNCTION();
        CV_TRACE_ARG_VALUE(name, "name", name.c_str());

        CV_OCL_RUN((preferableTarget == DNN_TARGET_OPENCL) &&
                   OCL_PERFORMANCE_CHECK(ocl::Device::getDefault().isIntel()),
                   forward_ocl(inputs_arr, outputs_arr, internals_arr))

        Layer::forward_fallback(inputs_arr, outputs_arr, internals_arr);
    }

    void forward(std::vector<Mat *> &inputs, std::vector<Mat> &outputs, std::vector<Mat> &internals)
    {
        CV_TRACE_FUNCTION();
        CV_TRACE_ARG_VALUE(name, "name", name.c_str());

        for (size_t inpIdx = 0; inpIdx < inputs.size(); inpIdx++)
        {
            Mat &inpBlob = *inputs[inpIdx];
            Mat &outBlob = outputs[inpIdx];

            int splitDim = (acrossChannels) ? 1 : 2;
            int i, newRows = 1;
            for( i = 0; i < splitDim; i++ )
                newRows *= inpBlob.size[i];
            Mat inpMat = inpBlob.reshape(1, newRows);
            Mat outMat = outBlob.reshape(1, newRows);

            Scalar mean, dev;
            for ( i = 0; i < newRows; i++)
            {
                Mat inpRow = inpMat.row(i);
                Mat outRow = outMat.row(i);

                cv::meanStdDev(inpRow, mean, (normVariance) ? dev : noArray());
                double alpha = (normVariance) ? 1/(eps + dev[0]) : 1;
                inpRow.convertTo(outRow, outRow.type(), alpha, -mean[0] * alpha);
            }
        }
    }

    virtual int64 getFLOPS(const std::vector<MatShape> &inputs,
                           const std::vector<MatShape> &outputs) const
    {
        (void)outputs; // suppress unused variable warning
        long flops = 0;
        for(int i = 0; i < inputs.size(); i++)
        {
            flops += 6*total(inputs[i]) + 3*total(inputs[i], 0, normVariance ? 2 : 1);
        }
        return flops;
    }
};

Ptr<MVNLayer> MVNLayer::create(const LayerParams& params)
{
    return Ptr<MVNLayer>(new MVNLayerImpl(params));
}

}
}
