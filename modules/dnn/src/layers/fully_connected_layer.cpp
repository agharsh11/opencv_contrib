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
#include "op_blas.hpp"
#include "op_halide.hpp"
#include <opencv2/dnn/shape_utils.hpp>

namespace cv
{
namespace dnn
{

class FullyConnectedLayerImpl : public InnerProductLayer
{
public:
    enum { VEC_ALIGN = 8 };

    FullyConnectedLayerImpl(const LayerParams& params)
    {
        setParamsFrom(params);
        CV_Assert(1 <= blobs.size() && blobs.size() <= 2);

        int numOutput = params.get<int>("num_output");
        int innerSize = (int)blobs[0].total() / numOutput;
        bias = params.get<bool>("bias_term", true);
        axis = params.get<int>("axis", 1);

        CV_Assert(blobs[0].dims >= 2 && (size_t)(innerSize * numOutput) == blobs[0].total());
        CV_Assert(!bias || (blobs.size() == 2 && (size_t)numOutput == blobs[1].total()));

        weightsMat = blobs[0] = blobs[0].reshape(1, numOutput);
        int vecsize = weightsMat.cols;
        if( vecsize % VEC_ALIGN != 0 )
        {
            int vecsize_aligned = (int)alignSize(vecsize, VEC_ALIGN);
            Mat weightsBuf(weightsMat.rows, vecsize_aligned, weightsMat.type());
            Mat wpadding = weightsBuf.colRange(vecsize, vecsize_aligned);
            wpadding.setTo(Scalar::all(0.));
            weightsMat = weightsBuf.colRange(0, vecsize);
            blobs[0].copyTo(weightsMat);
            blobs[0] = weightsMat;
        }

        if (bias)
            biasMat = blobs[1] = blobs[1].reshape(1, 1);
        else
            biasMat = Mat::zeros(1, numOutput, weightsMat.type());
    }

    bool getMemoryShapes(const std::vector<MatShape> &inputs,
                         const int requiredOutputs,
                         std::vector<MatShape> &outputs,
                         std::vector<MatShape> &) const
    {
        CV_Assert(inputs.size() > 0);
        CV_Assert(1 <= blobs.size() && blobs.size() <= 2);
        CV_Assert(blobs[0].dims == 2);

        int cAxis = clamp(axis, inputs[0]);
        int outerSize = total(inputs[0], 0, cAxis);
        int numOutput = blobs[0].size[0];
        outputs.resize(inputs.size(), shape(outerSize, numOutput));

        CV_Assert(!bias || (size_t)numOutput == blobs[1].total());
        return false;
    }

    virtual bool supportBackend(int backendId)
    {
        return backendId == DNN_BACKEND_DEFAULT ||
               backendId == DNN_BACKEND_HALIDE && haveHalide() && axis == 1;
    }

    class FullConnected : public ParallelLoopBody
    {
    public:
        FullConnected(const Mat& srcMat, const Mat& weights, const Mat& biasMat, Mat& dstMat, int nstripes)
        {
            CV_Assert( srcMat.dims == 2 && srcMat.cols == weights.cols &&
                       dstMat.rows == srcMat.rows && dstMat.cols == weights.rows &&
                       srcMat.type() == weights.type() && weights.type() == dstMat.type() &&
                       srcMat.type() == CV_32F &&
                       (biasMat.empty() || (biasMat.type() == srcMat.type() &&
                        biasMat.isContinuous() && (int)biasMat.total() == dstMat.cols)) );

            srcMat_ = &srcMat;
            weights_ = &weights;
            biasMat_ = &biasMat;
            dstMat_ = &dstMat;
            nstripes_ = nstripes;
            useAVX2_ = checkHardwareSupport(CPU_AVX2);
        }

        void operator()(const Range& r) const
        {
            int nsamples = srcMat_->rows;
            int nw0 = weights_->rows;
            int vecsize = srcMat_->cols;
            int nstripes = nstripes_;
            size_t total = (size_t)nsamples*nw0;
            size_t stripeSize = (total + nstripes - 1)/nstripes;
            size_t stripeStart = r.start*stripeSize;
            size_t stripeEnd = r.end == nstripes ? total : std::min(r.end*stripeSize, total);
            size_t wstep = weights_->step1();

            for( size_t ofs = stripeStart; ofs < stripeEnd; )
            {
                int sampleIdx = (int)(ofs / nw0);
                int delta = (int)(ofs - (size_t)sampleIdx*nw0);
                const float* sptr = srcMat_->ptr<float>(sampleIdx);
                const float* wptr = weights_->ptr<float>(delta);
                float* dptr = dstMat_->ptr<float>(sampleIdx) + delta;
                const float* biasptr = biasMat_->ptr<float>() + delta;
                int nw = std::min(nw0 - delta, (int)(stripeEnd - ofs));

            #if CV_DNN_TRY_AVX2
                if( useAVX2_ )
                    fastGEMM1T_avx2( sptr, wptr, wstep, biasptr, dptr, nw, vecsize);
                else
            #endif
                {
                    int i = 0, k;

            #if CV_SIMD128
                    for( ; i <= nw - 4; i += 4, wptr += 4*wstep )
                    {
                        vfloat32x4 vs0 = v_setall_f32(0.f), vs1 = v_setall_f32(0.f);
                        vfloat32x4 vs2 = v_setall_f32(0.f), vs3 = v_setall_f32(0.f);

                        for( k = 0; k < vecsize; k += 4 )
                        {
                            vfloat32x4 v = v_load(sptr + k);
                            vs0 += v*v_load_aligned(wptr + k);
                            vs1 += v*v_load_aligned(wptr + wstep + k);
                            vs2 += v*v_load_aligned(wptr + wstep*2 + k);
                            vs3 += v*v_load_aligned(wptr + wstep*3 + k);
                        }

                        vfloat32x4 s = v_reduce_sum4(vs0, vs1, vs2, vs3);
                        s += v_load(biasptr + i);
                        v_store(dptr + i, s);
                    }
            #endif

                    for( ; i < nw; i++, wptr += wstep )
                    {
                        float s0=biasptr[i];

                        for( k = 0; k < vecsize; k++ )
                        {
                            float v = sptr[k];
                            s0 += v*wptr[k];
                        }
                        dptr[i] = s0;
                    }
                }
                ofs += nw;
            }
        }

        const Mat *srcMat_, *weights_, *biasMat_;
        Mat* dstMat_;
        int nstripes_;
        bool useAVX2_;
    };

    void forward(std::vector<Mat*> &input, std::vector<Mat> &output, std::vector<Mat> &)
    {
        int axisCan = clamp(axis, input[0]->dims);
        int outerSize = input[0]->total(0, axisCan);

        for (size_t i = 0; i < input.size(); i++)
        {
            Mat srcMat = input[i]->reshape(1, outerSize);
            Mat dstMat = output[i].reshape(1, outerSize);

            const int nstripes = getNumThreads();
            FullConnected fconn(srcMat, weightsMat, biasMat, dstMat, nstripes);
            parallel_for_(Range(0, nstripes), fconn, nstripes);
        }
    }

    virtual Ptr<BackendNode> initHalide(const std::vector<Ptr<BackendWrapper> > &inputs)
    {
#ifdef HAVE_HALIDE
        int inW, inH, inC, inN, outC = blobs[0].size[0];
        Halide::Buffer<float> inputBuffer = halideBuffer(inputs[0]);
        getCanonicalSize(inputBuffer, &inW, &inH, &inC, &inN);
        auto weights = wrapToHalideBuffer(blobs[0], {inW, inH, inC, outC});

        Halide::Var x("x"), y("y"), c("c"), n("n");
        Halide::Func top = (name.empty() ? Halide::Func() : Halide::Func(name));
        Halide::RDom r(0, inW, 0, inH, 0, inC);
        Halide::Expr topExpr = sum(inputBuffer(r.x, r.y, r.z, n) *
                                   weights(r.x, r.y, r.z, c));
        if (bias)
        {
            Halide::Buffer<float> bias = wrapToHalideBuffer(blobs[1], {outC});
            topExpr += bias(c);
        }
        top(x, y, c, n) = topExpr;
        return Ptr<BackendNode>(new HalideBackendNode(top));
#endif  // HAVE_HALIDE
        return Ptr<BackendNode>();
    }

    virtual void applyHalideScheduler(Ptr<BackendNode>& node,
                                      const std::vector<Mat*> &inputs,
                                      const std::vector<Mat> &outputs) const
    {
#ifdef HAVE_HALIDE
        int outW, outH, outC, outN;
        getCanonicalSize(outputs[0].size, &outW, &outH, &outC, &outN);

        Halide::Var x("x"), y("y"), c("c"), n("n"), co("co"), ci("ci"), tile("tile");
        Halide::Func& top = node.dynamicCast<HalideBackendNode>()->funcs.back();

        if (outC + outN == 1)
            return;

        if (outC > 8)
          top.split(c, co, ci, 8)
             .fuse(x, y, tile).fuse(co, tile, tile).fuse(n, tile, tile)
             .parallel(tile)
             .vectorize(ci, 8);
        else
          top.fuse(x, y, tile).fuse(c, tile, tile).fuse(n, tile, tile)
             .parallel(tile);
#endif  // HAVE_HALIDE
    }

    virtual int64 getFLOPS(const std::vector<MatShape> &inputs,
                           const std::vector<MatShape> &outputs) const
    {
        (void)inputs; // suppress unused variable warning
        long flops = 0;

        int innerSize = blobs[0].size[1];
        for(int i = 0; i < outputs.size(); i++)
        {
            flops += 3*innerSize*total(outputs[i]);
        }

        return flops;

    }

    bool bias;
    Mat weightsMat, biasMat;
};

Ptr<InnerProductLayer> InnerProductLayer::create(const LayerParams& params)
{
    return Ptr<InnerProductLayer>(new FullyConnectedLayerImpl(params));
}

}
}
