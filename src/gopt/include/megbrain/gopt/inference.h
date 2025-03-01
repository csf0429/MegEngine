/**
 * \file src/gopt/include/megbrain/gopt/inference.h
 * MegEngine is Licensed under the Apache License, Version 2.0 (the "License")
 *
 * Copyright (c) 2014-2021 Megvii Inc. All rights reserved.
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT ARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#pragma once

#include "megbrain/gopt/framework.h"
#include "megbrain/graph/cg.h"
#include "megbrain/opr/dnn/convolution.h"
#include "megbrain/opr/search_policy/algo_chooser_helper.h"

#if MGB_CUDA
#include <cuda.h>
#endif

namespace mgb {
namespace gopt {

/*!
 * \brief redistribute SharedDeviceTensor oprs
 *
 * Redistribute parameters. For example, ``conv(x, w) * k`` may be replaced
 * by ``conv(x, w*k)``.
 *
 * Usually this pass is used before ParamFusePass.
 */
class ParamRedistributePass final : public Pass {
    class Impl;

public:
    const char* name() const override;

    void apply(OptState& opt) const override;
};

/*!
 * \brief fuse SharedDeviceTensor oprs
 *
 * This would treat all SharedDeviceTensor operators as constant, and
 * replace oprs that only depend on them by the evaluated value at compile
 * time.
 *
 * Usually this pass is used after ParamRedistributePass.
 */
class ParamFusePass final : public Pass {
    class ConstVarPropogateWithSizeCheck;
    class VarNamer;

    size_t m_param_grow_limit = std::numeric_limits<size_t>::max();

public:
    /*!
     * \brief set the limit for max param size growth due to merging
     *
     * Param size may grow if param fusing causes low-rank result (i.e.
     * by broadcasting). Size growth is defined to be the difference
     * between new param size and max size of source oprs that it
     * depends on.
     *
     * This limit is given in bytes
     */
    ParamFusePass& param_grow_limit(size_t val) {
        m_param_grow_limit = val;
        return *this;
    }

    const char* name() const override;

    void apply(OptState& opt) const override;
};

/*!
 * \brief replace the dtype of opr from float32 to float16.
 */
class ConvertF32ToF16Pass : public Pass {
    ThinHashMap<
            Typeinfo*,
            thin_function<OperatorNodeBase*(OperatorNodeBase*, const VarNodeArray&)>>
            m_opr_replace_func;
    VarReplaceCheckFlag m_var_replace_check_flag = VarReplaceCheckFlag::CHECK_ALL;

public:
    const char* name() const override;

    ConvertF32ToF16Pass& set_var_replace_check_flag(VarReplaceCheckFlag flag) {
        m_var_replace_check_flag = flag;
        return *this;
    }

    void apply(OptState& opt) const override;

    static std::unique_ptr<ConvertF32ToF16Pass> make(bool use_f32_comp);
};

/*!
 * \brief convert tensor format to speed up inference on certain devices
 */
class ConvertFormatPass : public Pass {
    ThinHashMap<
            Typeinfo*,
            thin_function<OperatorNodeBase*(OperatorNodeBase*, const VarNodeArray&)>>
            m_opr_replace_func;
    VarReplaceCheckFlag m_var_replace_check_flag = VarReplaceCheckFlag::CHECK_ALL;

public:
    const char* name() const override { return mgb_cstr_log("convert_format_nhwcd4"); }

    ConvertFormatPass& set_var_replace_check_flag(VarReplaceCheckFlag flag) {
        m_var_replace_check_flag = flag;
        return *this;
    }

    void apply(OptState& opt) const override;

    static std::unique_ptr<ConvertFormatPass> make_nhwcd4_converter();
};

/*!
 * \brief convert batch norm to elemwise
 * For inference phase, cudnnbn = scale * (x - mean) / variance + bias
 * In order to make the latter ParamDistributePass + ParamFusePass
 * to do const folding better
 */
class ConvertBatchNormToElemwisePass : public Pass {
public:
    const char* name() const override;
    void apply(OptState& opt) const override;
};

/*!
 * \brief fuse convolution, bias add, relu oprs to a ConvBiasForward opr
 */
class FuseConvBiasNonlinPass : public Pass {
public:
    const char* name() const override;
    void apply(OptState& opt) const override;
};

/*!
 * \brief fuse ConvBias, z oprs to a ConvBiasForward opr
 */
class FuseConvBiasZPass : public Pass {
public:
    const char* name() const override;
    void apply(OptState& opt) const override;
};

/*!
 * \brief fuse preprocess, like pad channel, quint8 to qint8
 */
class FuseNCHW4Int8Preprocess : public Pass {
public:
    const char* name() const override;
    void apply(OptState& opt) const override;
    static std::unique_ptr<FuseNCHW4Int8Preprocess> make();
    using DepType = cg::OperatorNodeProp::DepType;
    using ReaderType = ThinHashMap<
            OperatorNodeBase*, SmallVector<std::pair<OperatorNodeBase*, DepType>>>;

private:
    ThinHashMap<
            Typeinfo*, thin_function<OperatorNodeBase*(
                               OperatorNodeBase*, const VarNodeArray&,
                               SubGraph::Rewriter&, ReaderType&)>>
            m_opr_replace_func;
};

/*!
 * \brief fuse warp perspective and dimshuffle, quint8/uint8 to qint8/float
 */
class FuseWarpPerspectiveDimshufflePass : public Pass {
public:
    const char* name() const override;
    void apply(OptState& opt) const override;
};

/*!
 * \brief fuse deconv and typecvt to a deconv opr
 */
class FuseDeconvCvtPass : public Pass {
public:
    const char* name() const override;
    void apply(OptState& opt) const override;
};

/*!
 * \brief merge all the SharedDeviceTensor oprs into one
 *      MultipleDeviceTensorHolder
 */
class ParamMergePass final : public Pass {
public:
    const char* name() const override;
    void apply(OptState& opt_state) const override;
};

/*!
 * \brief tensor format converter to accelerate inference speed on Nvidia
 * platform
 */
class TensorReformatPass : public Pass {
    //! replace rule for endpoint var of computing graph
    virtual VarNode* on_graph_endpoint_var(
            VarNode* new_var, VarNode* orig_var) const = 0;
    //! insert relayout placeholder
    //! (nchw4->nchw32/nchw32->nchw4/nchw4->chwn4/chwn4->nchw4)
    void insert_pass(OptState& opt) const;
    //! translate relayout placeholder to actual implementation
    void translate_pass(OptState& opt) const;

protected:
    ThinHashMap<
            Typeinfo*,
            thin_function<OperatorNodeBase*(OperatorNodeBase*, const VarNodeArray&)>>
            m_opr_replace_func;
    VarReplaceCheckFlag m_var_replace_check_flag = VarReplaceCheckFlag::CHECK_ALL;
    class RelayoutPlaceholder;
    friend class ShuffleShuffleRemovePass;

public:
    TensorReformatPass& set_var_replace_check_flag(VarReplaceCheckFlag flag) {
        m_var_replace_check_flag = flag;
        return *this;
    }
    void apply(OptState& opt) const override;
};

/*!
 * \brief enable using tensorcore on Turing architecture
 */
class EnableTensorCorePass final : public TensorReformatPass {
    VarNode* on_graph_endpoint_var(VarNode* new_var, VarNode* orig_var) const override;

public:
    const char* name() const override { return mgb_cstr_log("enable_tensorcore"); }
    //! make enable tensorcore opt pass
    static std::unique_ptr<EnableTensorCorePass> make_tensorcore_converter();
};

/*!
 * \brief enable using chwn4 tensor format on Nvidia Platform with compute
 * capability 6.1 or later
 */
class EnableCHWN4Pass final : public TensorReformatPass {
    ThinHashSet<VarNode*> m_varshape_changed;
    VarNode* on_graph_endpoint_var(VarNode* new_var, VarNode* orig_var) const override;

public:
    const char* name() const override { return mgb_cstr_log("enable_chwn4"); }

    //! make nchw4 -> chwn4 converter opt pass
    static std::unique_ptr<EnableCHWN4Pass> make_chwn4_converter();
};

/*!
 * \brief convert tensor format to nchw4 to speed up inference on CUDA
 */
class EnableNCHW4Pass final : public TensorReformatPass {
    VarNode* on_graph_endpoint_var(VarNode* new_var, VarNode* orig_var) const override;

public:
    const char* name() const override { return mgb_cstr_log("tensor_format_nchw4"); }

    //! make nchw -> nchw4 converter opt pass
    static std::unique_ptr<EnableNCHW4Pass> make_nchw4_converter();
};

/*!
 * \brief convert tensor format to nchwxx to speed up inference on certain
 * devices
 */
class EnableNchwxxPass : public TensorReformatPass {
    std::string m_name = "tensor_format_nchwxx";
    size_t m_pack_c_size;
    VarNode* on_graph_endpoint_var(VarNode* new_var, VarNode* orig_var) const override;

public:
    EnableNchwxxPass(size_t pack_c_size) : m_pack_c_size(pack_c_size) {}

    //! the flag for conv to transform to nchwxx
    enum class TransType {
        TRANS_PURE_NCHWXX,    //!< weight and src all trans to nchwxx
        TRANS_HYBIRD_NCHWXX,  //!< input is nchw, output is nchwxx
        TRANS_NONE,           //!< no need trans
    };
    const char* name() const override { return mgb_cstr_log(m_name.c_str()); }
    void set_name(std::string in_name) { m_name = in_name; }

    void fill_opr_convert_fun(size_t pack_c_size);

    //! make nchw -> nchwxx converter opt pass, pack_c_size is the x, like
    //! 4,8,16
    static std::unique_ptr<EnableNchwxxPass> make_nchwxx_converter(size_t pack_c_size);
};

/*!
 * \brief convert tensor format from nchw44 to nchw44_dot to speed up
 * inference on armv8.2
 */
class EnableNchw44DotPass final : public EnableNchwxxPass {
    std::string m_name = "tensor_format_nchw44_dot";
    VarNode* on_graph_endpoint_var(VarNode* new_var, VarNode* orig_var) const override;

public:
    EnableNchw44DotPass() : EnableNchwxxPass(4) {}
    //! make nchw44 -> nchw44_dot converter opt pass
    static std::unique_ptr<EnableNchw44DotPass> make_nchw44_dot_converter();
};

struct OptimizeForInferenceOptions : cg::GraphCommonOptimizeOptions {
    uint64_t serialize() {
        uint64_t ret = 0;
        ret |= (uint64_t)layout_transform << 32;
        if (f16_io_f32_comp)
            ret |= 1u;
        if (f16_io_comp)
            ret |= 1u << 1;
        if (fuse_conv_bias_nonlinearity)
            ret |= 1u << 2;
        if (fuse_conv_bias_with_z)
            ret |= 1u << 3;
        if (weight_preprocess)
            ret |= 1u << 4;
        if (fuse_preprocess)
            ret |= 1u << 5;
        return ret;
    }

    static OptimizeForInferenceOptions deserialize(uint64_t buf) {
        OptimizeForInferenceOptions ret;
        ret.f16_io_f32_comp = buf & 1u;
        ret.f16_io_comp = buf & 1u << 1;
        ret.fuse_conv_bias_nonlinearity = buf & 1u << 2;
        ret.fuse_conv_bias_with_z = buf & 1u << 3;
        ret.weight_preprocess = buf & 1u << 4;
        ret.fuse_preprocess = buf & 1u << 5;
        ret.layout_transform = (LayoutTransform)(buf >> 32);
        return ret;
    }
};

/**
 * \brief graph level tuning options.
 * The GraphTuningOptions is corresponding to graph level optimizations.
 * Unlike the GraphCommonOptimizeOptions, these optimization options are
 * usually target-dependent and profiling based, and the optimize usually should take
 * place during runtime. The GraphTuningOptions includes layout optimization etc, more
 * optimize options will be introduced in the future.
 */
struct GraphTuningOptions {
    enum class Target : uint32_t {
        UNSPEC = 0,  ///< unspecific device target
        CUDA = 1,    ///< CUDA device, usually refer to GPU devices of Nvidia
        X86 = 2,     ///< x86 cpu
        ARM = 3,     ///< arm cpu
        OPENCL = 4,  ///< opencl, usually run on mobile devices
    };
    Target target;
    bool layout_transform = false;  ///< whether to enable graph level
                                    ///< tuning for layouts of tensors
#define SET(n)                          \
    GraphTuningOptions& enable_##n() {  \
        n = true;                       \
        return *this;                   \
    }                                   \
    GraphTuningOptions& disable_##n() { \
        n = false;                      \
        return *this;                   \
    }                                   \
    bool has_set_##n() const { return n == true; }
    SET(layout_transform);
#undef SET
};

/*!
 * \brief optimize a computing graph for inference
 *
 * This function applies a set of predefined optimizer passes to optimize
 * for inference. It assumes all params are constant.
 */
SymbolVarArray optimize_for_inference(
        const SymbolVarArray& dest_vars, const OptimizeForInferenceOptions& opt = {});

/*!
 * \brief optimize the layout selection for a computing graph
 *
 * The layout selection optimizers are target-dependent. And this function
 * applies a set of predefined optimizer passes designed for specific
 * device.      */
SymbolVarArray layout_transform(
        const SymbolVarArray& dest_vars,
        GraphTuningOptions::Target target = GraphTuningOptions::Target::UNSPEC);

/*!
 * \brief modify execution strategy for oprs with multiple
 *      algorithms
 *
 * This would modify the operators inplace. It can be used for implement
 * the fast-run mode.
 */
void modify_opr_algo_strategy_inplace(
        const VarNodeArrayView& dest_vars,
        opr::mixin::AlgoChooserHelper::ExecutionPolicy::Strategy strategy);

/*!
 * \brief enable PROFILE execution strategy for oprs with multiple
 *      algorithms
 *
 * This would modify the operators inplace. It is usually used to implement
 * the fast-run mode.
 *
 * You may want to implement TimedFuncInvoker::ForkExecImpl and/or
 * PersistentCache for better performance in an SDK.
 */
void enable_opr_algo_profiling_inplace(const VarNodeArrayView& dest_vars);

/*!
 * \brief enable opr try profiling cache first, if failed, fallback to
 * heuristic
 *
 * This would modify the operators inplace. It is usually used to enable
 * fast-run's cache when fast-run mode is disabled.
 *
 * You may want to implement TimedFuncInvoker::ForkExecImpl and/or
 * PersistentCache for better performance in an SDK.
 */
void enable_opr_use_profiling_cache_inplace(const VarNodeArrayView& dest_vars);

/*!
 * \brief set workspace_limit for execution strategy for oprs with multiple
 *      algorithms
 *
 * This would modify the operators inplace. It is usually used to implement
 * the fast-run mode.
 *
 * \warning It will influence the default algo choosed, and maybe slower but
 * save memory.
 */
void set_opr_algo_workspace_limit_inplace(
        const VarNodeArrayView& dest_vars, size_t workspace_limit);

/*!
 * \brief transform consecutive tensor shuffle operations into
 * one shuffle operator or a Nop
 *
 * Transform shuffle/typecvt operator chains to one shuffle operator and
 * multiple typecvt operators. For example, a operator chain like
 * reformat(nchw -> nchw4), asQuantizedS8, reformat(nchw4 -> nchw),
 * asFloat32, would be changed to asQuantizedS8, asFloat32. Since the
 * reciprocal reformat operations have been removed from the operator chain,
 * the computation can be speed up with fewer memory operations. This pass
 * is usually used after EnableTensorCorePass, TensorRTReplacePass.
 */
class ShuffleShuffleRemovePass final : public Pass {
    class Impl;

public:
    const char* name() const override;
    void apply(OptState& opt) const override;
};

#if CUDA_VERSION >= 10020
class FoldingConvBiasDimshufflePass final : public Pass {
public:
    const char* name() const override;
    void apply(OptState& opt) const override;
};

class FoldingConvBiasTypecvtPass final : public Pass {
public:
    const char* name() const override;
    void apply(OptState& opt) const override;
};
#endif

/*!
 * \brief padding channel to enable fast int8/int4 support
 * assume input network is built in NCHW tensor format
 */
class PaddingChannelPass final : public Pass {
public:
    const char* name() const override;
    void apply(OptState& opt) const override;
};

/*!
 * \brief convert tensor format to nchw64 to enable tensorcore int4 on CUDA
 * we assume that the input network is in NCHW layout
 */
class EnableNCHW64Pass final : public TensorReformatPass {
public:
    using Format = opr::ConvBias::Param::Format;
    const char* name() const override { return mgb_cstr_log("tensor_format_nchw64"); }

    //! make nchw -> nchw64 converter opt pass
    static std::unique_ptr<EnableNCHW64Pass> make_nchw64_converter();

private:
    ThinHashMap<OperatorNodeBase*, Format> m_opr_format_map;

    VarNode* on_graph_endpoint_var(VarNode* new_var, VarNode* orig_var) const override;
};

}  // namespace gopt
}  // namespace mgb

// vim: syntax=cpp.doxygen foldmethod=marker foldmarker=f{{{,f}}}
