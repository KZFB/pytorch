#include <torch/csrc/jit/tensorexpr/ir.h>

#include <torch/csrc/jit/tensorexpr/tensor.h>

#include <c10/util/irange.h>

namespace torch {
namespace jit {
namespace tensorexpr {

static Dtype ChooseDtype(const Dtype& buffer_dtype, const Dtype& index_dtype) {
  return Dtype(buffer_dtype, index_dtype.lanes());
}

static Dtype dtypeOfIndices(const std::vector<ExprPtr>& indices) {
  if (!indices.size()) {
    // Return something so we can handle scalar buffers.
    return kInt;
  }
  return indices.at(0)->dtype();
}

void castIndicesToInts(std::vector<ExprPtr>& indices) {
  // Cast all indices to either Int or Long
  auto index_dtype = ScalarType::Int;
  for (auto& index : indices) {
    if (index->dtype().scalar_type() == ScalarType::Long) {
      // If any of the indexes is Long, cast all of them to Long
      index_dtype = ScalarType::Long;
      break;
    }
  }

  for (auto& index : indices) {
    const Dtype& dt = index->dtype();
    if (c10::isIntegralType(dt.scalar_type(), true) &&
        dt.scalar_type() != index_dtype) {
      index = alloc<Cast>(Dtype(index_dtype, dt.lanes()), index);
    }
  }
}

Load::Load(Dtype dtype, BufPtr buf, std::vector<ExprPtr> indices)
    : ExprNodeBase(dtype), buf_(buf), indices_(std::move(indices)) {
  castIndicesToInts(indices_);
}

Load::Load(BufPtr buf, const std::vector<ExprPtr>& indices)
    : Load(ChooseDtype(buf->dtype(), dtypeOfIndices(indices)), buf, indices) {}

ExprHandle Load::make(
    Dtype dtype,
    const BufHandle& buf,
    const std::vector<ExprHandle>& indices) {
  return ExprHandle(
      alloc<Load>(dtype, buf.node(), ExprHandleVectorToExprVector(indices)));
}

ExprHandle Load::make(
    const BufHandle& buf,
    const std::vector<ExprHandle>& indices) {
  return Load::make(buf.dtype(), buf, indices);
}

Store::Store(BufPtr buf, std::vector<ExprPtr> indices, ExprPtr value)
    : buf_(buf), indices_(std::move(indices)), value_(value) {
  castIndicesToInts(indices_);
}

StorePtr Store::make(
    const BufHandle& buf,
    const std::vector<ExprHandle>& indices,
    const ExprHandle& value) {
  return alloc<Store>(
      buf.node(), ExprHandleVectorToExprVector(indices), value.node());
}

ExprPtr flatten_index(
    const std::vector<ExprPtr>& dims,
    const std::vector<ExprPtr>& indices) {
  // Handle already flattened indices first
  if (indices.size() == 1) {
    return indices[0];
  }

  size_t ndim = dims.size();
  if (ndim != indices.size()) {
    throw malformed_input("dimensions mismatch in flatten_index");
  }
  if (ndim == 0) {
    return alloc<LongImm>(0);
  }
  std::vector<ExprPtr> strides(ndim);
  // stride[i] = stride[i+1]*dims[i+1], i < ndim-1
  // stride[i] = 1,                     i = ndim-1
  strides[ndim - 1] = immLike(dims[ndim - 1], 1);
  for (size_t i = 1; i < ndim; i++) {
    strides[ndim - 1 - i] = alloc<Mul>(strides[ndim - i], dims[ndim - i]);
  }

  ExprPtr total_index = immLike(indices[0], 0);
  for (const auto i : c10::irange(ndim)) {
    total_index = alloc<Add>(total_index, alloc<Mul>(indices[i], strides[i]));
  }
  return total_index;
}

Dtype Intrinsics::IntrinsicsDtype(IntrinsicsOp op_type, Dtype dt1) {
  if (op_type == kIsNan) {
    return dt1.cloneWithScalarType(ScalarType::Int);
  }
  // TODO: check the op_type and make a real decision
  return dt1;
}

Dtype Intrinsics::IntrinsicsDtype(IntrinsicsOp op_type, Dtype dt1, Dtype dt2) {
  // TODO: check the op_type and make a real decision
  return dt1;
}

Dtype Intrinsics::IntrinsicsDtype(
    IntrinsicsOp op_type,
    const std::vector<ExprPtr>& params) {
  // TODO: check the op_type and make a real decision
  // Doesnt this fail with kRand?
  if (params.size() == 0) {
    throw malformed_input("invalid params in Intrinsics");
  } else if (params.size() == 1) {
    return IntrinsicsDtype(op_type, params[0]->dtype());
  } else if (params.size() == 2) {
    return IntrinsicsDtype(op_type, params[0]->dtype(), params[1]->dtype());
  }
  return params[0]->dtype();
}

int Intrinsics::OpArgCount(IntrinsicsOp op_type) {
  switch (op_type) {
    case kSin:
    case kCos:
    case kTan:
    case kAsin:
    case kAcos:
    case kAtan:
    case kSinh:
    case kCosh:
    case kTanh:
    case kSigmoid:
    case kExp:
    case kExpm1:
    case kAbs:
    case kLog:
    case kLog2:
    case kLog10:
    case kLog1p:
    case kErf:
    case kErfc:
    case kSqrt:
    case kRsqrt:
    case kCeil:
    case kFloor:
    case kRound:
    case kTrunc:
    case kFrac:
    case kLgamma:
    case kIsNan:
      return 1;
    case kRand:
      return 0;
    case kAtan2:
    case kFmod:
    case kPow:
    case kRemainder:
      return 2;
    default:
      throw std::runtime_error("invalid op_type: " + c10::to_string(op_type));
  }
}

ExternalCallPtr ExternalCall::make(
    BufHandle buf,
    const std::string& func_name,
    const std::vector<BufHandle>& buf_args,
    const std::vector<ExprHandle>& args) {
  std::vector<BufPtr> buf_arg_nodes;
  buf_arg_nodes.reserve(buf_args.size());
  for (const BufHandle& buf_arg : buf_args) {
    buf_arg_nodes.push_back(buf_arg.node());
  }
  return alloc<ExternalCall>(
      buf.node(), func_name, buf_arg_nodes, ExprHandleVectorToExprVector(args));
}

std::vector<ExprPtr> ExprHandleVectorToExprVector(
    const std::vector<ExprHandle>& v) {
  std::vector<ExprPtr> result(v.size());
  for (const auto i : c10::irange(v.size())) {
    result[i] = v[i].node();
  }
  return result;
}

std::vector<ExprHandle> ExprVectorToExprHandleVector(
    const std::vector<ExprPtr>& v) {
  std::vector<ExprHandle> result(v.size());
  for (const auto i : c10::irange(v.size())) {
    result[i] = ExprHandle(v[i]);
  }
  return result;
}

std::vector<VarPtr> VarHandleVectorToVarVector(
    const std::vector<VarHandle>& v) {
  std::vector<VarPtr> result(v.size());
  for (const auto i : c10::irange(v.size())) {
    result[i] = v[i].node();
  }
  return result;
}

std::vector<VarHandle> VarVectorToVarHandleVector(
    const std::vector<VarPtr>& v) {
  std::vector<VarHandle> result(v.size());
  for (const auto i : c10::irange(v.size())) {
    result[i] = VarHandle(v[i]);
  }
  return result;
}

bool immediateIsNegative(ExprPtr e) {
#define TYPE_CASE(Type, Name)                \
  if (Name##ImmPtr imm = to<Name##Imm>(e)) { \
    return imm->value() < 0;                 \
  }
  AT_FORALL_SCALAR_TYPES_AND(Half, TYPE_CASE);
#undef TYPE_CASE
  return false;
}

} // namespace tensorexpr
} // namespace jit
} // namespace torch
