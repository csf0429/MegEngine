/**
 * \file dnn/src/rocm/convolution/backward_filter/matmul.cpp
 * MegEngine is Licensed under the Apache License, Version 2.0 (the "License")
 *
 * Copyright (c) 2014-2021 Megvii Inc. All rights reserved.
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT ARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#include "./algo.h"
#include "src/rocm/convolution/helper.h"
#include "src/rocm/convolution/im2col.h.hip"
#include "src/rocm/utils.h"

using namespace megdnn;
using namespace rocm;

bool ConvolutionBackwardFilterImpl::AlgoMatmul::is_available(
        const SizeArgs& args) const {
    auto&& fm = args.grad_filter_meta;
    return fm.format == Param::Format::NCHW &&
           args.diff_layout->dtype.category() == DTypeCategory::FLOAT &&
           args.opr->param().compute_mode != Param::ComputeMode::FLOAT32 &&
           fm.group == 1 && fm.spatial_ndim == 2;
}

size_t ConvolutionBackwardFilterImpl::AlgoMatmul::get_workspace_in_bytes(
        const SizeArgs& args) const {
    return matmul_get_workspace_bundle(args.as_fwd_args()).total_size_in_bytes();
}

void ConvolutionBackwardFilterImpl::AlgoMatmul::exec(const ExecArgs& args) const {
#define cb(DType)                                        \
    if (args.diff_layout->dtype == DType()) {            \
        using ctype = typename DTypeTrait<DType>::ctype; \
        exec_internal<ctype>(args);                      \
        return;                                          \
    }
    MEGDNN_FOREACH_COMPUTING_DTYPE_FLOAT(cb)
#undef cb

    megdnn_assert_internal(0);
}

template <typename T>
void ConvolutionBackwardFilterImpl::AlgoMatmul::exec_internal(const ExecArgs& args) {
    auto&& fm = args.grad_filter_meta;
    size_t N = args.src_layout->shape[0], IC = fm.icpg, IH = args.src_layout->shape[2],
           IW = args.src_layout->shape[3], OC = fm.ocpg,
           OH = args.diff_layout->shape[2], OW = args.diff_layout->shape[3],
           FH = fm.spatial[0], FW = fm.spatial[1], PH = fm.padding[0],
           PW = fm.padding[1], SH = fm.stride[0], SW = fm.stride[1],
           DH = fm.dilation[0], DW = fm.dilation[1];
    auto stream = hip_stream(args.handle);
    auto wbundle = matmul_get_workspace_bundle(args.as_fwd_args());
    wbundle.set(args.workspace.raw_ptr);
    T* diff_t = static_cast<T*>(wbundle.get(0));
    T* col = static_cast<T*>(wbundle.get(1));
    {
        // transpose diff
        TensorLayout froml({N, OC * OH * OW}, typename DTypeTrait<T>::dtype()),
                tol(froml);
        froml.stride[0] = args.diff_layout->stride[0];
        tol.stride[0] = 1;
        tol.stride[1] = N;
        TensorND from(args.diff_tensor->ptr<T>(), froml), to(diff_t, tol);
        args.handle->relayout_opr()->exec(from, to);
    }
    {
        convolution::im2col<T>(
                args.src_tensor->ptr<T>(), col, N, args.src_tensor->layout.stride[0],
                IC, IH, IW, FH, FW, OH, OW, PH, PW, SH, SW, DH, DW, stream);
    }
    {
        // take gemm grad
        TensorLayout Al({OC, IC * FH * FW}, typename DTypeTrait<T>::dtype()),
                Bl({IC * FH * FW, OH * OW * N}, typename DTypeTrait<T>::dtype()),
                Cl({OC, OH * OW * N}, typename DTypeTrait<T>::dtype());
        TensorND A(args.grad_tensor->ptr<T>(), Al), B(col, Bl), C(diff_t, Cl);
        if (fm.should_flip) {
            A.raw_ptr = wbundle.get(2);
        }
        args.handle->matmul_bT_opr()->exec(C, B, A, Workspace());

        if (fm.should_flip) {
            convolution::flip_filter(
                    args.as_fwd_args(),
                    {static_cast<dt_byte*>(args.grad_tensor->raw_ptr),
                     wbundle.get_size(2)},
                    A.raw_ptr);
        }
    }
}

// vim: syntax=cpp.doxygen
